#pragma once

// Scenarios run by replay_viewer_test.cpp.
static void takeover_prompt_owns_input()
{
	for (const auto state : {pbd::takeover_phase::armed, pbd::takeover_phase::controlling})
		for (const bool paused : {false, true})
			for (const bool fighting : {false, true})
			{
				controller trial;
				context trial_context{paused, true, 0, state, 3, fighting};
				trial.poll(0, trial_context);
				const auto opened = trial.poll(start | cross | right, trial_context);
				check(opened.kind == (paused ? command::none : command::pause) && opened.game == 0);
				check(trial.menu == page::takeover_options && trial.menu_was_paused == paused);
				trial_context.paused = true;
				check(trial.poll(cross | right, trial_context).game == 0);
				check(trial.poll(cross, trial_context).game == 0 && trial.choice == 0);
				for (unsigned n = 0; n < 20; ++n)
					check(trial.poll(cross, trial_context).game == 0 && trial.choice == 0);
				// Resume, (Try Again, Return to Replay,) Display, Return to Launcher, Exit Game.
				const unsigned display_row = state == pbd::takeover_phase::controlling ? 3u : 1u;
				for (unsigned n = 0; n < display_row; ++n)
					check(press(trial, down, trial_context).game == 0);
				check(trial.choice == display_row);
				check(press(trial, cross, trial_context).game == 0 && trial.menu == page::display);
				check(trial.poll(cross | right, trial_context).game == 0 && !trial.history);
				check(press(trial, cross, trial_context).game == 0 && trial.history);
				check(press(trial, circle, trial_context).kind == command::none && trial.menu == page::takeover_options &&
					  trial.choice == display_row);
				const auto closed = press(trial, circle, trial_context);
				check(closed.kind == (paused ? command::none : command::pause) && closed.game == 0);
				check(trial.menu == page::none && trial.menu_was_paused == paused);
				trial_context.paused = paused;
				check(trial.poll(circle | cross, trial_context).game == 0);
				const auto fight = press(trial, circle | right, trial_context);
				if (state == pbd::takeover_phase::controlling)
					check(fight.kind == command::none && fight.game == (circle | right));
				else if (fighting)
					check(fight.kind == command::start_take && fight.game == (circle | right));
				else
					check(fight.kind == command::none && fight.game == 0);

				snapshot options_view;
				options_view.takeover = state;
				options_view.menu = static_cast<unsigned>(page::takeover_options);
				const auto layout = options_for(page::takeover_options, state);
				bool has_display = false, has_resume = false, has_try = false, has_return = false, has_round = false;
				bool has_launcher = false, has_quit = false;
				for (std::uint32_t i = 0; i < layout.count; ++i)
				{
					const auto row = layout.rows[i];
					has_display |= row == option::display;
					has_resume |= row == option::resume;
					has_try |= row == option::retry;
					has_return |= row == option::return_replay;
					has_round |= row == option::rounds;
					has_launcher |= row == option::launcher;
					has_quit |= row == option::quit;
				}
				check(has_display && has_resume && has_try == (state == pbd::takeover_phase::controlling) && has_return == (state == pbd::takeover_phase::controlling) && !has_round);
				check(has_launcher && has_quit && layout.count == (state == pbd::takeover_phase::controlling ? 6u : 4u));
				const auto bar = prompts(options_view);
				bool move = false, back = false;
				for (std::uint32_t i = 0; i < bar.count; ++i)
				{
					move |= bar.items[i].keys == "{dpad:ud}";
					back |= bar.items[i].pointer == pointer_options;
				}
				check(move && back);
			}
}

static void takeover_menu_close_and_resume()
{
	// Only trial-safe rows are navigable; gameplay shoulders cannot seek, step or change speed inside the menu.
	for (const auto state : {pbd::takeover_phase::armed, pbd::takeover_phase::controlling})
	{
		controller trial;
		context tx{false, true, 0, state, 0, true};
		press(trial, start, tx);
		tx.paused = true;
		press(trial, up, tx);
		check(trial.choice == (state == pbd::takeover_phase::controlling ? 5u : 3u));
		press(trial, down, tx);
		check(trial.choice == 0);
		for (const auto button : {l1, r1, l2, r2, left, right})
		{
			const auto ignored = press(trial, button, tx);
			check(ignored.kind == command::none && ignored.game == 0 && trial.choice == 0);
		}
	}
	// Back/START restore pause; explicit Resume may deliberately resume a previously paused trial.
	for (const auto state : {pbd::takeover_phase::armed, pbd::takeover_phase::controlling})
		for (const bool paused : {false, true})
		{
			for (const auto close : {circle, start})
			{
				controller trial;
				context tx{paused, true, 0, state, 0, true};
				press(trial, start, tx);
				tx.paused = true;
				const auto closed = press(trial, close, tx);
				check(closed.kind == (paused ? command::none : command::pause) && closed.game == 0 && trial.menu == page::none);
				check(trial.poll(close | right, tx).game == 0);
			}
			controller trial;
			context tx{paused, true, 0, state, 0, true};
			press(trial, start, tx);
			tx.paused = true;
			check(press(trial, cross, tx).kind == command::pause && trial.menu == page::none); // Explicit Play.
			check(trial.poll(cross, tx).game == 0);
		}
}

static void retry_and_return_commands()
{
	// SELECT taps open Display only on complete release. Chords and cancelled taps never open it or leak input.
	for (const auto state : {pbd::takeover_phase::armed, pbd::takeover_phase::controlling})
		for (const bool paused : {false, true})
		{
			controller trial;
			context tx{paused, true, 0, state, 0, true};
			trial.poll(0, tx);
			check(trial.poll(select, tx).kind == command::none && trial.menu == page::none);
			for (unsigned n = 0; n < 20; ++n)
				check(trial.poll(select, tx).game == 0 && trial.menu == page::none);
			const auto display = trial.poll(0, tx);
			check(display.kind == (paused ? command::none : command::pause) && trial.menu == page::display);
			tx.paused = true;
			check(press(trial, circle, tx).kind == (paused ? command::none : command::pause));
			trial.poll(0, tx);
			tx.paused = paused;
			check(trial.poll(select, tx).game == 0);
			check(trial.poll(select | right, tx).game == 0);
			check(trial.poll(right, tx).game == 0 && trial.menu == page::none);
			check(trial.poll(right | cross, tx).game == 0);
			const auto fresh = press(trial, cross, tx);
			check(fresh.kind == (state == pbd::takeover_phase::armed ? command::start_take : command::none) && fresh.game == cross);
			for (const auto button : {cross, circle})
			{
				const auto chord = press(trial, select | button | right, tx);
				check(chord.kind == (state == pbd::takeover_phase::armed ? command::none : button == cross ? command::retry :
																											 command::return_replay));
				check(chord.game == 0 && trial.menu == page::none);
				check(trial.poll(0, tx).game == 0 && trial.menu == page::none);
			}
			check(press(trial, start | select | cross, tx).game == 0 && trial.menu == page::none);
			check(trial.poll(cross, tx).game == 0 && trial.poll(cross, tx).game == 0);
			check(trial.poll(0, tx).game == 0 && trial.menu == page::none);
		}
	// Active menu retry/return use the same native restore commands; held confirms cannot become fighter input.
	for (const bool retry : {false, true})
	{
		controller trial;
		context tx{false, true, 0, pbd::takeover_phase::controlling, 0, true};
		press(trial, start, tx);
		tx.paused = true;
		for (unsigned n = 0; n < (retry ? 1u : 2u); ++n)
			press(trial, down, tx);
		const auto answer = press(trial, cross | right, tx);
		check(answer.kind == (retry ? command::retry : command::return_replay) && answer.game == 0 && trial.menu == page::none);
		check(trial.poll(cross | right, tx).game == 0);
	}
}

static void takeover_display_and_prompt_routing()
{
	// A completed 600-frame budget preempts any menu. Holding a menu below the deadline consumes no wall-clock budget.
	check(!takeover_finished(1599, 1600, true, 1598, 1000));
	check(takeover_finished(1600, 1600, true, 1599, 1000));
	check(takeover_finished(1601, 1600, true, 1600, 1000));
	check(!takeover_finished(1207, 1600, false, 1200, 1000));
	check(takeover_finished(1208, 1600, false, 1200, 1000));
	check(!takeover_finished(1208, 1600, false, 999, 1000));
	check(!takeover_finished(1199, 1600, false, 1200, 1000));
	for (const auto menu : {page::takeover_options, page::display})
		for (const auto prompt : {pbd::takeover_phase::end_prompt, pbd::takeover_phase::enable_prompt})
		{
			controller trial;
			context tx{false, true, 0, pbd::takeover_phase::controlling, 0, true};
			trial.poll(0, tx);
			trial.open(menu, start | cross, false);
			tx.paused = true;
			for (unsigned n = 0; n < 400; ++n)
			{
				check(trial.poll(start | cross, tx).game == 0);
				check(!takeover_finished(1500, 1600, true, 1499, 1000));
			}
			tx.takeover = prompt;
			check(trial.poll(cross, tx).kind == command::none && trial.menu == page::none);
			check(trial.poll(cross | right, tx).kind == command::none && trial.poll(cross, tx).game == 0);
			check(press(trial, cross, tx).kind == (prompt == pbd::takeover_phase::end_prompt ? command::retry : command::arm));
		}

	// Combined navigation keeps all 3 Display rows reachable from either 4/6-row trial menu.
	for (const auto state : {pbd::takeover_phase::armed, pbd::takeover_phase::controlling})
	{
		controller trial;
		context tx{false, true, 0, state, 0, true};
		press(trial, start, tx);
		tx.paused = true;
		for (unsigned n = 0; n < (state == pbd::takeover_phase::controlling ? 6u : 4u); ++n)
			press(trial, down, tx);
		check(trial.choice == 0 && trial.menu == page::takeover_options);
		for (unsigned n = 0; n < (state == pbd::takeover_phase::controlling ? 3u : 1u); ++n)
			press(trial, down, tx);
		press(trial, cross, tx);
		check(trial.menu == page::display);
		press(trial, down, tx);
		press(trial, down, tx);
		check(trial.choice == 2);
		press(trial, down, tx);
		check(trial.choice == 0);
		press(trial, cross, tx);
		check(trial.history && history_visible(display_flags(trial, true), 0) && !history_visible(display_flags(trial, true), 1));
	}
}
