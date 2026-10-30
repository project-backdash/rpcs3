#pragma once

// The lobby set driver tests' set: a manifest, its channel in a fresh temporary directory and the driver writing to it, with
// helpers that record, publish and play out whole games.
#include "pbd_test.h"

#include "Emu/Cell/pbd_lobby_set_driver.h"

namespace lobby_test_state
{
	using pbd::test::check;
	using namespace pbd::set_channel;
	using namespace pbd::match_recording;
	// What a game applied, read with both life bars full unless a test says otherwise.
	inline pbd::lobby_set::context applied(std::uint32_t stage, std::uint32_t left, std::uint32_t right, std::uint32_t rounds = 3,
		bool verified = true)
	{
		pbd::lobby_set::context out{stage, left, right, rounds, verified};
		out.handicap = {pbd::lobby_set::full_handicap, pbd::lobby_set::full_handicap};
		return out;
	}
	inline manifest identity(std::uint32_t first_to = 1, bool chosen = false, bool allow_jinpachi = true)
	{
		manifest m;
		m.session_id = "session-test";
		m.set_id = "set-test";
		m.match_id = "match-test";
		m.local_member_id = "left-test";
		m.local_side = side::left;
		m.left.member_id = "left-test";
		m.left.name = "Left";
		m.left.character_id = "left";
		m.left.row = 12;
		m.right.member_id = "right-test";
		m.right.name = "Right";
		m.right.character_id = "right";
		m.right.row = 32;
		m.game_number = 1;
		m.stage_id = 17;
		m.first_to_games = first_to;
		m.rounds_per_game = 3;
		m.build = "test-build";
		m.compat = "test-compat";
		m.publish_confirmed_inputs = true;
		m.allow_jinpachi = allow_jinpachi;
		if (chosen)
		{
			// Contract 2, in-game selection: no characters and no stage in the manifest; the game's own select decides them.
			m.contract = 2;
			m.selection.mode = selection_mode::in_game;
			m.selection.character_seconds = 30;
			m.selection.stage_seconds = 15;
			for (auto* player : {&m.left, &m.right})
			{
				player->has_character = false;
				player->character_id.clear();
				player->row = 0;
			}
			m.stage_id = 0;
		}
		return m;
	}
	struct sample_document
	{
		std::filesystem::path parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
		std::filesystem::path directory;
		channel events;
		pbd::lobby_set::driver driver;
		explicit sample_document(unsigned id, std::uint32_t first_to = 1, bool chosen = false, bool ban_jinpachi = false,
			std::function<std::shared_ptr<const std::string>()> start_record = {})
			: directory(parent / ("pbd-lobby-driver-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
									 "-" + std::to_string(id))),
			  events(directory, identity(first_to, chosen, !ban_jinpachi)), driver(events, [](const std::vector<std::uint8_t>&)
																				{
																					return std::string(64, 'a');
																				},
																				std::move(start_record), std::string(64, 'e'))
		{
			check(std::filesystem::create_directory(directory));
		}
		~sample_document()
		{
			const auto resolved = std::filesystem::canonical(directory);
			check(std::filesystem::equivalent(resolved.parent_path(), parent) &&
				  resolved.filename().string().starts_with("pbd-lobby-driver-test-"));
			std::filesystem::remove_all(resolved);
		}
		bool begin(bool selection = true, bool clean = true, bool round_state = false, bool keyed_draws = false, bool from_start = false,
			bool ban_jinpachi = false)
		{
			pbd::lobby_set::clean_start start{123, 7, std::string(64, 'b'), "NPUB30009", clean, selection};
			start.require_round_state = round_state;
			start.keyed_draws = keyed_draws;
			start.publish_from_start = from_start;
			start.ban_jinpachi = ban_jinpachi;
			return driver.begin(0, 0x8000007b, std::move(start));
		}
		json_value read(std::uint64_t serial)
		{
			const auto source = detail::read_text(directory / ("set-event-" + std::to_string(serial) + ".json"));
			check(source.value);
			const auto parsed = detail::json_parser(*source.value).parse();
			check(parsed.value);
			return *parsed.value;
		}
		bool record(std::uint32_t boundary, phase value, std::uint32_t left = 0, std::uint32_t right = 0, winner result = winner::none,
			pbd::lobby_set::context picks = applied(17, 12, 32))
		{
			observation o;
			o.boundary = boundary;
			o.native_phase = value;
			o.phase_verified = true;
			o.result_verified = true;
			o.score_left = left;
			o.score_right = right;
			o.outcome = result;
			return driver.record(o, picks);
		}
		bool publish(std::uint32_t through)
		{
			return driver.publish_through(through, [](std::uint32_t)
				{
					return true;
				},
				[](std::uint32_t)
				{
					return pbd::lobby_set::input_records{{0x0040, 0x0020}, {0x0040, 0x0020}, true};
				},
				[](std::uint32_t boundary)
				{
					return pbd::lobby_set::sync_facts{boundary, 0, 0x8000007b + boundary, 17, 7, 0, {11, 22, 33}, true};
				});
		}
		// Rounds trade to 2-2, then a double K.O. (both markers -1) gives both fighters their third round: the game is drawn at 3-3.
		// Boundaries 0..17; the match end is 17. Two input packets preserve every boundary before its result.
		void drawn_game()
		{
			check(record(0, phase::vs));
			check(record(1, phase::character_intro));
			const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> traded{{{1, 0}, {1, 1}, {2, 1}, {2, 2}}};
			std::uint32_t boundary = 2;
			for (const auto& [left, right] : traded)
			{
				check(record(boundary++, phase::round_intro));
				check(record(boundary++, phase::fight));
				check(record(boundary++, phase::round_result, left, right));
			}
			check(record(boundary++, phase::round_intro));
			check(record(boundary++, phase::fight));
			observation draw;
			draw.boundary = boundary++;
			draw.native_phase = phase::round_result;
			draw.phase_verified = true;
			draw.result_verified = true;
			draw.draw_verified = true;
			draw.score_left = 3;
			draw.score_right = 3;
			check(driver.record(draw, applied(17, 12, 32)));
			check(boundary == 17 && record(17, phase::match_end, 3, 3, winner::draw));
			check(publish(17));
			check(driver.complete());
			check(events.last_event() == 5);
			check(read(2).find("firstFrame")->text == "0" && read(2).find("frameCount")->text == "15");
			check(read(3).find("firstFrame")->text == "15" && read(3).find("frameCount")->text == "2");
		}
		// Left wins three straight rounds; the match end is 11. Events: game_started, confirmed_inputs, game_result, then set_ended
		// in a first-to-one set or between_games_ready in a longer one.
		void won_game()
		{
			check(record(0, phase::vs));
			check(record(1, phase::character_intro));
			for (std::uint32_t won = 1, boundary = 2; won <= 3; ++won)
			{
				check(record(boundary++, phase::round_intro));
				check(record(boundary++, phase::fight));
				check(record(boundary++, phase::round_result, won));
			}
			check(record(11, phase::match_end, 3, 0, winner::left));
			check(publish(11));
			check(driver.complete());
			check(events.last_event() == 4);
		}
	};
} // namespace lobby_test_state
