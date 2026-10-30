#pragma once

// Existing natural-continuation and draw scenarios for the lobby driver.
static void draw_result_and_replay_marker()
{
	{
		// A drawn game names no winner and does not count, even in a first-to-one set: the set goes on, and its replay keeps the
		// game number and the score. The replay then decides the set under that same number.
		sample_document test(12);
		check(test.begin());
		test.drawn_game();
		const auto result = test.read(4), ready = test.read(5);
		check(detail::is(result.find("type"), "game_result") && detail::is(result.find("outcome"), "draw"));
		check(result.find("winner") && result.find("winner")->kind == json_kind::null);
		check(result.find("gameNumber")->text == "1" && result.find("finalFrame")->text == "17");
		check(result.find("roundWins")->find("left")->text == "3" && result.find("roundWins")->find("right")->text == "3");
		check(detail::is(ready.find("type"), "between_games_ready") && ready.find("gameNumber")->text == "1");
		const auto sealed = test.driver.sealed_game();
		check(sealed && sealed->game_number == 1 && sealed->outcome == winner::draw && !sealed->terminal);
		check(sealed->games.left == 0 && sealed->games.right == 0 && sealed->end_boundary == 17);
		command replay;
		replay.kind = command_kind::continue_game;
		replay.serial = 1;
		replay.game_number = 1;
		replay.replay = true;
		check(test.driver.continue_game(replay, 17, 0x8000008c));
		check(test.events.identity().game_number == 1 && test.events.last_command() == 1 && !test.driver.complete());
		check(detail::is(test.read(6).find("type"), "command_applied"));
		check(test.record(17, phase::vs));
		check(test.record(18, phase::character_intro));
		for (std::uint32_t won = 1, boundary = 19; won <= 3; ++won)
		{
			check(test.record(boundary++, phase::round_intro));
			check(test.record(boundary++, phase::fight));
			check(test.record(boundary++, phase::round_result, won));
		}
		check(test.record(28, phase::match_end, 3, 0, winner::left));
		check(test.publish(28));
		const auto started = test.read(7), decided = test.read(9), ended = test.read(10);
		check(detail::is(started.find("type"), "game_started") && started.find("gameNumber")->text == "1");
		check(detail::is(started.find("start")->find("kind"), "natural_vs") && started.find("start")->find("originBoundary")->text == "17");
		check(detail::is(decided.find("type"), "game_result") && detail::is(decided.find("winner"), "left") && !decided.find("outcome"));
		check(decided.find("gameNumber")->text == "1");
		check(detail::is(ended.find("type"), "set_ended") && ended.find("score")->find("left")->text == "1");
		check(test.driver.sealed_game()->terminal);
	}
	{
		// After a draw the next game number is not a continuation: only the replay marker with the same number is.
		sample_document test(13);
		check(test.begin());
		test.drawn_game();
		command next;
		next.kind = command_kind::continue_game;
		next.serial = 1;
		next.game_number = 2;
		next.games = {1, 0};
		check(!test.driver.continue_game(next, 17, 0x8000008c));
		check(!test.driver.healthy());
		check(detail::is(test.read(6).find("code"), "natural_continuation_unverified"));
		check(test.events.identity().game_number == 1 && test.events.last_command() == 0);
	}
}

static void continuation_number_and_score()
{
	{
		// A decided game that leaves the set open continues only to the next number with the score it made.
		sample_document test(14, 2);
		check(test.begin());
		test.won_game();
		check(detail::is(test.read(3).find("winner"), "left") && detail::is(test.read(4).find("type"), "between_games_ready"));
		const auto sealed = test.driver.sealed_game();
		check(sealed && sealed->outcome == winner::left && !sealed->terminal && sealed->games.left == 1 && sealed->end_boundary == 11);
		command next;
		next.kind = command_kind::continue_game;
		next.serial = 1;
		next.game_number = 2;
		next.games = {1, 0};
		check(test.driver.continue_game(next, 11, 0x8000008c));
		check(test.events.identity().game_number == 2 && test.events.last_command() == 1);
		check(detail::is(test.read(5).find("type"), "command_applied"));
	}
	{
		// The draw's replay marker after a decided game is refused, so a decided game can never be played again.
		sample_document test(15, 2);
		check(test.begin());
		test.won_game();
		command replay;
		replay.kind = command_kind::continue_game;
		replay.serial = 1;
		replay.game_number = 1;
		replay.replay = true;
		check(!test.driver.continue_game(replay, 11, 0x8000008c));
		check(!test.driver.healthy());
		check(detail::is(test.read(5).find("code"), "natural_continuation_unverified"));
		check(test.events.identity().game_number == 1 && test.events.last_command() == 0);
	}
}

static void repick_rejects_winner_and_draw()
{
	{
		// Only the loser picks again: Left won, so a load naming the Left side is refused before anything is acknowledged.
		sample_document test(194, 3, true);
		check(test.begin());
		test.won_game();
		const auto previous = *test.driver.rematch_state();
		command next;
		next.kind = command_kind::continue_game;
		next.serial = 1;
		next.game_number = 2;
		next.games = {1, 0};
		next.rematch = pbd::rematch::load{1, 2, previous.end_frame, 1, previous.selected, previous.rules, false};
		next.rematch->repick = 0;
		check(!test.driver.continue_game(next, 11, 0x8000008c) && !test.driver.healthy());
		check(detail::is(test.read(5).find("code"), "rematch_load_changed") && test.events.last_command() == 0);
	}
	{
		// A drawn game has no loser: its replay cannot change a character.
		sample_document test(195);
		check(test.begin());
		test.drawn_game();
		const auto previous = *test.driver.rematch_state();
		command replay;
		replay.kind = command_kind::continue_game;
		replay.serial = 1;
		replay.game_number = 1;
		replay.replay = true;
		replay.rematch = pbd::rematch::load{1, 1, previous.end_frame, 1, previous.selected, previous.rules, false};
		replay.rematch->repick = 0;
		check(!test.driver.continue_game(replay, 17, 0x8000008c) && !test.driver.healthy());
		check(detail::is(test.read(6).find("code"), "rematch_load_changed"));
	}
}
