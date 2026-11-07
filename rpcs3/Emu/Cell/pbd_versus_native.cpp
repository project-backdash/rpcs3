#include "stdafx.h"
#include "pbd_versus_native.h"
#include "pbd_versus.h"
#include "../PBD/pbd_player_seats.h"
#include "../PBD/pbd_t5dr_memory.h"
#include "../PBD/pbd_versus_input.h"
#include "../PBD/pbd_custom_editor.h"
#include "../PBD/pbd_costume_panel.h"
#include "../PBD/pbd_custom_files.h"
#include "pbd_practice_native.h"
#include "PPUThread.h"
#include "Emu/pbd_session_mode.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

#include <chrono>
#include <mutex>
#include <random>

LOG_CHANNEL(pbd_versus_log, "PBD Versus");

namespace pbd::versus
{
	namespace
	{
		// Throttle director steps across repeated port reads; intervening reads reuse the last output.
		constexpr auto frame_gap = std::chrono::milliseconds(10);
		// VS win halfwords: each player's streak, then the extra counter advanced by the right player's wins.
		constexpr std::array<u32, 3> streak_words{0x1008ebf4, 0x1008ebf8, 0x1008ebfc};
		// Stage select tests bit 0x10 at *(*(TOC - 0x173c) + 0x230), with TOC 0x470540, for original Tekken 5 stages.
		constexpr u32 unlock_owner = 0x46ee04, unlock_byte_field = 0x230;
		constexpr u8 old_stages_unlock = 0x10;

		struct runtime
		{
			std::mutex mutex;
			director steps;
			output last;
			std::optional<pbd::rematch::selection> pending;
			std::chrono::steady_clock::time_point stepped{};
			std::mt19937 random{std::random_device{}()};
			u64 generation = 0;
			bool supported = false;
			// The VS mode was on at the last step, so its next opening frees the players (pbd_versus_input.h opened).
			bool versus = false;
			// A player held START + SELECT at the last step.
			bool chord = false;
			// The screen last logged: game mode, scene, subscene and each player's select state.
			std::array<u32, 5> logged{};
			std::array<pbd::costume_select::row, sides> costumes;
			std::array<std::optional<pbd::costume_select::card>, sides> looks;
			pbd::custom_files::practice costume_files;
			bool at_select = false, looks_pending = false;
			bool menu_logged = false;
			std::array<u16, sides> menu_words{};
		};

		runtime& state()
		{
			static runtime value;
			return value;
		}

		std::optional<u32> word(u32 address)
		{
			be_t<u32> value{};
			if (!vm::try_access(address, &value, sizeof(value), false))
				return {};
			return u32{value};
		}

		// The players' controllers: id, name and layout as the backend plays them now (pbd_controller_backend.cpp).
		void controllers(frame& f)
		{
			const auto seated = pbd::controllers::seats::read();
			const auto settings = pbd::controllers::settings::read();
			for (unsigned i = 0; i < sides; ++i)
			{
				f.devices[i] = seated.ids[i];
				f.names[i] = seated.names[i];
				f.layouts[i] = default_layout();
				for (const auto& d : settings.devices)
					if (d.id == seated.ids[i] && d.mapped)
						f.layouts[i] = d.mapping;
				f.words[i] = input::words[i].load(std::memory_order_acquire);
			}
		}

		void step(runtime& s)
		{
			frame f;
			const auto mode = word(pbd::t5dr::game_mode_address), scene = word(pbd::t5dr::scene_address),
					   subscene = word(pbd::t5dr::subscene_address);
			if (!mode || !scene || !subscene)
				return;
			f.versus = *mode == 4;
			input::active.store(f.versus, std::memory_order_release);
			f.phase = word(0x10093230).value_or(0);
			if (f.versus && !s.versus)
				input::opened.fetch_add(1, std::memory_order_acq_rel);
			s.versus = f.versus;
			f.scene = *scene;
			f.subscene = *subscene;
			for (unsigned i = 0; i < sides; ++i)
			{
				be_t<u16> wins{};
				if (vm::try_access(streak_words[i], &wins, sizeof(wins), false))
					f.wins[i] = wins;
			}
			// Unlock original Tekken 5 stages before stage select builds its lists.
			if (f.versus && f.scene == select_scene)
				if (const auto owner = word(unlock_owner); owner && *owner)
					if (const auto bits = word(*owner + unlock_byte_field); bits && *bits)
					{
						u8 unlocks = 0;
						if (vm::try_access(*bits, &unlocks, sizeof(unlocks), false) && !(unlocks & old_stages_unlock))
						{
							unlocks |= old_stages_unlock;
							vm::try_access(*bits, &unlocks, sizeof(unlocks), true);
						}
					}
			// Clear streaks after sampling the winner, before they can appear in the next game.
			if (f.versus && f.scene == select_scene)
				for (const u32 address : streak_words)
				{
					be_t<u16> wins{};
					if (vm::try_access(address, &wins, sizeof(wins), false) && wins)
					{
						wins = 0;
						vm::try_access(address, &wins, sizeof(wins), true);
					}
				}
			for (u32 i = 0; i < sides; ++i)
				if (const auto seen = pbd::hidden_selection::read_native_observation(i))
					f.players[i] = *seen;
			controllers(f);
			f.random = static_cast<u32>(s.random());
			s.last = s.steps.step(f);
			if (pbd::custom_wire::enabled())
			{
				const auto shown = s.steps.snapshot(f);
				if (shown.select && !s.at_select)
					s.costume_files = pbd::custom_files::read_practice(pbd::env::path("PBD_MENU_CHANNEL") / "practice-looks.json");
				s.at_select = shown.select;
				pbd::costume_panel::snapshot view{s.generation};
				if (const auto tables = pbd::hidden_selection::read_native_tables(); tables && shown.select)
					for (u32 side = 0; side < sides; ++side)
					{
						if (shown.setup[side].open || f.devices[side].empty())
							continue;
						const auto customs = pbd::custom_wire::choices(s.costume_files.table, f.players[side].base_character);
						const auto choice = s.costumes[side].step(*tables, f.players[side], f.words[side], customs);
						if (choice.picked)
							s.looks[side] = choice.picked;
						if (choice.word && !choice.controller_setup)
						{
							s.last.words[side] = choice.word;
							s.last.hidden[side] = s.last.added[side] = 0;
						}
						if (s.costumes[side].open() || (!view.visible && !s.costumes[side].choices().empty()))
							view = pbd::costume_panel::compose(s.generation, side, s.costumes[side]);
					}
				for (auto& card : view.cards)
					for (const auto& label : s.costume_files.labels)
						if (label.row == card.look.row && label.slot == (card.slot ? *card.slot + 1 : 0))
						{
							card.display_name = label.name;
							card.picture = label.picture;
						}
				if (!shown.select)
					s.costumes = {};
				pbd::costume_panel::publish(std::move(view));
			}
			if (s.last.new_players)
				input::opened.fetch_add(1, std::memory_order_acq_rel);
			if (s.last.direct_start)
				s.pending = s.last.direct_start;
			if (!f.versus)
				s.pending.reset();
			// One line when a player's START + SELECT goes down and one when its hold leaves, so a log shows how far a leave got.
			bool chord = false;
			for (unsigned i = 0; i < sides; ++i)
				if ((f.words[i] & leave_chord) == leave_chord)
				{
					chord = true;
					if (!s.chord)
						pbd_versus_log.notice("START + SELECT down on player %u (scene %u phase %u)", i + 1, f.scene, f.phase);
				}
			s.chord = chord;
			if (s.last.leave)
			{
				const bool asked = pbd::settings_menu::request_leave(pbd::settings_menu::entry::launcher);
				pbd_versus_log.notice("START + SELECT held: %s", asked ? "back to the launcher" : "no launcher to return to (no request file)");
			}
			const auto shown = s.steps.snapshot(f);
			if (shown.menu && !s.menu_logged)
				pbd_versus_log.notice("After-game result: wins %u/%u at 0x1008ebf4/0x1008ebf8; player %u chooses",
					f.wins[0], f.wins[1], shown.menu_player + 1);
			if (shown.menu && (!s.menu_logged || f.words != s.menu_words))
				pbd_versus_log.notice("After-game input: words %04x/%04x controllers \"%s\"/\"%s\" focus %u",
					f.words[0], f.words[1], f.devices[0], f.devices[1], shown.menu_focus);
			s.menu_logged = shown.menu;
			s.menu_words = f.words;
			// One line each time the screen changes, so a log shows the path the director followed.
			const std::array<u32, 5> screen{*mode, f.scene, f.subscene, f.players[0].verified ? f.players[0].state : ~0u,
				f.players[1].verified ? f.players[1].state : ~0u};
			if (screen != s.logged)
			{
				s.logged = screen;
				pbd_versus_log.notice("mode %u scene %u subscene %u states %d %d: select %d menu %d banner \"%s\"", screen[0], screen[1],
					screen[2], static_cast<s32>(screen[3]), static_cast<s32>(screen[4]), shown.select, shown.menu, shown.banner);
			}
			publish(shown);
			for (unsigned i = 0; i < sides; ++i)
			{
				input::panel[i].store(shown.setup[i].open, std::memory_order_release);
				if (s.last.changed[i])
					pbd::controllers::settings::submit({f.devices[i], *s.last.changed[i]});
			}
			// At the select the buttons pick and confirm as themselves; in a game they are the actions the layouts give them.
			input::select.store(f.versus && f.scene == select_scene, std::memory_order_release);
		}
	} // namespace

	port_word at_read(u32 port) noexcept
	{
		if (pbd::session_mode::customize() || pbd::custom_editor::blocks_saves(static_cast<u64>(Emu.GetEmulationIdentifier())))
		{
			if (pbd::custom_editor::trial_pad.load(std::memory_order_acquire))
				return {port == 0 ? pbd::custom_editor::trial_word.load(std::memory_order_acquire) : u16{0}};
			return port == 0 ? port_word{} : port_word{u16{0}};
		}
		try
		{
			if (port >= sides)
				return {};
			auto& s = state();
			std::lock_guard lock(s.mutex);
			const u64 generation = static_cast<u64>(Emu.GetEmulationIdentifier());
			if (generation != s.generation)
			{
				// Each boot reads the game afresh: its executable, and the select's tables when a rematch needs them.
				s.generation = generation;
				s.supported = Emu.GetTitleID() == pbd::t5dr::title_id && Emu.GetExecutableHash() == pbd::t5dr::executable_hash;
				pbd_versus_log.notice("offline Versus %s for %s %s", s.supported ? "on" : "off", Emu.GetTitleID(), Emu.GetExecutableHash());
				s.steps = {};
				s.last = {};
				s.pending.reset();
				s.versus = false;
				s.costumes = {};
				s.looks = {};
				s.at_select = s.looks_pending = false;
			}
			if (!s.supported)
				return {};
			const auto now = std::chrono::steady_clock::now();
			if (now - s.stepped >= frame_gap)
			{
				s.stepped = now;
				step(s);
			}
			return {s.last.words[port], s.last.hidden[port], s.last.added[port]};
		}
		catch (...)
		{
			return {};
		}
	}

	bool frame_boundary(ppu_thread& owner, u32 address) noexcept
	{
		if (!pbd::session_mode::offline_versus() || address != pbd::t5dr::boundary_pc ||
			owner.id != pbd::t5dr::main_ppu_id || pbd::practice::native::in_native_call())
			return true;
		try
		{
			auto& s = state();
			std::optional<pbd::rematch::selection> request;
			std::array<std::optional<pbd::costume_select::card>, sides> looks;
			bool dress = false;
			const auto generation = static_cast<u64>(Emu.GetEmulationIdentifier());
			{
				std::lock_guard lock(s.mutex);
				if (!s.supported || s.generation != generation)
					return true;
				request = std::exchange(s.pending, {});
				looks = s.looks;
				if (s.looks_pending && word(pbd::t5dr::scene_address) == 2u)
				{
					dress = true;
					s.looks_pending = false;
				}
			}
			if (dress)
			{
				// The outgoing selector fills the cards before resource scene 2. Apply the saved look after that fill.
				bool dressed = true;
				for (u32 side = 0; side < sides; ++side)
				{
					const u32 row = word(0x10093f50 + side * 4).value_or(~0u);
					const auto* base = pbd::custom_look::catalogue_row(row);
					if (!base)
					{
						dressed = false;
						break;
					}
					std::array<u8, pbd::custom_look::blocks_size> saved{}, held{};
					const auto look = looks[side] && looks[side]->look.row == row ? looks[side]->look : pbd::custom_look::look{row};
					dressed = vm::try_access(pbd::custom_look::save_blocks_address(base->fighter), saved.data(), saved.size(), false) && dressed;
					auto blocks = pbd::custom_look::record_blocks(saved, look);
					dressed = vm::try_access(pbd::custom_look::record_address(side), blocks.data(), blocks.size(), true) && dressed;
					dressed = vm::try_access(pbd::custom_look::record_address(side), held.data(), held.size(), false) && held == blocks && dressed;
				}
				pbd_versus_log.notice("Local costumes at resource scene: records written %u", dressed);
			}
			if (!request)
				return true;
			const pbd::replay_native::choices selected{request->rows[0], request->rows[1], request->stage};
			const char* refusal = "versus_direct_start_refused";
			bool applied = false;
			// Scheduler work raised after the owner is serviced waits until the setters return (see owner_guard).
			if (pbd::replay_native::service_owner(owner, generation, &refusal))
			{
				const pbd::replay_native::owner_guard admission(owner);
				applied = pbd::replay_native::can_prepare_resources(owner, generation, selected, &refusal) &&
				          pbd::replay_native::apply_handicap_options(owner, generation, 100, 100) &&
				          pbd::replay_native::prepare_resources(owner, generation, selected, &refusal);
			}
			if (!applied)
			{
				pbd_versus_log.error("Offline Versus direct start refused: %s", refusal);
				return false;
			}
			{
				std::lock_guard lock(s.mutex);
				s.looks_pending = pbd::custom_wire::enabled();
			}
			pbd_versus_log.notice("Offline Versus direct load: rows %u/%u stage %u", selected.left, selected.right, selected.stage);
			return true;
		}
		catch (...)
		{
			pbd_versus_log.error("Offline Versus direct start failed.");
			return false;
		}
	}
} // namespace pbd::versus
