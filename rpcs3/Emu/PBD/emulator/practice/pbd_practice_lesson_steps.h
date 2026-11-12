#pragma once

struct pbd::rollback_session::lesson_steps : practice_steps
{
	decltype(pbd::practice::lesson::current)& channel;
	const std::filesystem::path& root;
	const std::string& generation;
	const session_clock::time_point& now;
	const pbd::practice::pbd_lesson_game& game;

	void poll_command()
	{
		if (now >= channel.next_poll)
		{
			channel.next_poll = now + std::chrono::milliseconds(50);
			std::error_code ec;
			const auto file = root / "command.json";
			const auto size = std::filesystem::file_size(file, ec);
			if (!ec && size <= 16384)
			{
				std::ifstream input(file, std::ios::binary);
				std::string text(static_cast<std::size_t>(size), '\0');
				if (input.read(text.data(), text.size()))
				{
					auto parsed = lesson::parse(text);
					if (parsed.value && parsed.value->id != channel.seen)
					{
						apply_command(std::move(*parsed.value));
					}
				}
			}
		}
	}

	void publish_status()
	{
		const auto status = fmt::format("{\"schema\":\"pbd-practice-lesson-status-1\",\"captureAvailable\":true,\"generation\":\"%s\",\"mode\":%u,\"inMatch\":%s,\"paused\":%s,\"characters\":[%u,%u],\"stage\":%u,\"side\":\"%s\",\"stance\":\"%s\",\"hitCondition\":\"%s\",\"ack\":%s,\"outcome\":%s,\"rejected\":%s,\"rejection\":%s}\n",
			generation, game.mode, json_bool(game.in_match), json_bool(game.paused), game.setup.characters[0], game.setup.characters[1], game.setup.stage,
			game.setup.side == pbd_lesson_side::right ? "right" : "left", game.setup.stance == pbd_lesson_stance::crouching ? "crouching" : "standing",
			game.setup.hit_condition == pbd_lesson_hit_condition::counter ? "counter" : "normal", pbd::json::quoted(channel.ack), pbd::json::quoted(channel.outcome),
			pbd::json::quoted(channel.rejected), pbd::json::quoted(channel.rejection));
		if (status != channel.published || now >= channel.next_publish)
		{
			std::ofstream output(root / "status.json", std::ios::binary | std::ios::trunc);
			if (output.write(status.data(), status.size()))
			{
				channel.published = status;
				channel.next_publish = now + std::chrono::seconds(1);
			}
		}
	}

	void apply_command(pbd::practice::lesson::command command)
	{
		channel.seen = command.id;
		const bool busy = channel.preparing || channel.play.active() || channel.reset ||
		                  (channel.capture.started && command.kind != "play");
		if (command.generation != generation)
			channel.refuse(command.id, "stale_generation");
		else if (command.kind == "cancel")
		{
			channel.ack = command.id;
			channel.capture = {};
			channel.finish("complete");
		}
		else if (command.kind == "record_end")
		{
			channel.end_capture(command.id);
		}
		else if (busy)
			channel.refuse(command.id, "lesson_busy");
		else
		{
			start_lesson(std::move(command));
		}
	}

	void start_lesson(pbd::practice::lesson::command&& command)
	{
		channel.ack = command.id;
		auto configuration = game;
		if (command.kind == "prepare")
		{
			configuration.setup.stance = command.setup.stance;
			configuration.setup.hit_condition = command.setup.hit_condition;
		}
		const auto result = pbd_admit_lesson(command.setup, configuration);
		channel.outcome = lesson::refusal(result);
		if (result == pbd_lesson_check::ready && data.practice.dummy.state != dummy_state::idle)
			channel.outcome = "lesson_busy";
		else if (result == pbd_lesson_check::ready)
		{
			channel.deadline = now + std::chrono::seconds(65);
			if (command.kind == "prepare")
			{
				auto options = practice_game_values();
				if (!session_runtime.m_practice_memory || !options)
					channel.outcome = "setup_unavailable";
				else
				{
					(*options)[0] = command.setup.stance == pbd_lesson_stance::crouching ? 2 : 0;
					(*options)[1] = command.setup.hit_condition == pbd_lesson_hit_condition::counter ? 1 : 0;
					// The lesson's stance and hit condition hold for all of it: no guard choice or random counter hit.
					data.practice.reactions.guard.choose(pbd::practice::pbd_guard_option::game);
					data.practice.reactions.counter_hit.choose(false);
					auto& memory = *session_runtime.m_practice_memory;
					memory.restore = pbd::practice::pack_game_options(*options);
					memory.restore_from = index;
					memory.restore_requested = memory.restore_queued = false;
					channel.preparing = std::move(command);
					channel.outcome = "preparing";
				}
			}
			else if (command.kind == "play")
			{
				channel.play.words = std::move(command.words);
				channel.play.port = command.port;
				channel.play.demonstration = command.port == pbd_lesson_play_port(command.setup.side, true);
				channel.play.position = 0;
				channel.play.pending = false;
				channel.outcome = "playing";
			}
			else if (command.kind == "record_start")
			{
				channel.capture.begin(command.setup);
				channel.outcome = "complete";
			}
			else
			{
				// A lesson sets up its own situation, so the player's Restart Settings stay out of it.
				queue_reset(pbd::game_lifecycle::placement::centre, {});
				channel.reset = data.practice.restart.reset_request ? data.practice.restart.reset_request->request_id : 0;
				channel.outcome = channel.reset ? "resetting" : "not_ready";
			}
		}
	}
};
