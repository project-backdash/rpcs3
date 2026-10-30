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
	namespace punish = pbd::practice::punish;

	using pbd::test::check;

	std::optional<readiness_reading> free_on(u32 cursor)
	{
		readiness_reading out;
		out.free = cursor;
		return out;
	}

	// Fighter 0 is the player and fighter 1 the dummy. The dummy's attack (startup 15, free on its cursor 40) starts on frame
	// 100 and is blocked on frame 114; the player's block reaction from frame 115 is free on its cursor 12. The player can act
	// from frame 126 and the dummy from frame 139: -13 on block.
	struct blocked_attack
	{
		calculator frames;
		punish::watch watch;

		explicit blocked_attack(u32 dummy_free = 40)
		{
			frames.commit_reading(0, 900, 20, 1, free_on(1), false);
			frames.commit_reading(1, 120, 100, 1, free_on(dummy_free), true, 16, 15);
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
		// The player's attack with `startup`, started on frame `start` and connecting on its first active frame.
		void attack(u32 start, u32 startup, contact kind)
		{
			frames.commit_reading(0, 130 + startup, start, 1, free_on(30), true, startup + 1, startup);
			frames.applied_contact(0, kind, start + startup - 1);
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
		"i10 on the first free frame lands on frame 135, three frames before the dummy can guard on 139");
	check(!sample.watch.waiting() && !sample.at(137) && !sample.at(140), "each blocked attack is judged once");
}

PBD_TEST(PbdPracticePunish, TheLastFrameStillPunishes)
{
	blocked_attack sample;
	sample.reaction();
	sample.at(115);
	sample.attack(126, 13, contact::hit);
	const auto judged = sample.at(139);
	check(judged && judged->outcome == punish::verdict::punished && judged->frames == 0, "i13 on -13 lands on the frame before the dummy can guard");
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
			"i10 pressed four frames late lands on frame 139, when the dummy could guard");
	}
	{
		blocked_attack sample;
		sample.reaction();
		sample.at(115);
		sample.attack(126, 15, contact::hit);
		const auto judged = sample.at(141);
		check(judged && judged->outcome == punish::verdict::late && judged->frames == 2, "i15 on the first free frame lands two frames late");
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
	// The throw's reach starts on frame 126 and catches; the thrower's hold, a move of throw level, starts on frame 138.
	sample.frames.commit_reading(0, 300, 126, 1, free_on(20), true, 12, 12);
	check(!sample.at(127) && sample.watch.waiting(), "the throw has not caught yet");
	sample.frames.commit_reading(0, 301, 138, 1, free_on(60), true);
	sample.frames.throw_connected(0);
	check(!sample.at(139) && !sample.watch.waiting(), "a throw has no contact frame to judge");
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
	frames.commit_reading(0, 140, 126, 1, free_on(30), true, 11, 10);
	frames.applied_contact(0, contact::hit, 135);
	const auto judged = watch.boundary(frames.latest[0], frames.latest[1], 136);
	check(judged && judged->outcome == punish::verdict::punished && judged->frames == 3, "the punish after it is judged");
}
