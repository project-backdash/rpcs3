#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_punish.h"

#include <cstdint>
#include <optional>

namespace
{
	using u32 = std::uint32_t;
	using pbd::practice::frames::calculator;
	using pbd::practice::frames::contact;
	using pbd::practice::frames::readiness_reading;
	using pbd::practice::frames::ready_state;
	namespace punish = pbd::practice::punish;

	using pbd::test::check;

	// Hit levels as the contact check reads them (pbd::practice::frames::hit_level_text).
	constexpr u32 high = 0x0512, mid = 0x0217, low = 0x010f;

	std::optional<readiness_reading> free_on(u32 cursor, ready_state state = ready_state::stand)
	{
		readiness_reading out;
		out.free = cursor;
		out.state = state;
		return out;
	}

	// Fighter 0 is the player and fighter 1 the dummy. The dummy's attack (startup 15, free on its cursor 40) starts on frame
	// 100 and is blocked on frame 114; the player's block reaction from frame 115 is free on its cursor 12. The player can act
	// from frame 126 and the dummy from frame 139: -13 on block. An input taken on a free frame starts its move on the next
	// frame, so the player's fastest attack starts on frame 127.
	struct blocked_attack
	{
		calculator frames;
		punish::watch watch;

		explicit blocked_attack(u32 dummy_free = 40, ready_state dummy_state = ready_state::stand)
		{
			frames.commit_reading(0, 900, 20, 1, free_on(1), false);
			frames.commit_reading(1, 120, 100, 1, free_on(dummy_free, dummy_state), true, 16, 15);
			frames.applied_contact(1, contact::block, 114);
		}
		std::optional<punish::result> at(std::int64_t now)
		{
			return watch.boundary(frames.latest[0], frames.latest[1], now);
		}
		void reaction()
		{
			frames.commit_reading(0, 220, 115, 1, free_on(12), false);
		}
		// The player's attack with `startup`, its input taken on frame `taken`: it starts on the next frame and connects on its
		// first active frame.
		void start(u32 taken, u32 startup)
		{
			frames.commit_reading(0, 130 + startup, taken + 1, 1, free_on(30), true, startup + 1, startup);
		}
		void connect(u32 taken, u32 startup, contact kind, u32 level = 0)
		{
			frames.applied_contact(0, kind, taken + startup, pbd::practice::frames::outcome_label::none, level);
		}
		void attack(u32 taken, u32 startup, contact kind, u32 level = 0)
		{
			start(taken, startup);
			connect(taken, startup, kind, level);
		}
	};
} // namespace

PBD_TEST(PbdPracticePunish, ContactFrameIsKept)
{
	blocked_attack sample;
	check(sample.frames.latest[1].impact == 114 && sample.frames.latest[1].contacted, "the contact keeps the frame it was applied on");
	sample.reaction();
	check(sample.frames.latest[1].advantage == -13 && sample.frames.latest[1].attacker_ready == 139 && sample.frames.latest[1].defender_ready == 126,
		"the block reaction gives the dummy's -13");
	sample.frames.commit_reading(1, 121, 150, 1, free_on(30), true, 12, 10);
	check(!sample.frames.latest[1].impact && !sample.frames.latest[1].contacted, "a new attack has no contact yet");
}

PBD_TEST(PbdPracticePunish, InTimeIsPunished)
{
	blocked_attack sample;
	check(!sample.at(114) && !sample.watch.waiting(), "the gap waits for the block reaction's numbers");
	sample.reaction();
	check(!sample.at(115) && sample.watch.waiting(), "-13 on block is punishable");
	sample.attack(126, 10, contact::hit);
	const auto judged = sample.at(136);
	check(judged && judged->outcome == punish::verdict::punished && judged->frames == 3 && judged->startup == 10u && judged->advantage == -13,
		"i10 taken on the first free frame starts on frame 127 and lands on 136, three frames before the dummy's first free frame");
	check(punish::text(*judged) == "PUNISHED\ti10 on -13, 3 frames to spare");
	check(!sample.watch.waiting() && !sample.at(137) && !sample.at(140), "each blocked attack is judged once");
}

PBD_TEST(PbdPracticePunish, TheLastFrameStillPunishes)
{
	blocked_attack sample;
	sample.reaction();
	sample.at(115);
	sample.attack(126, 13, contact::hit);
	const auto judged = sample.at(139);
	check(judged && judged->outcome == punish::verdict::punished && judged->frames == 0,
		"i13 on -13 lands on the dummy's first free frame, before the guard it takes there holds");
}

// The dummy is free on frame 136: -10, the first gap a jab punishes.
PBD_TEST(PbdPracticePunish, AnI10PunishesMinusTen)
{
	{
		blocked_attack sample(37);
		sample.reaction();
		check(sample.frames.latest[1].advantage == -10 && !sample.at(115) && sample.watch.waiting(), "-10 on block is punishable");
		sample.attack(126, 10, contact::hit, high);
		const auto judged = sample.at(136);
		check(judged && judged->outcome == punish::verdict::punished && judged->frames == 0 && !judged->blocked,
			"i10 taken on the first free frame lands on frame 136, the dummy's first free frame");
		check(punish::text(*judged) == "PUNISHED\ti10 on -10, with no frame to spare");
	}
	{
		blocked_attack sample(37);
		sample.reaction();
		sample.at(115);
		sample.attack(126, 11, contact::block, mid);
		const auto judged = sample.at(137);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1 && judged->blocked,
			"i11 taken on the first free frame lands on frame 137, when the dummy's guard holds");
		check(punish::text(*judged) == "TOO SLOW\ti11 can't punish -10: use i10 or faster");
	}
}

// A press made during the block stun is held by the game and taken on the player's first free frame, the earliest frame an
// attack can be taken on, so it punishes as one pressed right then.
PBD_TEST(PbdPracticePunish, ABufferedPressPunishes)
{
	blocked_attack sample(37);
	sample.reaction();
	for (std::int64_t frame = 115; frame <= 126; ++frame)
		check(!sample.at(frame) && sample.watch.waiting(), "the block stun plays out whatever was pressed during it");
	sample.start(126, 10);
	for (std::int64_t frame = 127; frame < 136; ++frame)
		check(!sample.at(frame) && sample.watch.waiting(), "the attack has not connected yet");
	sample.connect(126, 10, contact::hit, high);
	const auto judged = sample.at(136);
	check(judged && judged->outcome == punish::verdict::punished && judged->frames == 0, "the held i10 lands on the dummy's first free frame");
}

PBD_TEST(PbdPracticePunish, OneFrameLateIsLate)
{
	{
		blocked_attack sample(37);
		sample.reaction();
		sample.at(115);
		sample.attack(127, 10, contact::hit, high);
		const auto judged = sample.at(137);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1 && !judged->blocked,
			"i10 taken one frame after the first free frame lands on frame 137");
		check(punish::text(*judged) == "TOO LATE\ti10 on -10, 1 frame late");
	}
	{
		blocked_attack sample(37);
		sample.reaction();
		sample.at(115);
		sample.attack(127, 10, contact::block, high);
		const auto judged = sample.at(137);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1 && judged->blocked, "a guarding dummy blocks it");
	}
}

PBD_TEST(PbdPracticePunish, LateAndSlowAttacks)
{
	{
		blocked_attack sample;
		sample.reaction();
		sample.at(115);
		sample.attack(130, 10, contact::hit);
		const auto judged = sample.at(141);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1 && !judged->blocked,
			"i10 taken four frames late lands on frame 140, once the dummy guards");
	}
	{
		blocked_attack sample;
		sample.reaction();
		sample.at(115);
		sample.attack(126, 15, contact::hit);
		const auto judged = sample.at(141);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 2, "i15 taken on the first free frame lands two frames late");
		check(punish::text(*judged) == "TOO SLOW\ti15 can't punish -13: use i13 or faster");
	}
	{
		blocked_attack sample;
		sample.reaction();
		sample.at(115);
		sample.attack(130, 12, contact::block);
		const auto judged = sample.at(142);
		check(judged && judged->outcome == punish::verdict::late && judged->blocked && judged->frames == 3, "a guarding dummy blocks a late attack");
	}
}

// The dummy's low leaves it free crouching on frame 138: -12 FC. Whether it stays crouching to guard a low or stands to guard
// a mid, its guard holds from frame 139.
PBD_TEST(PbdPracticePunish, ACrouchingDummyGuardsEveryLevelFromTheSameFrame)
{
	{
		blocked_attack sample(39, ready_state::crouch);
		sample.reaction();
		check(sample.frames.latest[1].advantage == -12 && sample.frames.latest[1].attacker_state == ready_state::crouch, "the low leaves the dummy at -12 FC");
		check(!sample.at(115) && sample.watch.waiting());
		sample.attack(126, 12, contact::hit, mid);
		const auto judged = sample.at(138);
		check(judged && judged->outcome == punish::verdict::punished && judged->frames == 0 && judged->crouching,
			"a WS i12 mid taken on the first free frame lands on frame 138, the dummy's first free frame");
		check(punish::text(*judged) == "PUNISHED\ti12 on -12 FC, with no frame to spare");
	}
	{
		blocked_attack sample(39, ready_state::crouch);
		sample.reaction();
		sample.at(115);
		sample.attack(126, 13, contact::block, mid);
		const auto judged = sample.at(139);
		check(judged && judged->outcome == punish::verdict::late && judged->blocked && judged->frames == 1, "an i13 mid lands once the dummy stands and guards");
		check(punish::text(*judged) == "TOO SLOW\ti13 can't punish -12 FC: use i12 or faster");
	}
	{
		blocked_attack sample(39, ready_state::crouch);
		sample.reaction();
		sample.at(115);
		sample.attack(126, 13, contact::block, low);
		const auto judged = sample.at(139);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1, "the crouching guard stops a low on frame 139");
		check(punish::text(*judged) == "TOO SLOW\ti13 can't punish -12 FC: use i12 or faster");
	}
	{
		blocked_attack sample(38, ready_state::crouch);
		sample.reaction();
		sample.at(115);
		sample.attack(127, 11, contact::block, mid);
		const auto judged = sample.at(138);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1, "a WS i11 taken one frame late on -11 FC is late");
		check(punish::text(*judged) == "TOO LATE\ti11 on -11 FC, 1 frame late");
	}
}

PBD_TEST(PbdPracticePunish, AStandingDummyGuardsEveryLevelFromTheSameFrame)
{
	{
		blocked_attack sample;
		sample.reaction();
		sample.at(115);
		sample.attack(126, 14, contact::hit, mid);
		const auto judged = sample.at(140);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1 && !judged->crouching);
		check(punish::text(*judged) == "TOO SLOW\ti14 can't punish -13: use i13 or faster");
	}
	{
		blocked_attack sample;
		sample.reaction();
		sample.at(115);
		sample.attack(126, 13, contact::hit, high);
		const auto judged = sample.at(139);
		check(judged && judged->outcome == punish::verdict::punished && judged->frames == 0, "i13 high on -13 lands on the dummy's first free frame");
	}
	{
		blocked_attack sample;
		sample.reaction();
		sample.at(115);
		sample.attack(126, 14, contact::block, low);
		const auto judged = sample.at(140);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 1, "a standing dummy that crouches to guard a low is no slower");
		check(punish::text(*judged) == "TOO SLOW\ti14 can't punish -13: use i13 or faster");
	}
}

PBD_TEST(PbdPracticePunish, ConflictingReadingsGiveNoVerdict)
{
	blocked_attack sample;
	sample.reaction();
	sample.at(115);
	sample.attack(126, 10, contact::block);
	check(!sample.at(136) && !sample.watch.waiting(), "an attack blocked while the dummy could not guard yet is no verdict");
}

PBD_TEST(PbdPracticePunish, SafeAttacksOpenNoGap)
{
	blocked_attack sample(28);
	sample.reaction();
	check(sample.frames.latest[1].advantage == -1, "the dummy is free on frame 127");
	check(!sample.at(115) && !sample.watch.waiting(), "-1 on block is safe");
	sample.attack(126, 10, contact::hit);
	check(!sample.at(136), "a hit after a safe attack is no punish");
}

PBD_TEST(PbdPracticePunish, AnUnusedGapExpires)
{
	blocked_attack sample;
	sample.reaction();
	sample.at(115);
	check(!sample.at(139 + punish::expiry_frames) && sample.watch.waiting(), "the gap stays for a while after the dummy can act");
	check(!sample.at(140 + punish::expiry_frames) && !sample.watch.waiting(), "then it is over");
	sample.attack(210, 10, contact::hit);
	check(!sample.at(220), "a hit long after is a new situation");
}

PBD_TEST(PbdPracticePunish, TheDummysNextContactEndsTheGap)
{
	blocked_attack sample;
	sample.reaction();
	sample.at(115);
	sample.frames.commit_reading(1, 121, 150, 1, free_on(30), true, 12, 10);
	check(!sample.at(151) && sample.watch.waiting(), "the dummy's next attack has not connected yet");
	sample.frames.applied_contact(1, contact::hit, 159);
	check(!sample.at(160) && !sample.watch.waiting(), "the dummy hitting the player ends the gap");
	sample.attack(170, 10, contact::hit);
	check(!sample.at(180), "the player's hit after that is no punish");
}

PBD_TEST(PbdPracticePunish, ACounterThatWentBackClosesTheGap)
{
	blocked_attack sample;
	sample.reaction();
	sample.at(115);
	check(!sample.at(110) && !sample.watch.waiting(), "a restore to before the block leaves nothing to judge");
	check(!sample.at(116) && !sample.watch.waiting(), "and the same blocked attack is not taken up again");
	sample.watch.reset();
	check(!sample.at(116) && sample.watch.waiting(), "a restart reads the fight afresh");
}

PBD_TEST(PbdPracticePunish, AThrowEndsTheGapWithoutAVerdict)
{
	blocked_attack sample;
	sample.reaction();
	sample.at(115);
	// The throw's reach starts on frame 127 and catches; the thrower's hold, a move of throw level, starts on frame 139.
	sample.frames.commit_reading(0, 300, 127, 1, free_on(20), true, 12, 12);
	check(!sample.at(128) && sample.watch.waiting(), "the throw has not caught yet");
	sample.frames.commit_reading(0, 301, 139, 1, free_on(60), true);
	sample.frames.throw_connected(0);
	check(!sample.at(140) && !sample.watch.waiting(), "a throw has no contact frame to judge");
	sample.attack(160, 10, contact::hit);
	check(!sample.at(170), "a hit after the throw is no punish");
}

PBD_TEST(PbdPracticePunish, AnEarlierThrowIsNoAnswer)
{
	calculator frames;
	punish::watch watch;
	frames.commit_reading(0, 301, 20, 1, free_on(60), true);
	frames.throw_connected(0);
	frames.commit_reading(1, 120, 100, 1, free_on(40), true, 16, 15);
	frames.applied_contact(1, contact::block, 114);
	frames.commit_reading(0, 220, 115, 1, free_on(12), false);
	check(!watch.boundary(frames.latest[0], frames.latest[1], 115) && watch.waiting() && !watch.boundary(frames.latest[0], frames.latest[1], 116) &&
			  watch.waiting(),
		"the player's throw from before the block stays on screen and ends nothing");
	frames.commit_reading(0, 140, 127, 1, free_on(30), true, 11, 10);
	frames.applied_contact(0, contact::hit, 136);
	const auto judged = watch.boundary(frames.latest[0], frames.latest[1], 136);
	check(judged && judged->outcome == punish::verdict::punished && judged->frames == 3, "the punish after it is judged");
}
