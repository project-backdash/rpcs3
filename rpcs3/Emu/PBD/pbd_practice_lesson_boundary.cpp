#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_practice_lesson_channel.h"

namespace lesson = pbd::practice::lesson;
using namespace pbd::practice;
#include "pbd_practice_lesson_steps.h"

void pbd::rollback_session::practice_steps::lesson_boundary()
{
	namespace lesson = pbd::practice::lesson;
	using namespace pbd::practice;
	auto& channel = lesson::current;
	const char* configured = pbd::env::find("PBD_PRACTICE_LESSON_CHANNEL");
	if (!configured || !*configured)
	{
		channel = {};
		return;
	}
	const std::filesystem::path root = std::filesystem::u8path(configured);
	if (!root.is_absolute() || std::string_view(configured).starts_with("\\\\") ||
		std::string_view(configured).starts_with("//"))
	{
		channel = {};
		return;
	}
	const auto generation = fmt::format("%u-%u-%u", static_cast<u64>(Emu.GetEmulationIdentifier()), data.session.matches, data.session.match_start);
	if (channel.root != root || channel.generation != generation)
	{
		channel = {};
		channel.root = root;
		channel.generation = generation;
	}
	const auto now = session_clock::now();
	const auto game = read_lesson_game();
	channel.permitted = game.in_match && !game.paused && data.practice.native.screen == 0 &&
	                    !data.practice.restart.reset_request && !data.practice.pause.requested && !data.practice.pause.resume_native && !data.practice_resume_settling();
	channel.play.observe(data.practice.frame_advanced);
	channel.capture.check_setup(game);
	if (channel.capture.active)
	{
		channel.capture.observe(data.practice.frame_advanced, data.practice.history_words[channel.capture.port],
			data.practice.history_sides[channel.capture.port]);
	}
	if (channel.outcome == "playing" && !channel.play.active())
		channel.finish("complete");
	if ((channel.preparing || channel.play.active() || channel.reset) && now >= channel.deadline)
		channel.finish("not_ready");
	if (!game.in_match && (channel.preparing || channel.play.active() || channel.reset))
		channel.finish("not_ready");
	if (channel.preparing && session_runtime.m_practice_memory && !session_runtime.m_practice_memory->restore && !game.paused &&
		!data.practice_resume_settling())
	{
		const auto result = pbd_admit_lesson(channel.preparing->setup, game);
		channel.finish(result == pbd_lesson_check::ready ? "complete" : lesson::refusal(result));
	}
	lesson_steps steps{*this, channel, root, generation, now, game};
	steps.poll_command();
	steps.publish_status();
}

pbd::practice::pbd_lesson_game pbd::rollback_session::practice_steps::read_lesson_game()
{
	using namespace pbd::practice;
	const auto native = pbd::practice::native::inspect(true);
	pbd_lesson_game game;
	read_word(pbd::t5dr::game_mode_address, game.mode);
	game.in_match = data.session.in_match && native.practice && data.practice.native.pause_valid;
	game.paused = data.practice.native.paused || data.practice.menu.open || data.game_overlay.claim;
	game.setup.side = native.initiating == 0x10090910 ? pbd_lesson_side::right : pbd_lesson_side::left;
	read_word(0x1008f088, game.setup.stage);
	if (session_runtime.m_practice_memory && session_runtime.m_practice_memory->tables)
		for (u32 logical = 0; logical < 2; ++logical)
		{
			const u32 player = pbd_lesson_play_port(game.setup.side, true);
			const u32 port = logical ? 1 - player : player;
			u32 row = umax;
			if (read_word(port ? 0x10093f54 : 0x10093f50, row) && row <= practice_store::max_character_row)
				game.setup.characters[logical] = session_runtime.m_practice_memory->tables->characters[row].character;
		}
	u32 action = umax, counter = umax;
	if (native.practice)
	{
		read_word(native.practice + 0x6c, action);
		read_word(native.practice + 0x30, counter);
		if (data.practice.native.controlled)
			action = data.practice.native.action[0];
	}
	game.setup.stance = action == 2 || action == 3 ? pbd_lesson_stance::crouching : pbd_lesson_stance::standing;
	game.setup.hit_condition = counter == 1 ? pbd_lesson_hit_condition::counter : pbd_lesson_hit_condition::normal;
	game.in_match = game.in_match && action <= 10 && counter <= 1;

	return game;
}
