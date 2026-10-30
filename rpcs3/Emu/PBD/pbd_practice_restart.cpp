#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_practice_lesson_channel.h"

std::optional<replay_control_result> pbd::rollback_session::practice_steps::apply_reset()
{
	if (data.practice.restart.reset_request)
	{
		const auto observed = read_practice_pause();
		// A restart that cannot run is dropped with a log line and a toast, and the fight continues; it is never dropped
		// without the player seeing it.

		if (!data.session.in_match)
			abandon_reset("left_match");
		else if (session_clock::now() >= data.practice.restart.reset_deadline)
			abandon_reset("deadline");
		// Resume acknowledgement can precede a quiescent PPU boundary. Keep input claimed and the request pending
		// until its existing execution guard admits it.
		else if (observed.valid && !observed.paused && pbd::game_lifecycle::boundary_ready(ppu))
		{
			auto command = *data.practice.restart.reset_request;
			data.practice.restart.reset_request.reset();
			const auto receipt = pbd::game_lifecycle::execute(ppu, command);
			if (pbd::practice::lesson::current.reset == command.request_id && (receipt.completed || receipt.attempted))
				pbd::practice::lesson::current.finish(receipt.completed ? "complete" : "not_ready");
			log_reset_receipt(command, receipt);
			if (receipt.completed)
				complete_reset(command, receipt);
			else if (receipt.attempted)
			{
				// The kind-20 reset itself was entered and did not complete, so the fight may be part-way through a
				// reset. That is contained by retiring this private session, never by replaying the request.
				practice_dummy_log(fmt::format("{\"type\":\"practice_reset_retired\",\"request_id\":%u,\"boundary\":%u,\"reason\":\"%s\"}\n",
					receipt.request_id, index, receipt.reason));
				pbd_session_log.error("Practice: Restart %u failed inside the native reset (%s) at boundary %u; ending Practice.",
					receipt.request_id, receipt.reason, index);
				leave();
				return replay_control_result::stopped;
			}
			else
			{
				// The reset itself was not entered (at most the statistics clear ran, and a refused clear leaves the
				// fight as it was), so nothing was half-applied. A refusal that can clear by waiting keeps the
				// request pending for the next eligible boundaries; the player's input stays withheld meanwhile.
				data.practice.restart.reset_request = command;
				if (data.practice.restart.reset_retry_from == umax || index < data.practice.restart.reset_retry_from)
					data.practice.restart.reset_retry_from = index;
				const u32 waited = index - data.practice.restart.reset_retry_from;
				const bool retry = pbd::practice::controls::restart_retryable(receipt.reason) &&
				                   waited < pbd::practice::controls::restart_retry_frames;
				// The first refusal, every 30th boundary of the wait and the last, so a wait through a whole throw names what
				// held the restart without a row per frame.
				if (!retry || waited % 30 == 0)
					practice_dummy_log(fmt::format("{\"type\":\"practice_reset_not_taken\",\"request_id\":%u,\"boundary\":%u,\"reason\":\"%s\",\"statistics_attempted\":%s,\"retry\":%s,\"retry_from\":%u,\"waited\":%u}\n",
						receipt.request_id, index, receipt.reason, json_bool(receipt.statistics_attempted), json_bool(retry), data.practice.restart.reset_retry_from, waited));
				if (!retry)
					abandon_reset(receipt.reason);
			}
			if (!data.practice.restart.reset_request)
			{
				data.practice.dummy.ignore_until_release = true;
				data.practice.dummy.mask_ui = true;
			}
			publish();
		}
	}
	return std::nullopt;
}

void pbd::rollback_session::practice_steps::abandon_reset(const char* reason)
{
	if (pbd::practice::lesson::current.reset == data.practice.restart.reset_request->request_id)
		pbd::practice::lesson::current.finish("not_ready");
	practice_dummy_log(fmt::format("{\"type\":\"practice_reset_abandoned\",\"request_id\":%u,\"boundary\":%u,\"reason\":\"%s\"}\n",
		data.practice.restart.reset_request->request_id, index, reason));
	pbd_session_log.warning("Practice: Restart %u abandoned at boundary %u (%s).", data.practice.restart.reset_request->request_id, index, reason);
	if (std::strcmp(reason, "left_match") != 0)
	{
		data.practice.view.toast = "The restart didn't run\tTry it again";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
	}
	data.practice.restart.reset_request.reset();
	data.practice.restart.reset_retry_from = umax;
	data.practice.dummy.ignore_until_release = true;
	data.practice.dummy.mask_ui = true;
}

void pbd::rollback_session::practice_steps::complete_reset(const pbd::game_lifecycle::practice_reset& command, const pbd::game_lifecycle::receipt& receipt)
{
	data.practice.history.reset();
	data.practice.history_pending = false;
	data.practice.health_valid = false;
	data.practice.view.fighters = {};
	data.practice.combos = {};
	data.practice.view.fight_live = false;
	data.practice.streak.reset();
	data.practice.precision.reset();
	data.practice.punish.reset();
	data.practice.record_word_valid = false;
	data.practice.play_before.valid = false;
	data.practice.dummy.play_pos = 0;
	data.practice.dummy.wait = 0;
	data.practice.dummy.have_play_slot = false;
	data.practice.movement.pos = 0;
	data.practice.dummy.neutral_run = 0;
	practice_capture_mark("restart", index);
	if (command.target >= pbd::game_lifecycle::placement::left_wall)
	{
		data.practice.restart.sides_request = receipt.request_id;
		data.practice.restart.sides_target = static_cast<u32>(command.target);
		data.practice.restart.sides_logged = 0;
		data.practice.restart.sides_armed = false;
		data.practice.restart.sides_provisional = receipt.provisional_placement;
		if (receipt.phase_sides)
			practice_sides_row(index, 0, "wall_phase", *receipt.phase_sides, receipt.provisional_placement);
	}
	practice_restart_contact();
	if (data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview &&
		!practice_dummy_begin_routine())
		data.practice.dummy.state = dummy_state::idle;
	const auto after = read_practice_pause();
	data.practice.boundary_counter_valid = after.valid;
	data.practice.boundary_counter = after.counter;
	data.practice.restart.reset_retry_from = umax;
}

void pbd::rollback_session::practice_steps::log_reset_receipt(const pbd::game_lifecycle::practice_reset& command, const pbd::game_lifecycle::receipt& receipt)
{
	practice_dummy_log(fmt::format("{\"type\":\"practice_reset_receipt\",\"request_id\":%u,\"boundary\":%u,\"stage\":%u,\"stage_generation\":%u,\"pair_generation\":%u,\"native_kind\":%u,\"target\":%u,\"switched\":%s,\"dummy_back\":%s,\"statistics_attempted\":%s,\"attempted\":%s,\"completed\":%s,\"preserved\":%s,\"readback\":%s,\"placement_phase\":%s,\"neutral_verified\":%s,\"counter_before\":%u,\"counter_after\":%u,\"reason\":\"%s\",\"scope_expected\":[%u,%u,%u,%u],\"scope_observed\":[%u,%u,%u,%u],\"applied_positions\":[[%u,%u,%u,%u,%u],[%u,%u,%u,%u,%u]],\"positions\":[[%u,%u,%u,%u,%u],[%u,%u,%u,%u,%u]]}\n",
		receipt.request_id, index, command.lifetime.stage, receipt.stage_generation, receipt.pair_generation, receipt.native_kind,
		static_cast<u32>(command.target), json_bool(command.variant.switched), json_bool(command.variant.dummy_back),
		json_bool(receipt.statistics_attempted), json_bool(receipt.attempted),
		json_bool(receipt.completed), json_bool(receipt.preserved),
		json_bool(receipt.readback), json_bool(receipt.placement_phase), json_bool(receipt.neutral_verified),
		receipt.counter_before, receipt.counter_after, receipt.reason,
		command.lifetime.emulation_generation, command.lifetime.stage, command.lifetime.characters[0], command.lifetime.characters[1],
		receipt.observed_lifetime.emulation_generation, receipt.observed_lifetime.stage, receipt.observed_lifetime.characters[0], receipt.observed_lifetime.characters[1],
		static_cast<u32>(receipt.applied_positions[0].x), static_cast<u32>(receipt.applied_positions[0].y), static_cast<u32>(receipt.applied_positions[0].z),
		static_cast<u16>(receipt.applied_positions[0].facing), receipt.applied_positions[0].yaw_bits,
		static_cast<u32>(receipt.applied_positions[1].x), static_cast<u32>(receipt.applied_positions[1].y), static_cast<u32>(receipt.applied_positions[1].z),
		static_cast<u16>(receipt.applied_positions[1].facing), receipt.applied_positions[1].yaw_bits,
		static_cast<u32>(receipt.positions[0].x), static_cast<u32>(receipt.positions[0].y), static_cast<u32>(receipt.positions[0].z),
		static_cast<u16>(receipt.positions[0].facing), receipt.positions[0].yaw_bits,
		static_cast<u32>(receipt.positions[1].x), static_cast<u32>(receipt.positions[1].y), static_cast<u32>(receipt.positions[1].z),
		static_cast<u16>(receipt.positions[1].facing), receipt.positions[1].yaw_bits));
}

void pbd::rollback_session::practice_steps::practice_wall_report()
{
	if (!pbd::game_lifecycle::requested() || !data.session.in_match || !data.practice.native.pause_valid)
		return;
	const auto lifetime = lifecycle_scope();
	if (lifetime.stage == ~0u)
		return;
	if (lifetime != data.practice.restart.wall_scope)
	{
		data.practice.restart.wall_scope = lifetime;
		data.practice.restart.wall_reported = false;
		data.practice.restart.wall_attempts = 0;
		data.practice.restart.wall_wait = 0;
	}
	if (data.practice.restart.wall_reported)
		return;
	// Counted in calls rather than boundary numbers, which a Practice rewind can move backwards.
	if (data.practice.restart.wall_wait)
	{
		--data.practice.restart.wall_wait;
		return;
	}
	data.practice.restart.wall_wait = 30;
	const auto report = pbd::game_lifecycle::survey_walls(lifetime);
	if (!report.read && ++data.practice.restart.wall_attempts < 20)
		return;
	data.practice.restart.wall_reported = true;
	const bool general = std::strcmp(report.path, "general") == 0;
	const bool all = std::all_of(report.available.begin(), report.available.end(), [](bool value)
		{
			return value;
		});
	const auto dump = write_wall_polygons(lifetime, report, general, all);
	const auto& count = report.counts;
	const auto offered = [&](u32 at)
	{
		return report.available[at] ? "yes" : "no";
	};
	pbd_session_log.warning("Practice walls: stage %u at boundary %u, collision %s: %u polygons, %u walls, %u floors; skipped %u inactive, %u malformed, %u sloped, %u misshapen, %u off the floor, %u facing away (kept as obstacles); path %s%s%s; Left Wall %s, Right Wall %s, Foreground Wall %s, Background Wall %s%s%s.",
		lifetime.stage, index, report.read ? "read" : "not read", count.polygons, count.walls, count.floors,
		count.inactive, count.malformed, count.sloped, count.shape, count.off_floor, count.facing_away,
		report.path, *report.reason ? " - " : "", report.reason, offered(0), offered(1), offered(2), offered(3),
		dump.empty() ? "" : "; polygons in ", dump.c_str());
	practice_dummy_log(fmt::format("{\"type\":\"practice_walls\",\"boundary\":%u,\"stage\":%u,\"read\":%s,\"reason\":\"%s\",\"path\":\"%s\",\"polygons\":%u,\"walls\":%u,\"floors\":%u,\"skipped\":{\"inactive\":%u,\"malformed\":%u,\"sloped\":%u,\"shape\":%u,\"off_floor\":%u},\"facing_away\":%u,\"available\":[%s,%s,%s,%s],\"dump\":\"%s\"}\n",
		index, lifetime.stage, json_bool(report.read), report.reason, report.path, count.polygons, count.walls, count.floors,
		count.inactive, count.malformed, count.sloped, count.shape, count.off_floor, count.facing_away,
		json_bool(report.available[0]), json_bool(report.available[1]), json_bool(report.available[2]), json_bool(report.available[3]),
		dump.c_str()));
}

std::string pbd::rollback_session::practice_steps::write_wall_polygons(const pbd::game_lifecycle::scope& lifetime, const pbd::game_lifecycle::wall_report& report, bool general, bool all)
{
	// Dump the bounded raw polygons and exact float bits when wall detection uses the general path or misses a position.
	// This diagnostic is optional; file failures must leave Practice running.
	std::string dump;
	if (report.read && (general || !all) && !data.checkpoint.capture_directory.empty())
		try
		{
			const auto name = fmt::format("practice-walls-stage-%u.json", lifetime.stage);
			const auto path = (data.checkpoint.capture_directory / name).u8string();
			if (fs::file out(std::string(path.begin(), path.end()), fs::write_new + fs::isfile); out)
			{
				const auto number = [](float value)
				{
					return std::isfinite(value) ? fmt::format("%.9g", static_cast<double>(value)) : std::string("null");
				};
				std::string text = fmt::format("{\"schema\":1,\"stage\":%u,\"path\":\"%s\",\"reason\":\"%s\",\"objects\":%u,\"digest\":\"%016x\",\"polygons\":[\n",
					lifetime.stage, report.path, report.reason, report.objects, report.digest);
				text.reserve(text.size() + report.raw.size() * 640 + 4);
				for (usz at = 0; at < report.raw.size(); ++at)
				{
					const auto& polygon = report.raw[at];
					std::array<u32, 20> bits{};
					for (u32 k = 0; k < bits.size(); ++k)
						bits[k] = std::bit_cast<u32>(k < 16 ? polygon.vertices[k / 4][k % 4] : polygon.normal[k - 16]);
					text += "{\"vertices\":[";
					for (u32 v = 0; v < 4; ++v)
					{
						text += v ? ",[" : "[";
						for (u32 k = 0; k < 4; ++k)
						{
							if (k)
								text += ',';
							text += number(polygon.vertices[v][k]);
						}
						text += ']';
					}
					text += "],\"normal\":[";
					for (u32 k = 0; k < 4; ++k)
					{
						if (k)
							text += ',';
						text += number(polygon.normal[k]);
					}
					text += "],\"active\":";
					text += json_bool(polygon.active);
					text += ",\"bits\":[";
					for (u32 k = 0; k < bits.size(); ++k)
					{
						if (k)
							text += ',';
						text += std::to_string(bits[k]);
					}
					text += at + 1 < report.raw.size() ? "]},\n" : "]}\n";
				}
				text += "]}\n";
				out.write(text.data(), text.size());
				dump = name;
			}
		}
		catch (...)
		{
			dump.clear();
		}

	return dump;
}
