#pragma once
#include <thread>

#include "../PBD/pbd_set_event_payload.h"
#include "../PBD/pbd_set_publication.h"
#include <memory>

namespace pbd::set_channel
{
	/** One emulation owner thread owns this object; a write failure poisons publication so later facts cannot overtake a gap. */
	class channel
	{
		std::filesystem::path directory;
		manifest session;
		std::uint64_t event_serial = 0, command_serial = 0;
		mutable std::optional<std::chrono::steady_clock::time_point> command_read_failure;
		bool failed = false;
		std::string failure;
		std::optional<bool> post_winner;
		// In-game selection: the stage and rows the current game's game_started reported. Its game_result must repeat them.
		std::optional<std::array<std::uint32_t, 3>> started_picks;
		bool fail(std::string code)
		{
			failed = true;
			failure = std::move(code);
			return false;
		}

		publication_result publish_file(std::uint64_t serial, const std::string& text) const
		{
			const auto destination = directory / ("set-event-" + std::to_string(serial) + ".json");
			const auto temporary = directory / (".set-event-" + std::to_string(serial) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");
#ifdef _WIN32
			const auto result = retry_publication([&]() -> publication_result
				{
					const auto denied = [](DWORD error)
					{
						return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED;
					};
					const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (file == INVALID_HANDLE_VALUE)
						return {false, denied(GetLastError()), "event_open_failed"};
					DWORD written = 0;
					const bool wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 && written == text.size();
					const DWORD write_error = wrote ? ERROR_SUCCESS : GetLastError();
					const bool closed = CloseHandle(file) != 0;
					if (!wrote || !closed)
					{
						const DWORD error = !wrote ? write_error : GetLastError();
						DeleteFileW(temporary.c_str());
						return {false, denied(error), "event_write_failed"};
					}
					// Closed bytes are visible to readers; a storage flush must not block a fighting thread.
				    // No replacement: an existing published serial is never overwritten.
					if (!MoveFileExW(temporary.c_str(), destination.c_str(), 0))
					{
						const DWORD error = GetLastError();
						DeleteFileW(temporary.c_str());
						return {false, denied(error), "event_publish_failed"};
					}
					return {true, false, nullptr};
				},
				[]
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				},
				[]
				{
					return std::chrono::steady_clock::now();
				});
			return result;
#else
			std::ofstream file(temporary, std::ios::binary | std::ios::out);
			file.write(text.data(), static_cast<std::streamsize>(text.size()));
			file.flush();
			file.close();
			if (!file)
				return {false, false, "event_write_failed"};
			// A hard-link publication is atomic and refuses an existing destination on hosts whose rename would replace it.
			std::error_code ec;
			std::filesystem::create_hard_link(temporary, destination, ec);
			const bool published = !ec;
			std::filesystem::remove(temporary, ec);
			if (!published)
				return {false, false, "event_publish_failed"};
#endif
			return {true, false, nullptr};
		}
		// Declared last: background I/O joins while its immutable directory is still alive.
		std::unique_ptr<publication_writer<>> writer;
		std::unique_ptr<command_reader> reader;

	public:
		channel(std::filesystem::path root, manifest value, bool background = false) : directory(std::move(root)), session(std::move(value))
		{
			if (!directory.is_absolute())
			{
				failed = true;
				failure = "relative_channel";
			}
			if (background && !failed)
			{
				writer = std::make_unique<publication_writer<>>([this](std::uint64_t serial, const std::string& text)
					{
						return publish_file(serial, text);
					});
				reader = std::make_unique<command_reader>([this](std::uint64_t serial)
					{
						return detail::read_text(directory / ("set-command-" + std::to_string(serial) + ".json"));
					});
			}
		}
		const manifest& identity() const
		{
			return session;
		}
		std::string error() const
		{
			return writer && writer->error() ? writer->error() : failure;
		}
		std::uint64_t last_event() const
		{
			return event_serial;
		}
		std::uint64_t last_command() const
		{
			return command_serial;
		}
		bool healthy() const
		{
			return !failed && (!writer || !writer->error());
		}

		bool flush() const noexcept
		{
			return !writer || writer->flush(event_serial);
		}
		bool output_failed() const noexcept
		{
			return writer && writer->error();
		}

		result<command> next_command(std::uint32_t current_game) const
		{
			if (!healthy())
				return {{}, error()};
			auto text = reader ? reader->poll(command_serial + 1) :
			                     detail::read_text(directory / ("set-command-" + std::to_string(command_serial + 1) + ".json"));
			if (!text.value)
			{
				// An atomic rename on a Windows share can briefly retain the writer's exclusive handle.
				// Poll again without advancing the command; persistent denial still fails within one second.
				if (text.error == "read_failed")
				{
					const auto now = std::chrono::steady_clock::now();
					if (!command_read_failure)
						command_read_failure = now;
					if (now - *command_read_failure < std::chrono::seconds(1))
						return {{}, "unavailable"};
				}
				return {{}, text.error};
			}
			command_read_failure.reset();
			return parse_command(*text.value, session, command_serial + 1, current_game);
		}

		bool write_event(std::string_view type, json_value payload = json_value::object())
		{
			if (!healthy())
				return false;
			if (payload.kind != json_kind::object)
				return fail("invalid_event_payload");
			const bool known = type == "game_preparing" || type == "game_started" || type == "game_result" || type == "set_ended" || type == "post_set_choice" ||
			                   type == "between_games_ready" || type == "leave" || type == "confirmed_inputs" || type == "contract_error" || type == "command_applied";
			if (!known)
				return fail("unknown_event");
			for (const auto key : {"schema", "sessionId", "setId", "serial", "at", "type"})
				if (payload.find(key))
					return fail("event_envelope_collision");
			if (!valid_event_payload(type, payload, session, post_winner))
				return fail("invalid_event_payload");
			const auto* capsule = payload.find("gameStartCapsule");
			const bool capsule_start = type == "game_started" && capsule && capsule->kind == json_kind::object;
			std::optional<std::array<std::uint32_t, 3>> reported;
			if (in_game(session) && (type == "game_started" || type == "game_result"))
			{
				std::array<std::uint32_t, 3> picks{};
				if (!detail::number32(payload.find("stageId"), 0, max_stage_id, picks[0]) || !detail::number32(payload.find("leftCharacterRow"), 0, max_character_row, picks[1]) ||
					!detail::number32(payload.find("rightCharacterRow"), 0, max_character_row, picks[2]))
					return fail("invalid_event_payload");
				// A result names the fighters and stage its game started with; the next game_started may report new ones.
				if (type == "game_result" && started_picks != picks)
					return fail("event_picks_changed");
				reported = picks;
			}
			const auto at = utc_now();
			if (at.empty() || event_serial >= max_safe_integer)
				return fail("invalid_event_envelope");
			json_value event = json_value::object({{"schema", json_value::string(std::string(schema_of(session)))}, {"sessionId", json_value::string(session.session_id)},
				{"setId", json_value::string(session.set_id)}, {"serial", json_value::number(event_serial + 1)}, {"at", json_value::string(at)}, {"type", json_value::string(std::string(type))}});
			for (auto& field : payload.fields)
				event.fields.push_back(std::move(field));
			auto text = detail::serialize(event) + '\n';
			if (text.size() > max_file_bytes ||
				!detail::json_parser(text, capsule_start ? max_json_nodes * 4 : max_json_nodes, max_file_bytes,
					capsule_start ? 65536 : 4096)
					.parse()
					.value)
				return fail("invalid_event_payload");
			if (writer)
			{
				if (!writer->submit(event_serial + 1, std::move(text)))
					return fail("event_queue_full");
			}
			else if (const auto result = publish_file(event_serial + 1, text); !result.published)
				return fail(result.error);
			event_serial++;
			if (type == "game_started" && reported)
				started_picks = reported;
			return true;
		}

		/** Call after the command's effect succeeded. An acknowledgement cannot claim the effect of a refused command. */
		bool acknowledge(const command& applied)
		{
			if (!healthy() || applied.serial != command_serial + 1)
				return false;
			if (!write_event("command_applied", json_value::object({{"commandSerial", json_value::number(applied.serial)}})))
				return false;
			command_serial = applied.serial;
			if (applied.kind == command_kind::show_post_set)
				post_winner = applied.winner;
			return true;
		}

		// A confirmed natural VS continuation keeps the same process, event cursor and fixed set values.
		// The driver verifies its sealed result before requesting this channel transition. A drawn game's replay keeps its
		// number and score.
		bool continue_naturally(const command& applied)
		{
			if (!healthy() || applied.kind != command_kind::continue_game || applied.game_number != session.game_number + (applied.replay ? 0u : 1u) ||
				(applied.replay && (applied.games.left != session.games.left || applied.games.right != session.games.right)) ||
				applied.game_number != applied.games.left + applied.games.right + 1 || !acknowledge(applied))
				return false;
			session.game_number = applied.game_number;
			session.games = applied.games;
			if (!in_game(session) && applied.rematch)
				session.stage_id = applied.rematch->selected.stage;
			post_winner.reset();
			started_picks.reset();
			return true;
		}

		// The rank the game's own PROMOTED TO gave `player` at the set's last knockout (pbd::rank_stakes::kept_promotion): the
		// records keep showing it at every later scene change. A next_set naming the player's rank replaces it.
		void hold_rank(side player, std::uint32_t rank)
		{
			(player == side::left ? session.left : session.right).rank = rank;
		}

		// A next set against the same opponent keeps the process, the directory and both cursors. The driver verifies the
		// completed set's terminal result first. The acknowledgement is written under the completed set's identity, so the
		// launcher reads it with the identity it wrote the command under; everything after it belongs to the new set, including
		// the ranks it names (a player it names none for keeps the rank the plates showed).
		bool next_set(const command& applied)
		{
			const bool renamed = !applied.next_set_id.empty() && applied.next_set_id != session.set_id && !applied.next_match_id.empty() &&
			                     applied.next_match_id != session.match_id;
			if (!healthy() || applied.kind != command_kind::next_set || !renamed || applied.game_number != 1 || applied.games.left ||
				applied.games.right || applied.first_to_games < 1 || applied.first_to_games > 10 || !acknowledge(applied))
				return false;
			session.set_id = applied.next_set_id;
			session.match_id = applied.next_match_id;
			session.first_to_games = applied.first_to_games;
			session.game_number = 1;
			session.games = {};
			if (!in_game(session) && applied.rematch)
				session.stage_id = applied.rematch->selected.stage;
			if (applied.ranks[0])
				session.left.rank = *applied.ranks[0];
			if (applied.ranks[1])
				session.right.rank = *applied.ranks[1];
			post_winner.reset();
			started_picks.reset();
			return true;
		}
	};

	inline std::optional<std::filesystem::path> requested_directory()
	{
#ifdef _WIN32
		const wchar_t* value = pbd::env::find_wide("PBD_SET_CHANNEL");
#else
		const char* value = pbd::env::find("PBD_SET_CHANNEL");
#endif
		if (!value || !*value)
			return {};
		return std::filesystem::path(value);
	}

	// Whether the renderer needs the set's screens: a lobby set configured at launch, or a game held ready for one
	// (PBD_MATCH_STANDBY_CHANNEL). A held game binds its set's channel only at activation, after the RSX thread has made the
	// overlays; until then it publishes nothing, so they stay invisible.
	inline bool set_screens_requested()
	{
		if (requested_directory())
			return true;
#ifdef _WIN32
		const wchar_t* value = pbd::env::find_wide("PBD_MATCH_STANDBY_CHANNEL");
#else
		const char* value = pbd::env::find("PBD_MATCH_STANDBY_CHANNEL");
#endif
		return value && *value;
	}
} // namespace pbd::set_channel
