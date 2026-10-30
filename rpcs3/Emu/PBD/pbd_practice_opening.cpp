#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_dummy_move_runtime.h"
#include "pbd_practice_lesson_channel.h"
#include "pbd_practice_frames.h"
#include "pbd_practice_start.h"

namespace start = pbd::practice::start;

std::optional<replay_control_result> pbd::rollback_session::practice_steps::opening_request()
{
	if (data.game_overlay.enabled && !data.peer.enabled)
	{
		static thread_local start::state entry;
		const u64 generation = static_cast<u64>(Emu.GetEmulationIdentifier());
		const auto now = session_clock::now();
		if (entry.root != data.game_overlay.dir || entry.generation != generation)
		{
			start::input_held = false;
			if (entry.wanted || !entry.refusal.empty())
				pbd::game_overlay::publish({});
			entry = {};
			entry.root = data.game_overlay.dir;
			entry.generation = generation;
		}

		inspect_opening_request(entry, now);
		if (entry.wanted || !entry.refusal.empty())
		{
			if (!entry.released)
				start::input_held = true;
			if (!entry.ready && entry.refusal.empty() && now >= entry.deadline)
				opening_refuse(entry, now, "practice_start_timeout");
			u32 mode = 0, scene = 0, pending = 0;
			const bool context = read_word(pbd::t5dr::game_mode_address, mode) && read_word(pbd::t5dr::scene_address, scene) &&
			                     read_word(pbd::t5dr::subscene_address, pending);
			if (context && mode != 10 && entry.refusal.empty())
				opening_refuse(entry, now, "practice_start_mode_changed");
			prepare_opening_selection(entry, now, context, mode, scene, pending);
			advance_opening_selection(entry, now, context, mode, scene, pending);
			publish_opening_result(entry, mode);
			if (!entry.ready || !entry.published)
			{
				if (!entry.refusal.empty())
					start::input_held = true;
				data.practice.dummy.mask_ui = true;
				pbd::practice::input_claim.store(true, std::memory_order_release);
				pbd::practice::publish({});
				auto card = pbd::game_overlay::selection_card({});
				card.title = entry.refusal.empty() ? "Starting Practice" : "Practice could not start";
				card.subtitle = entry.refusal.empty() ? "Loading your fighter and stage." : "Returning to the launcher.";
				pbd::game_overlay::publish(card);
				if (!entry.refusal.empty() && now - entry.failed_at >= std::chrono::seconds(3))
				{
					Emu.CallFromMainThread([]
						{
							Emu.Kill(true, false);
						});
					return replay_control_result::stopped;
				}
				return replay_control_result::go;
			}
			if (!entry.released)
			{
				entry.released = true;
				start::input_held = false;
				data.practice.dummy.mask_ui = false;
				data.practice.dummy.ignore_until_release = true;
				pbd::game_overlay::publish({});
			}
		}
	}
	return std::nullopt;
}

void pbd::rollback_session::practice_steps::inspect_opening_request(pbd::practice::start::state& entry, session_clock::time_point now)
{
	if (!entry.inspected)
	{
		entry.inspected = true;
		const auto file = entry.root / "practice-start.json";
		std::error_code ec;
		if (std::filesystem::exists(file, ec))
		{
			const auto text = pbd::json::detail::read_text(file);
			const auto parsed = text.value ? start::parse(*text.value) : pbd::json::result<start::request>{{}, text.error};
			if (parsed.value)
				entry.wanted = *parsed.value;
			else
				opening_refuse(entry, now, "invalid_practice_start");
			// A rewind or a fresh PPU thread cannot replay this one startup request.
			std::filesystem::rename(file, entry.root / "practice-start-consumed.json", ec);
			if (ec)
				opening_refuse(entry, now, "practice_start_consume_failed");
			entry.deadline = now + std::chrono::seconds(65);
		}
		else if (ec)
			opening_refuse(entry, now, "practice_start_read_failed");
	}
}

void pbd::rollback_session::practice_steps::prepare_opening_selection(pbd::practice::start::state& entry, session_clock::time_point now, bool context, u32 mode, u32 scene, u32 pending)
{
	if (entry.refusal.empty() && !entry.prepared && context && scene == 3 && pending == 44 &&
		pbd::practice::native::boundary_ready(ppu))
	{
		const auto tables = pbd::hidden_selection::read_native_tables();
		const auto chooser = pbd::hidden_selection::read_native_observation(0);
		if (tables && chooser && chooser->verified && chooser->grid_mode == 1)
		{
			u32 manager = 0, resource = 0;
			if (!pbd::hidden_selection::pinned_binary() || !read_word(0x1008f060, manager) || !read_word(0x1008f064, resource) ||
				!start::selection_boundary(mode, scene, pending, manager, resource))
				opening_refuse(entry, now, "practice_start_selector_owner");
			u32 default_dummy = 0;
			auto* memory = session_runtime.m_practice_memory.get();
			if (memory)
				memory->tables = *tables;
			if (memory && memory->saved.pick)
				default_dummy = memory->saved.pick->dummy_row;
			else if (!read_word(0x10093f54, default_dummy))
				default_dummy = entry.wanted->character_row;
			const auto selected = start::resolve(*entry.wanted, *tables, default_dummy);
			if (!selected)
				opening_refuse(entry, now, "practice_start_costume_unavailable");
			else if (entry.refusal.empty())
			{
				entry.loaded.wanted = *selected;
				entry.prepared = true;
			}
		}
	}
}

void pbd::rollback_session::practice_steps::advance_opening_selection(pbd::practice::start::state& entry, session_clock::time_point now, bool context, u32 mode, u32 scene, u32 pending)
{
	if (entry.prepared && !entry.ready && entry.refusal.empty())
	{
		if (!entry.load_requested && context && scene == 3 && pending == 44)
		{
			const auto& selected = entry.loaded.wanted;
			const pbd::replay_native::choices choices{selected.rows[0], selected.rows[1], selected.stage};
			const char* refusal = "practice_start_resource_request_failed";
			// Scheduler work raised after the owner is serviced waits until the setters return (see owner_guard).
			bool requested = pbd::replay_native::service_owner(ppu, entry.generation, &refusal);
			if (requested)
			{
				const pbd::replay_native::owner_guard admission(ppu);
				requested = pbd::replay_native::prepare_resources(ppu, entry.generation, choices, &refusal,
					pbd::replay_native::resource_mode::practice);
			}
			if (!requested)
				opening_refuse(entry, now, refusal);
			else
				entry.load_requested = true;
		}
		start::snapshot sample;
		sample.mode = mode;
		sample.scene = scene;
		// The round phase is separate from the scene manager's idle pending value 44.
		sample.models_ready = read_word(0x10093230, sample.pending) && read_word(0x1008f088, sample.stage) && read_word(0x1008f05c, sample.counter) &&
		                      read_word(0x10093f50, sample.rows[0]) && read_word(0x10093f54, sample.rows[1]) &&
		                      pbd::practice::native::start_models_match(entry.loaded.wanted.rows);
		const auto progress = entry.loaded.step(sample);
		if (progress == start::progress::refused)
			opening_refuse(entry, now, "practice_start_readback_changed");
		entry.ready = progress == start::progress::ready;
	}
}

void pbd::rollback_session::practice_steps::publish_opening_result(pbd::practice::start::state& entry, u32 mode)
{
	if (!entry.published && (entry.ready || !entry.refusal.empty()))
	{
		const auto text = fmt::format("{\"schema\":\"pbd-practice-start-result-1\",\"id\":%s,\"state\":\"%s\",\"characterRow\":%u,\"costume\":%u,\"playerRow\":%u,\"dummyRow\":%u,\"stage\":%u,\"mode\":%u,\"advancingFrames\":%u,\"reason\":%s}\n",
			pbd::json::quoted(entry.wanted ? entry.wanted->id : std::string{}), entry.ready ? "ready" : "unavailable",
			entry.wanted ? entry.wanted->character_row : 0, entry.wanted ? entry.wanted->costume : 0,
			entry.loaded.wanted.rows[0], entry.loaded.wanted.rows[1], entry.loaded.wanted.stage, mode,
			entry.loaded.advancing, pbd::json::quoted(entry.refusal));
		entry.published = practice_store::commit_text(entry.root / "practice-start-result.json", text);
	}
}

void pbd::rollback_session::practice_steps::opening_refuse(pbd::practice::start::state& entry, session_clock::time_point now, const char* reason)
{
	if (entry.refusal.empty())
	{
		entry.refusal = reason;
		entry.failed_at = now;
	}
}
