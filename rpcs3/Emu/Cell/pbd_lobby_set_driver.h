#pragma once

#include "../PBD/pbd_lobby_set_model.h"
#include "../PBD/pbd_session_health.h"

namespace pbd::lobby_set
{
	struct rejected_observation
	{
		pbd::match_recording::observation phase;
		context picks;
		pbd::match_recording::refusal reason;
	};
	class driver
	{
		struct sample
		{
			pbd::match_recording::observation phase;
			context picks;
		};
		channel& output;
		pbd::match_recording::lifecycle lifecycle;
		std::map<u32, sample> pending;
		std::vector<std::uint8_t> pads;
		std::vector<std::uint8_t> syncs;
		std::vector<std::uint8_t> paired_pads;
		std::vector<std::uint8_t> applied_pads;
		clean_start clean;
		u32 next = 0, frame = 0, chunk_first = 0, game_origin = 0;
		std::optional<sealed_game_result> sealed;
		std::optional<rejected_observation> rejected;
		bool started = false, admitted = false, failed = false, stream_enabled = true;
		// In-game selection: the stage and rows this game applied, read where its game_started is written.
		std::optional<context> observed;
		score games;
		std::function<std::string(const std::vector<std::uint8_t>&)> digest;
		std::function<std::shared_ptr<const std::string>()> pbd_start_record;
		std::string pbd_source_emulator;
		std::function<std::optional<pbd::fight_timing::result>(u32, u32)> pbd_performance;
		std::uint64_t rematch_epoch = 0;
		std::optional<pbd::rematch::load> current_load;
		// The values the game's own Ranked screens are written with in this game (pbd::rank_screens), recorded in its start, and
		// those given for the next game, which it takes as it begins.
		std::optional<pbd::rank_stakes::game_values> rank_values, next_rank_values;

		bool event(std::string_view type, json_value value)
		{
			if (failed || !output.write_event(type, std::move(value)))
			{
				failed = true;
				return false;
			}
			return true;
		}
		// A fixed set reports its agreed picks; an in-game set reports what this game applied (game_started and game_result).
		json_value picks() const
		{
			const auto& m = output.identity();
			const bool chosen = in_game(m) && observed;
			return json_value::object({{"gameNumber", json_value::number(m.game_number)}, {"stageId", json_value::number(chosen ? observed->stage : m.stage_id)},
				{"leftCharacterRow", json_value::number(chosen ? observed->left_row : m.left.row)},
				{"rightCharacterRow", json_value::number(chosen ? observed->right_row : m.right.row)}});
		}
		std::optional<pbd::rematch::sealed_state> state_at(u32 end_frame) const
		{
			if (!observed || !observed->verified)
				return {};
			const auto& p = *observed;
			pbd::rematch::sealed_state value{output.identity().game_number, end_frame, rematch_epoch,
				{{p.left_row, p.right_row}, p.stage}, {p.rounds, p.time_index, p.handicap}};
			return pbd::rematch::valid(value) ? std::optional{value} : std::nullopt;
		}
		json_value start_payload() const
		{
			auto payload = json_value::object({{"gameNumber", json_value::number(output.identity().game_number)},
				{"cleanStart", json_value::object({{"seed", json_value::number(clean.seed)},
								   {"checkpointCompatibility", json_value::string(pbd::checkpoint_compatibility::key)},
								   {"recipeSha256", json_value::string(clean.recipe_sha256)}, {"titleId", json_value::string(clean.title_id)},
								   {"generation", json_value::number(clean.generation)}})},
				{"start", json_value::object({{"encoding", json_value::string("pbd-spectator-start-v1")},
							  {"kind", json_value::string(clean.natural ? "natural_vs" : "clean_character_select")},
							  {"seed", json_value::number(clean.seed)}, {"recipeSha256", json_value::string(clean.recipe_sha256)},
							  {"titleId", json_value::string(clean.title_id)}, {"checkpointSha256", json_value::string(clean.checkpoint_sha256)},
							  {"originBoundary", json_value::number(game_origin)}, {"originCounter", json_value::number(clean.counter)},
							  {"syncEncoding", json_value::string("pbd-sync-facts-v1")}, {"syncDefinition", json_value::string("pbd-peer-sync-v3")},
							  {"pairedEncoding", json_value::string("pbd-paired-pad-v1")}, {"appliedEncoding", json_value::string("pbd-applied-pad-v1")},
							  {"inputPolicy", json_value::object({{"sessionStartSkip", json_value::boolean(clean.start_skip)},
												  {"exitGate", json_value::boolean(clean.exit_gate)}, {"keyedDraws", json_value::boolean(clean.keyed_draws)},
												  {"banJinpachi", json_value::boolean(clean.ban_jinpachi)},
												  {"phaseDelay", json_value::boolean(clean.phase_delay)},
												  {"battleClockVersion", json_value::number(clean.battle_clock_version)}, {"menuHashMask", json_value::number(clean.menu_hash_mask)}})}})}});
			if (clean.direct_start)
				payload.fields.back().second.fields.back().second.fields.emplace_back("directStart", json_value::boolean(true));
			if (clean.publish_from_start)
				payload.fields.back().second.fields.emplace_back("roundWitnessRequired", json_value::boolean(true));
			if (current_load)
				payload.fields.back().second.fields.emplace_back("load", pbd::rematch::load_json<json_value>(*current_load));
			// A watcher writes the same values into its own game at the same points. Taken as the game began, so its game_preparing
			// and its game_started name the same ones.
			if (rank_values && rank_values->any())
				payload.fields.back().second.fields.emplace_back("rankScreens", pbd::rank_stakes::values_json(*rank_values));
			return payload;
		}
		bool prepare_stream()
		{
			rank_values = std::exchange(next_rank_values, std::nullopt);
			// Selection can be replayed from this verified origin. Its actual round call is still checked and published later.
			return !clean.publish_from_start || event("game_preparing", start_payload());
		}
		bool flush()
		{
			if (pads.empty() || (!started && !clean.publish_from_start))
				return true;
			if (!stream_enabled || !output.identity().publish_confirmed_inputs)
			{
				pads.clear();
				syncs.clear();
				paired_pads.clear();
				applied_pads.clear();
				chunk_first = frame;
				return true;
			}
			const auto& m = output.identity();
			for (std::size_t offset = 0; offset < pads.size();)
			{
				const auto count = std::min<std::size_t>(confirmed_chunk_frames * 8, pads.size() - offset);
				const std::vector<std::uint8_t> chunk(pads.begin() + static_cast<std::ptrdiff_t>(offset), pads.begin() + static_cast<std::ptrdiff_t>(offset + count));
				const auto sync_offset = offset / 8 * 48, sync_count = count / 8 * 48;
				const std::vector<std::uint8_t> facts(syncs.begin() + static_cast<std::ptrdiff_t>(sync_offset), syncs.begin() + static_cast<std::ptrdiff_t>(sync_offset + sync_count));
				const std::vector<std::uint8_t> pair(paired_pads.begin() + static_cast<std::ptrdiff_t>(offset * 2), paired_pads.begin() + static_cast<std::ptrdiff_t>((offset + count) * 2));
				const std::vector<std::uint8_t> applied(applied_pads.begin() + static_cast<std::ptrdiff_t>(offset * 2), applied_pads.begin() + static_cast<std::ptrdiff_t>((offset + count) * 2));
				if (!event("confirmed_inputs", json_value::object({{"gameNumber", json_value::number(m.game_number)},
												   {"side", json_value::string(m.local_side == side::left ? "left" : "right")}, {"firstFrame", json_value::number(chunk_first + offset / 8)},
												   {"frameCount", json_value::number(count / 8)}, {"encoding", json_value::string("pbd-pad-v1")},
												   {"data", json_value::string(base64url(chunk))}, {"digest", json_value::string(digest(chunk))},
												   {"pairedData", json_value::object({{"encoding", json_value::string("pbd-paired-pad-v1")},
																	  {"data", json_value::string(base64url(pair))}, {"digest", json_value::string(digest(pair))}})},
												   {"appliedData", json_value::object({{"encoding", json_value::string("pbd-applied-pad-v1")},
																	   {"data", json_value::string(base64url(applied))}, {"digest", json_value::string(digest(applied))}})},
												   {"syncFacts", json_value::object({{"encoding", json_value::string("pbd-sync-facts-v1")},
																	 {"definition", json_value::string("pbd-peer-sync-v3")}, {"data", json_value::string(base64url(facts))},
																	 {"digest", json_value::string(digest(facts))}})}})))
					return false;
				offset += count;
			}
			pads.clear();
			syncs.clear();
			paired_pads.clear();
			applied_pads.clear();
			chunk_first = frame;
			return true;
		}
		void append_start_record(json_value& payload, u32 observed_boundary)
		{
			if (pbd_start_record)
			{
				try
				{
					const auto bytes = pbd_start_record();
					if (bytes && bytes->size() <= 65536)
					{
						auto parsed = detail::json_parser(*bytes, 16384, 65536, 65536).parse();
						u32 source_game = 0, boundary = 0, seed = 0;
						const auto* cap = parsed.value ? parsed.value->find("capsule") : nullptr;
						const auto* generation = parsed.value ? parsed.value->find("generation") : nullptr;
						if (cap && generation && generation->kind == json_kind::number && generation->text == std::to_string(clean.generation) &&
							detail::is(parsed.value->find("type"), "game_start") && detail::is(parsed.value->find("source"), "live-game") &&
							detail::number32(parsed.value->find("game"), 1, 65535, source_game) &&
							detail::number32(parsed.value->find("boundary"), game_origin, observed_boundary, boundary) &&
							detail::number32(cap->find("sessionSeed"), 1, 0xffffffffu, seed) && seed == clean.seed &&
							detail::is(cap->find("schema"), "pbd-game-start-capsule-draft-2"))
						{
							payload.fields.emplace_back("gameStartCapsule", std::move(*parsed.value));
							if (pbd_source_emulator.size() == 64 && std::all_of(pbd_source_emulator.begin(), pbd_source_emulator.end(),
																		[](char c)
																		{
																			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
																		}))
								payload.fields.emplace_back("gameStartEmulatorSha256", json_value::string(pbd_source_emulator));
						}
					}
				}
				catch (...)
				{
					// An unavailable optional start record preserves the ordinary game and full-stream watch.
				}
			}
		}

		bool start_game(const sample& value)
		{
			const auto& m = output.identity();
			const auto& o = value.phase;
			const auto& p = value.picks;
			if (in_game(m))
			{
				// The players chose in the game's own select: what the game applied is the game's picks, within the native tables,
				// under the set's three-round rule. Both games applied the same values: their battle states are hash-checked.
				if (!p.verified || p.rounds != m.rounds_per_game || p.stage > max_stage_id || p.left_row > max_character_row || p.right_row > max_character_row)
					return refuse("applied_selection_unverified", "The fighters, stage and three-round rule the game applied could not be read.");
			}
			else if (!p.verified || p.stage != m.stage_id || p.left_row != m.left.row || p.right_row != m.right.row || p.rounds != m.rounds_per_game)
				return refuse("applied_selection_mismatch", "The game did not apply the agreed fighters, stage and three-round rule.");
			if (!m.allow_jinpachi && (p.left_row >= 81 || p.right_row >= 81))
				return refuse("applied_character_rule_mismatch", "The game did not apply the character rule.");
			if (p.handicap[0] != full_handicap || p.handicap[1] != full_handicap)
				return refuse("applied_handicap_uneven", "Both life bars must start full in a lobby game (applied " + std::to_string(p.handicap[0]) +
															 "/" + std::to_string(p.handicap[1]) + ").");
			observed = p;
			// A repick's chooser picked in the select (its row is checked like any in-game pick above); everything else is frozen.
			if (current_load && (!pbd::rematch::matches(*current_load, {{p.left_row, p.right_row}, p.stage}) ||
									current_load->rules != pbd::rematch::options{p.rounds, p.time_index, p.handicap}))
				return refuse("rematch_applied_state_changed", "The game did not apply the frozen Rematch choices.");
			if (clean.require_round_state && (!p.round_verified || p.pending_phase != 4 || p.round_ordinal > 8))
				return refuse("native_round_start_unverified", "The native round call and random words could not be read.");
			auto payload = picks();
			auto identity = start_payload();
			for (std::size_t i = 1; i < identity.fields.size(); ++i)
				payload.fields.push_back(std::move(identity.fields[i]));
			if (clean.require_round_state)
			{
				auto* start = &payload.fields.back().second;
				start->fields.emplace_back("nativeRound", json_value::object({{"encoding", json_value::string("pbd-native-round-start-v1")},
															  {"pendingPhase", json_value::number(p.pending_phase)}, {"roundOrdinal", json_value::number(p.round_ordinal)},
															  {"randomWords", json_value::array({json_value::number(p.round_random[0]), json_value::number(p.round_random[1])})}}));
			}
			append_start_record(payload, o.boundary);
			if (!event("game_started", std::move(payload)))
				return false;
			started = true;
			return true;
		}

		bool finish_game(const pbd::match_recording::event& row, u32 confirmed_through)
		{
			using namespace pbd::match_recording;
			const auto& m = output.identity();
			// The lifecycle admits a draw only when both fighters reached the rounds to win on the same final round.
			const bool drawn = row.outcome == winner::draw;
			if (row.outcome != winner::left && row.outcome != winner::right && !drawn)
				return refuse("native_result_unverified", "The native match phases or result could not be verified.");
			if (!flush())
				return false;
			auto payload = picks();
			const auto winning_side = row.outcome == winner::left ? "left" : "right";
			// A drawn game names no winner and does not count: the score stays, and the next continuation replays this game number.
			if (drawn)
			{
				payload.fields.emplace_back("winner", json_value{});
				payload.fields.emplace_back("outcome", json_value::string("draw"));
			}
			else
				payload.fields.emplace_back("winner", json_value::string(winning_side));
			payload.fields.emplace_back("roundWins", json_value::object({{"left", json_value::number(row.score_left)}, {"right", json_value::number(row.score_right)}}));
			// This watermark describes executed digital updates, including presentation, rather than a live room score.
			payload.fields.emplace_back("finalFrame", json_value::number(frame));
			const auto rematch_state = state_at(frame);
			if (!rematch_state)
				return refuse("rematch_state_unverified", "The completed game settings could not be read.");
			payload.fields.emplace_back("rematchState", pbd::rematch::state_json<json_value>(*rematch_state));
			if (pbd_performance)
				if (const auto timing = pbd_performance(game_origin, row.boundary))
					payload.fields.emplace_back("performance", json_value::object({{"schema", json_value::string("pbd-fight-timing-1")},
																   {"fightFrames", json_value::number(timing->fight_frames)},
																   {"onTimeFrames", json_value::number(timing->on_time_frames)}}));
			if (!event("game_result", std::move(payload)))
				return false;
			if (row.outcome == winner::left)
				++games.left;
			else if (row.outcome == winner::right)
				++games.right;
			const bool terminal = !drawn && (games.left == m.first_to_games || games.right == m.first_to_games);
			if (terminal)
			{
				if (!event("set_ended", json_value::object({{"winner", json_value::string(winning_side)},
											{"score", json_value::object({{"left", json_value::number(games.left)}, {"right", json_value::number(games.right)}})},
											{"reason", json_value::string("completed")}})))
					return false;
			}
			else if (!event("between_games_ready", json_value::object({{"gameNumber", json_value::number(m.game_number)}})))
				return false;
			// Only a confirmed observation reaches here, with every required chunk/result/tail record accepted in serial order.
			// The background writer publishes those records; shutdown drains pending output.
			// Paused or opted-out spectator publication remains an explicit stream policy, not invented input coverage.
			sealed = sealed_game_result{m.game_number, game_origin, row.boundary, confirmed_through, frame,
				row.score_left, row.score_right, games, row.outcome, clean.generation, output.last_event(),
				terminal, stream_enabled && m.publish_confirmed_inputs};
			return true;
		}

		bool consume(const sample& value, u32 confirmed_through)
		{
			using namespace pbd::match_recording;
			const auto& o = value.phase;
			if (!started && (clean.require_round_state ? o.native_phase == phase::round_intro :
														 (o.native_phase == phase::character_intro || o.native_phase == phase::round_intro || o.native_phase == phase::fight)))
			{
				if (!start_game(value))
					return false;
			}
			const auto rows = lifecycle.observe(o);
			if (rows.refused != refusal::none)
			{
				rejected = rejected_observation{o, value.picks, rows.refused};
				return refuse("native_result_unverified", "The native match phases or result could not be verified.");
			}
			for (u32 i = 0; i < rows.count; ++i)
			{
				const auto& row = rows.values[i];
				if (row.type != kind::match_end)
					continue;
				if (!finish_game(row, confirmed_through))
					return false;
			}
			return true;
		}

	public:
		driver(channel& target, std::function<std::string(const std::vector<std::uint8_t>&)> hash,
			std::function<std::shared_ptr<const std::string>()> start_record = {}, std::string source_emulator = {},
			std::function<std::optional<pbd::fight_timing::result>(u32, u32)> performance = {})
			: output(target), games(target.identity().games), digest(std::move(hash)), pbd_start_record(std::move(start_record)),
			  pbd_source_emulator(std::move(source_emulator)), pbd_performance(std::move(performance)) {}
		bool healthy() const
		{
			return !failed && output.healthy();
		}
		bool active() const
		{
			return admitted && healthy() && lifecycle.active();
		}
		bool complete() const
		{
			return lifecycle.complete();
		}
		bool round_closed() const
		{
			return lifecycle.round_closed();
		}
		std::optional<pbd::rematch::sealed_state> rematch_state() const
		{
			return sealed ? state_at(sealed->final_frame) : std::nullopt;
		}
		bool match_decided() const
		{
			return lifecycle.match_decided();
		}
		pbd::match_recording::refusal result_refusal() const
		{
			return lifecycle.refused();
		}
		std::optional<rejected_observation> last_refusal() const
		{
			return rejected;
		}
		std::optional<pbd::session_health::progress> confirmed_progress() const
		{
			const auto wins = lifecycle.round_wins();
			if (!started || !active() || !wins)
				return {};
			return pbd::session_health::progress{output.identity().game_number, *wins};
		}
		std::optional<sealed_game_result> sealed_game() const
		{
			return healthy() ? sealed : std::nullopt;
		}
		bool refuse(std::string_view code, std::string_view detail)
		{
			if (!failed)
				output.write_event("contract_error", json_value::object({{"code", json_value::string(std::string(code))}, {"detail", json_value::string(std::string(detail))}}));
			failed = true;
			return false;
		}
		bool begin(u32 origin, u32 counter, clean_start identity)
		{
			if (admitted || failed)
				return false;
			clean = std::move(identity);
			if (clean.publish_from_start && !clean.require_round_state)
				return refuse("native_round_start_required", "Watching from selection needs the native round start check.");
			if (!clean.selection_admitted)
				return refuse("selection_driver_unavailable", "Lobby fighter and stage selection is Coming soon.");
			if (!clean.verified)
				return refuse("clean_start_unverified", "A fresh verified checkpoint and synchronized seed are required.");
			if (clean.ban_jinpachi != !output.identity().allow_jinpachi)
				return refuse("start_character_rule_mismatch", "The game did not apply the character rule.");
			next = game_origin = origin;
			clean.counter = counter;
			const auto rows = lifecycle.begin(output.identity().game_number, origin, counter, output.identity().rounds_per_game, true);
			if (rows.refused != pbd::match_recording::refusal::none)
				return refuse("clean_start_unverified", "The native clean-start rule was refused.");
			admitted = true;
			return prepare_stream();
		}
		bool record(pbd::match_recording::observation phase, context applied)
		{
			if (!active())
				return healthy();
			if (phase.boundary < next)
				return refuse("confirmed_observation_changed", "An already confirmed update changed.");
			if (pending.size() >= 4096 && !pending.contains(phase.boundary))
				return refuse("confirmation_window_exceeded", "The confirmed-input window exceeded its bound.");
			pending.insert_or_assign(phase.boundary, sample{phase, applied});
			return true;
		}
		template <typename Executed, typename Pad, typename Facts>
		bool publish_through(u32 through, Executed executed, Pad pad, Facts facts)
		{
			if (!active())
				return healthy();
			for (; next < through; ++next)
			{
				auto found = pending.find(next);
				if (found == pending.end())
				{
					if (executed(next))
						return refuse("confirmed_phase_missing", "A confirmed update has no verified native observation.");
					continue;
				}
				const auto value = found->second;
				pending.erase(found);
				if (!consume(value, through))
					return false;
				if (complete())
					return true;
				const auto words = pad(next);
				if (!words.verified)
					return refuse("confirmed_pad_readback_missing", "The confirmed input has no verified two-port native post.");
				const auto encoded = pad_record(words.raw[output.identity().local_side == side::left ? 0 : 1]);
				for (const auto word : words.raw)
				{
					const auto record = pad_record(word);
					paired_pads.insert(paired_pads.end(), record.begin(), record.end());
				}
				for (const auto word : words.applied)
				{
					const auto record = pad_record(word);
					applied_pads.insert(applied_pads.end(), record.begin(), record.end());
				}
				const auto native = facts(next);
				if (!native.verified || native.boundary != next || native.mask > 7 || native.gate_hold > 1)
					return refuse("confirmed_sync_facts_missing", "The confirmed input has no native peer sync evidence.");
				const auto sync = sync_record(native);
				syncs.insert(syncs.end(), sync.begin(), sync.end());
				pads.insert(pads.end(), encoded.begin(), encoded.end());
				++frame;
				if ((!started && frame > presentation_limit(output.identity())) || ((started || clean.publish_from_start) && pads.size() >= confirmed_chunk_frames * 8 && !flush()))
					return refuse("presentation_buffer_exceeded", "The verified match presentation exceeded its input buffer.");
			}
			// The natural scene exit is the exclusive end. It does not contain a new match input record.
			const auto end = pending.find(through);
			if (end != pending.end() && end->second.phase.native_phase == pbd::match_recording::phase::match_end)
			{
				const auto value = end->second;
				pending.erase(end);
				return consume(value, through);
			}
			return true;
		}
		// The values the game's own Ranked screens write in the next game this driver begins (none without them): given before its
		// begin, continue_game or next_set, so its game_preparing, its game_started and its fight all have the same values.
		void rank_screens(std::optional<pbd::rank_stakes::game_values> values)
		{
			next_rank_values = values;
		}
		bool apply_stream_status(const command& value)
		{
			if (value.kind != command_kind::stream_status || !healthy())
				return false;
			if (!flush())
				return false;
			// Publication pressure changes only this side's spectator stream, never its peer input or game progression.
			stream_enabled = value.status == "active";
			return output.acknowledge(value);
		}
		// A deliberate quit gives up this player's seat; the launcher sends it to the lobby as set.leave leave_seat. Only a healthy
		// set writes it, so a refused contract (which already wrote contract_error) or a failed channel never becomes a forfeit.
		// Success accepts the event in serial order; the background writer publishes it and shutdown drains pending output.
		bool leave_seat(std::string_view cause)
		{
			if (!healthy())
				return false;
			return event("leave", json_value::object({{"action", json_value::string("leave_seat")}, {"cause", json_value::string(std::string(cause))}}));
		}
		// The player left while this PC waited for the other PC's game to load or answer (the waiting box's Leave, offered after 30 s).
		// The launcher sends it to the lobby as set.leave peer_wait, which ends the set with no result: never a forfeit, so a game
		// that does not load cannot cost the waiting player the set. A healthy set accepts it for publication like leave_seat.
		bool leave_peer_wait()
		{
			if (!healthy())
				return false;
			return event("leave", json_value::object({{"action", json_value::string("peer_wait")}}));
		}
		bool continue_game(const command& value, u32 origin, u32 counter)
		{
			// After a draw the only continuation is the replay of the same game number with the score it had; after a decided game it
			// is the next number with the score that game made. A replay marker after a decided game, or none after a draw, is refused.
			const bool drawn = sealed && sealed->outcome == pbd::match_recording::winner::draw;
			if (!healthy() || !complete() || !sealed || sealed->terminal || origin != sealed->end_boundary ||
				value.kind != command_kind::continue_game || value.replay != drawn || value.game_number != sealed->game_number + (drawn ? 0u : 1u) ||
				value.games.left != sealed->games.left || value.games.right != sealed->games.right)
				return refuse("natural_continuation_unverified", "The next game does not match the confirmed set result.");
			if (current_load && !value.rematch)
				return refuse("rematch_load_missing", "The next game lost its Rematch record.");
			if (clean.direct_start && (!value.rematch || value.rematch->repick))
				return refuse("direct_start_load_required", "A matched game needs the agreed fighters and stage for a direct start.");
			if (value.rematch)
			{
				const auto previous = rematch_state();
				// Only the player who lost the completed game may pick again (a draw has no loser and validate refuses its repick).
				const u32 loser = sealed->outcome == pbd::match_recording::winner::left ? 1u : 0u;
				if (!previous || pbd::rematch::validate(*value.rematch, *previous, drawn) != pbd::rematch::error::none ||
					(value.rematch->repick && *value.rematch->repick != loser))
					return refuse("rematch_load_changed", "The Rematch does not match the completed game.");
			}
			if (!output.continue_naturally(value))
				return refuse("natural_continuation_channel_failed", "The next game could not be acknowledged.");
			current_load = value.rematch;
			if (current_load)
				rematch_epoch = current_load->epoch;
			return begin_natural(value.game_number, origin, counter, "natural_continuation_unverified");
		}
		// Join Battle against the same opponent: the next set starts at the completed set's natural end, in the same session, and
		// its first game uses either stock select or a sealed Rematch load. Only a terminal result admits it. A load carries its
		// verified epoch across the set identity change. The session's seed, options and delay stay.
		bool next_set(const command& value, u32 origin, u32 counter)
		{
			if (!healthy() || !complete() || !sealed || !sealed->terminal || origin != sealed->end_boundary ||
				value.kind != command_kind::next_set || value.game_number != 1 || value.games.left || value.games.right)
				return refuse("next_set_unverified", "The next set does not follow a completed set.");
			const auto previous = rematch_state();
			if (clean.direct_start && (!value.rematch || value.rematch->repick))
				return refuse("direct_start_load_required", "A matched set needs the agreed fighters and stage for a direct start.");
			if (value.rematch && (!previous || pbd::rematch::validate(*value.rematch, *previous, false, true) != pbd::rematch::error::none))
				return refuse("rematch_load_changed", "The next set does not match the completed game's Rematch state.");
			if (!output.next_set(value))
				return refuse("next_set_channel_failed", "The next set could not be acknowledged.");
			games = {};
			current_load = value.rematch;
			rematch_epoch = current_load ? current_load->epoch : 0;
			return begin_natural(1, origin, counter, "next_set_unverified");
		}

	private:
		// The next game's presentation starts at the previous game's natural end, which the stock flow loads from (natural_vs).
		bool begin_natural(u32 game, u32 origin, u32 counter, std::string_view refusal)
		{
			lifecycle = {};
			pending.clear();
			pads.clear();
			syncs.clear();
			paired_pads.clear();
			applied_pads.clear();
			sealed.reset();
			observed.reset();
			clean.natural = true;
			clean.counter = counter;
			next = game_origin = origin;
			frame = chunk_first = 0;
			started = false;
			const auto rows = lifecycle.begin(game, origin, counter, output.identity().rounds_per_game, true);
			if (rows.refused != pbd::match_recording::refusal::none)
				return refuse(refusal, "The natural VS origin could not be admitted.");
			return prepare_stream();
		}
	};
} // namespace pbd::lobby_set
