#include "stdafx.h"
#include "pbd_custom_editor.h"
#include "pbd_custom_wire.h"
#include "pbd_custom_files.h"
#include "pbd_custom_photo.h"
#include "pbd_costume_panel.h"
#include "pbd_practice_presentation.h"
#include "pbd_settings_runtime.h"
#include "pbd_output_gain.h"
#include "../Cell/pbd_rollback_session.h"
#include "pbd_native_call.h"
#include "../Cell/pbd_hidden_selection.h"
#include "Emu/pbd_session_mode.h"

#include <charconv>
#include <filesystem>

LOG_CHANNEL(pbd_custom_editor_log, "PBD Customize");

namespace pbd::custom_editor
{
	namespace
	{
		struct saved_span
		{
			u32 address;
			std::vector<u8> bytes;
			bool read()
			{
				return vm::try_access(address, bytes.data(), bytes.size(), false);
			}
			bool matches() const
			{
				std::vector<u8> current(bytes.size());
				return vm::try_access(address, current.data(), current.size(), false) && current == bytes;
			}
			bool restore()
			{
				return vm::try_access(address, bytes.data(), bytes.size(), true) && matches();
			}
		};
		struct trial
		{
			flow steps;
			u64 generation = 0;
			u32 row = 0, logged_shop = ~0u;
			std::optional<u32> exercise_frame;
			bool protocol = false;
			std::optional<custom_files::customize> request;
			std::optional<custom_look::look> saved_look;
			bool inspected = false, requested = false, captured = false, touched = false, finished = false;
			std::filesystem::path output;
			std::vector<saved_span> held, protected_save;
		};

		bool put_word(u32 address, u32 value)
		{
			be_t<u32> bytes{value};
			return vm::try_access(address, &bytes, sizeof(bytes), true) && practice::native::word(address) == value;
		}
		bool put_byte(u32 address, u8 value)
		{
			return vm::try_access(address, &value, sizeof(value), true) && practice::native::byte(address) == value;
		}
		bool remember(trial& state)
		{
			state.held = {{custom_look::record_address(0), std::vector<u8>(0x1f4)},
				{0x10093f58, std::vector<u8>(0x78)}, {0x10093f50, std::vector<u8>(4)},
				{0x10538e38, std::vector<u8>(0x98)}, {0x10094050, std::vector<u8>(4)}};
			state.protected_save = {{custom_look::save_image, std::vector<u8>(137408)},
				{0x1054ae70, std::vector<u8>(0x431b0 * 2)}};
			for (auto& span : state.held)
				if (!span.read())
					return false;
			for (auto& span : state.protected_save)
				if (!span.read())
					return false;
			state.captured = true;
			return true;
		}
		// Pictures observe play without owning input; encoding and publication run on a separate host thread.
		void capture_worn_picture(ppu_thread& ppu, u64 generation)
		{
			struct capture_state
			{
				u64 generation = 0, ticket = 0, after_flip = 0;
				u32 side = 0, waits = 0;
				bool loaded = false;
				std::filesystem::path root;
				std::vector<custom_wire::drawn> looks, saving;
				custom_photo::entry_pose pose;
			};
			static thread_local capture_state state;
			if (state.generation != generation)
			{
				custom_photo::pending.cancel(state.ticket);
				state = {};
				state.generation = generation;
				const auto path = env::path("PBD_CUSTOM_PICTURES");
				if (path.is_absolute() && path.filename() == "picture-looks.json")
				{
					state.root = path.parent_path() / "photos";
				}
			}
			if (state.root.empty() || (state.loaded && state.looks.empty() && state.saving.empty() && !state.ticket))
			{
				state.pose.finish();
				return;
			}
			if (!practice::native::boundary_readable(ppu) || !hidden_selection::pinned_binary())
				return;
			const auto mode = practice::native::word(t5dr::game_mode_address);
			const auto scene = practice::native::word(t5dr::scene_address);
			const auto phase = practice::native::word(0x10093230);
			const bool visible = output_gain::output().visible();
			const bool neutral = state.pose.observe(mode, scene, phase, visible);
			// Observe loading while hidden, but never request a hidden frame or discard the remembered opening.
			if (!visible)
			{
				custom_photo::pending.cancel(state.ticket);
				state.ticket = 0;
				state.looks.insert(state.looks.end(), state.saving.begin(), state.saving.end());
				state.saving.clear();
				return;
			}
			if (!state.loaded)
			{
				state.loaded = true;
				state.looks = custom_files::read_picture_looks(state.root.parent_path() / "picture-looks.json");
			}
			const auto file_for = [&](const custom_wire::drawn& value)
			{
				if (!value.slot)
					return state.root / ("costume-" + std::string(value.value[0] < 10 ? "0" : "") + std::to_string(value.value[0]) + ".jpg");
				const auto* row = custom_look::catalogue_row(value.value[0]);
				return state.root / (std::string(custom_files::character_ids[row->fighter]) + "-" +
										(value.slot < 10 ? "0" : "") + std::to_string(value.slot) + ".jpg");
			};
			const auto worn_for = [](const auto& blocks, u32 row) -> std::optional<custom_look::look>
			{
				const auto* entry = custom_look::catalogue_row(row);
				if (entry && !entry->dressable)
					return custom_look::normalise(custom_look::look{row});
				return custom_look::read_record(blocks, row);
			};
			if (!neutral)
			{
				custom_photo::pending.cancel(state.ticket);
				state.ticket = 0;
				state.looks.insert(state.looks.end(), state.saving.begin(), state.saving.end());
				state.saving.clear();
				return;
			}
			if (state.ticket)
			{
				if (auto photo = custom_photo::pending.take(state.ticket))
				{
					std::vector<std::filesystem::path> targets;
					for (const auto& look : state.saving)
						targets.push_back(file_for(look));
					const auto flip = photo->flip;
					if (!custom_photo::save_async(std::move(*photo), state.side, std::move(targets)))
						state.looks.insert(state.looks.end(), state.saving.begin(), state.saving.end());
					else
						pbd_custom_editor_log.notice("custom-first-use-picture-1: side %u flip %u queued", state.side, flip);
					state.ticket = 0;
					state.saving.clear();
				}
				else if (++state.waits > 600)
				{
					custom_photo::pending.cancel(state.ticket);
					state.ticket = 0;
					state.saving.clear();
				}
				return;
			}
			if (!state.saving.empty())
			{
				if (custom_photo::pending.latest_flip() >= state.after_flip)
				{
					state.waits = 0;
					state.ticket = custom_photo::pending.request(generation, custom_photo::pending.latest_flip() + 2);
					if (!state.ticket)
						state.saving.clear();
				}
				return;
			}
			for (u32 side = 0; side < 2; ++side)
			{
				be_t<u16> row{};
				std::array<u8, custom_look::blocks_size> blocks{};
				if (!vm::try_access(replay_memory::actors + side * replay_memory::actor_stride + replay_memory::actor_row, &row, sizeof(row), false) ||
					!vm::try_access(custom_look::record_address(side), blocks.data(), blocks.size(), false))
					continue;
				const auto worn = worn_for(blocks, row);
				if (!worn)
					continue;
				const auto identity = custom_wire::pack(*worn);
				for (auto it = state.looks.begin(); it != state.looks.end();)
				{
					if (it->value != identity)
					{
						++it;
						continue;
					}
					std::error_code error;
					if (std::filesystem::exists(file_for(*it), error))
						it = state.looks.erase(it);
					else
					{
						state.saving.push_back(*it);
						it = state.looks.erase(it);
					}
				}
				if (!state.saving.empty())
				{
					state.side = side;
					// Wait for the fade only while the player stays neutral. The countdown already keeps online poses neutral.
					state.after_flip = custom_photo::pending.latest_flip() + (mode == 10 ? 120 : 0);
					return;
				}
			}
			state.pose.finish();
		}
		void finish(ppu_thread& ppu, trial& state, const char* reason, bool capture)
		{
			state.finished = true;
			trial_word.store(0, std::memory_order_release);
			std::optional<custom_look::look> look;
			std::array<u8, custom_look::blocks_size> blocks{};
			if (capture && vm::try_access(custom_look::record_address(0), blocks.data(), blocks.size(), false))
				look = state.saved_look ? state.saved_look : custom_look::read_record(blocks, state.row);
			bool unchanged = state.captured;
			for (const auto& span : state.protected_save)
				unchanged = span.matches() && unchanged;
			bool restored = state.captured;
			if (state.touched)
				for (auto& span : state.held)
					restored = span.restore() && restored;
			const auto text = fmt::format("{\"schema\":\"pbd-customize-trial-1\",\"reason\":%s,\"row\":%u,\"look\":%s,\"save_unchanged\":%s,\"card_and_select_restored\":%s,\"photo_written\":%s,\"overlays_included\":false}\n",
				json::quoted(reason), state.row, look ? json::quoted(custom_wire::hex(custom_wire::pack(*look))) : std::string("null"),
				unchanged ? "true" : "false", restored ? "true" : "false", "false");
			if (state.protocol)
			{
				const bool safe = capture && look && unchanged && restored;
				// EXIT saves to the requested slot before closing; pictures come from the first time the look is worn.
				const auto text = custom_files::result(state.request.value_or(custom_files::customize{}),
					safe ? "saved" : "unavailable",
					state.request ? state.request->slot : 0, look, false);
				const bool written = custom_files::commit_result(state.output, text);
				pbd_custom_editor_log.notice("Customize result: %s; save unchanged %u; restored %u; result written %u", reason, unchanged, restored, written);
				ppu.state += cpu_flag::exit;
				Emu.CallFromMainThread([]
					{
						Emu.Kill(true, false);
					});
				return;
			}
			const auto path = state.output.u8string();
			fs::file file(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
			const bool written = file && file.write(text.data(), text.size()) == text.size();
			pbd_custom_editor_log.notice("Customize entry observation: %s; look %s; save unchanged %u; restored %u; result written %u",
				reason, look ? custom_wire::hex(custom_wire::pack(*look)) : "unavailable", unchanged, restored, written);
			// Restored selector bytes must never execute against the shop's newly allocated preview tasks.
			ppu.state += cpu_flag::exit;
			Emu.CallFromMainThread([]
				{
					Emu.Kill(true, false);
				});
		}
	} // namespace

	bool suppress_profile_paint(ppu_thread& ppu, u32 address) noexcept
	{
		if (!session_mode::customize() || !blocks_saves(static_cast<u64>(Emu.GetEmulationIdentifier())) || Emu.GetUsrId() != 3 ||
			ppu.id != t5dr::main_ppu_id || practice::native::word(t5dr::game_mode_address) != 4 ||
			practice::native::word(t5dr::scene_address) != 3 || practice::native::word(t5dr::subscene_address) != 44 ||
			!hidden_selection::pinned_binary())
			return false;
		if (select_rank_sprite(address, static_cast<u32>(ppu.lr)) &&
			practice::native::word(address) == 0x38000070 && practice::native::word(address + 4) == 0xf821ff31)
		{
			// The select's card-present branch sets sprite+0x9c to one at 0x113998; its absent-card branch sets zero.
			// Keep the resource update and only hide this select sprite, leaving the shop's preview tasks running.
			put_word(static_cast<u32>(ppu.gpr[3]) + 0x9c, 0);
		}
		return (address == profile_draws[0] && practice::native::word(address) == 0x380000b0 &&
				   practice::native::word(address + 4) == 0xf821fed1) ||
		       (address == profile_draws[1] && practice::native::word(address) == 0x38000190 &&
				   practice::native::word(address + 4) == 0xf821fdd1);
	}

	bool trial_boundary(ppu_thread& ppu, u32 address) noexcept
	{
		if (address != t5dr::boundary_pc || ppu.id != t5dr::main_ppu_id || practice::native::in_native_call())
			return true;
		try
		{
			practice_costume::boundary(ppu, static_cast<u64>(Emu.GetEmulationIdentifier()));
			static thread_local u64 picture_failed = 0;
			const auto picture_generation = static_cast<u64>(Emu.GetEmulationIdentifier());
			if (picture_failed != picture_generation)
				try
				{
					capture_worn_picture(ppu, picture_generation);
				}
				catch (...)
				{
					picture_failed = picture_generation;
					pbd_custom_editor_log.warning("First-use picture unavailable; play continues.");
				}
			static thread_local trial state;
			const auto generation = static_cast<u64>(Emu.GetEmulationIdentifier());
			if (state.generation != generation)
			{
				state = {};
				state.generation = generation;
				save_blocked.store(false, std::memory_order_release);
				trial_pad.store(false, std::memory_order_release);
				trial_word.store(0, std::memory_order_release);
			}
			if (!state.inspected)
			{
				state.inspected = true;
				if (const auto path = env::path("PBD_CUSTOMIZE_REQUEST"); custom_wire::enabled() && !path.empty())
				{
					state.protocol = true;
					if (path.is_absolute() && path.filename() == "customize-request.json")
						state.output = path.parent_path() / "customize-result.json";
					const auto text = path.is_absolute() && path.filename() == "customize-request.json" ? custom_files::read(path, 8192) : std::nullopt;
					state.request = text ? custom_files::parse_customize(*text) : std::nullopt;
					if (!state.request)
					{
						finish(ppu, state, "customize_request_unavailable", false);
						return true;
					}
					state.row = state.request->start.row;
					state.requested = true;
					save_generation.store(generation, std::memory_order_release);
					save_blocked.store(true, std::memory_order_release);
				}
				const char* row = env::find("PBD_CUSTOMIZE_TRIAL_ROW");
				const char* output = env::find("PBD_CUSTOMIZE_TRIAL_OUTPUT");
				if (!state.protocol && (!custom_wire::enabled() || !row || !output || !*output))
					return true;
				if (!state.protocol)
				{
					state.output = std::filesystem::u8path(output);
					const std::string_view text(row);
					const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), state.row);
					state.requested = error == std::errc{} && end == text.data() + text.size() && state.output.is_absolute() &&
					                  custom_look::shop_blocks(custom_look::look{state.row}).has_value();
					const char* automatic = env::find("PBD_CUSTOMIZE_TRIAL_PAD");
					trial_pad.store(automatic && std::string_view(automatic) == "1", std::memory_order_release);
				}
			}
			if (!state.requested || state.finished)
				return true;
			const bool owner = practice::native::boundary_readable(ppu) && hidden_selection::pinned_binary();
			const u32 parent = practice::native::word(0x104cb574);
			observation value{owner, session_mode::offline_play(), parent && parent != ~0u && vm::check_addr(parent, vm::page_readable, 0x100),
				Emu.GetUsrId(), practice::native::word(t5dr::game_mode_address), practice::native::word(t5dr::scene_address),
				practice::native::word(t5dr::subscene_address), practice::native::byte(0x10093f78),
				practice::native::byte(0x10093f79), practice::native::word(0x10538e38)};
			// Keep the stock select timer from closing a look while its player is editing.
			if (!put_word(0x10094050, 3600))
			{
				finish(ppu, state, "customize_timer_unavailable", false);
				return true;
			}
			const auto next = state.steps.step(value, state.protocol ? pbd_local_pad_word() : 0);
			if (next == action::refuse)
			{
				finish(ppu, state, "editor_context_or_transition_refused", false);
				return true;
			}
			if (next == action::enter)
			{
				if (!practice::native::body_matches(0x1500c8, 0x194, "91294e8d3b7a212957d2b2e65cec47142880b45fc8037c4e71ec8bef83f85624") || !remember(state))
				{
					finish(ppu, state, "editor_snapshot_or_initializer_refused", false);
					return true;
				}
				save_generation.store(generation, std::memory_order_release);
				save_blocked.store(true, std::memory_order_release);
				state.touched = true;
				auto blocks = *custom_look::shop_blocks(state.request ? state.request->start : custom_look::look{state.row});
				// State 5 calls the stock side initializer; state 6 then runs its controller and preview loader.
				const bool applied = vm::try_access(custom_look::record_address(0), blocks.data(), blocks.size(), true) &&
				                     put_word(0x10093f68, state.row) && put_word(0x10093f50, state.row) && put_word(0x10094050, 3600) &&
				                     put_byte(0x10093f78, 5) && put_byte(0x10093f79, 5);
				if (!applied)
					finish(ppu, state, "editor_entry_write_failed", false);
			}
			else if (next == action::open_items)
			{
				// This is the exact next state of Item Shop & Equip's stock confirm, after the preview is ready.
				if (!put_word(0x10538e38, 4))
					finish(ppu, state, "editor_items_write_failed", false);
				state.exercise_frame = 0;
			}
			else if (next == action::capture)
			{
				if (state.protocol)
				{
					std::array<u8, custom_look::blocks_size> blocks{};
					state.row = practice::native::word(0x10093f50);
					if (vm::try_access(custom_look::record_address(0), blocks.data(), blocks.size(), false))
						state.saved_look = custom_look::read_record(blocks, state.row);
					const auto* row = custom_look::catalogue_row(state.row);
					const auto* base = custom_look::catalogue_row(state.request->start.row);
					if (!state.saved_look || !row || !base || row->fighter != base->fighter)
					{
						finish(ppu, state, "customize_look_unavailable", false);
						return true;
					}
				}
				finish(ppu, state, "customize_saved", true);
				return true;
			}
			if (!state.finished && state.exercise_frame && trial_pad.load(std::memory_order_acquire))
				trial_word.store(exercise_word(++*state.exercise_frame), std::memory_order_release);
			if (value.shop != state.logged_shop)
			{
				state.logged_shop = value.shop;
				pbd_custom_editor_log.notice("Customize entry: selector %u/%u, shop %u, row %u", value.selector, value.next_selector, value.shop, state.row);
			}
			return true;
		}
		catch (...)
		{
			pbd_custom_editor_log.error("Customize entry observation failed.");
			return false;
		}
	}
} // namespace pbd::custom_editor

namespace pbd::practice_costume
{
	namespace
	{
		struct runtime
		{
			std::mutex mutex;
			u64 generation = 0;
			bool selecting = false, written = false, files_read = false;
			std::array<costume_select::row, 2> menus;
			std::array<std::optional<costume_select::card>, 2> picks;
			costume_select::confirm_release confirmation;
			custom_files::practice files;
		};
		runtime current;
		void reset(u64 generation)
		{
			current.generation = generation;
			current.selecting = current.written = current.files_read = false;
			current.menus = {};
			current.picks = {};
			current.confirmation = {};
			current.files = {};
		}
	} // namespace

	std::optional<u16> select_word(u32 port, u16 raw) noexcept
	{
		if (port != 0 || !practice::requested() || !custom_wire::enabled() ||
			practice::native::word(t5dr::game_mode_address) != 10 || practice::native::word(t5dr::scene_address) != 3)
			return {};
		try
		{
			std::lock_guard lock(current.mutex);
			const auto generation = static_cast<u64>(Emu.GetEmulationIdentifier());
			if (current.generation != generation)
				reset(generation);
			const bool held = current.confirmation.holds(raw);
			if (held)
				raw = 0;
			const auto tables = hidden_selection::read_native_tables();
			if (!tables)
				return {};
			for (u32 side = 0; side < 2; ++side)
			{
				const auto seen = hidden_selection::read_native_observation(side);
				if (!seen || seen->mode != 10 || seen->scene != 3 || seen->state != 3 || seen->pad_role != port)
					continue;
				current.selecting = true;
				if ((raw & costume_select::square) && !current.menus[side].open())
				{
					current.files_read = true;
					current.files = custom_files::read_practice(env::path("PBD_MENU_CHANNEL") / "practice-looks.json");
					const auto pictured = std::count_if(current.files.labels.begin(), current.files.labels.end(), [](const auto& label)
						{
							return !label.picture.empty();
						});
					pbd_custom_editor_log.notice("Practice costume pictures: %u loaded from %u labels.", pictured, current.files.labels.size());
				}
				auto& menu = current.menus[side];
				const auto customs = custom_wire::choices(current.files.table, seen->base_character);
				const auto chosen = menu.step(*tables, *seen, raw, customs);
				if (chosen.picked)
				{
					current.picks[side] = chosen.picked;
					current.confirmation.consume();
				}
				if (chosen.controller_setup)
					settings_menu::request_controls_open(controllers::read_display().device);
				auto view = costume_panel::compose(generation, side, menu);
				for (auto& card : view.cards)
					for (const auto& label : current.files.labels)
						if (label.row == card.look.row && label.slot == (card.slot ? *card.slot + 1 : 0))
						{
							card.display_name = label.name;
							card.picture = label.picture;
						}
				costume_panel::publish(std::move(view));
				return chosen.word.value_or(raw);
			}
			costume_panel::publish({generation});
			if (held)
				return u16{0};
		}
		catch (...)
		{
			pbd_custom_editor_log.warning("Practice costume selection unavailable; stock input continues.");
		}
		return {};
	}

	void boundary(ppu_thread& ppu, u64 generation) noexcept
	{
		if (!practice::requested() || !custom_wire::enabled())
			return;
		try
		{
			std::lock_guard lock(current.mutex);
			if (current.generation != generation)
				reset(generation);
			if (!practice::native::boundary_readable(ppu))
				return;
			if (practice::native::word(t5dr::game_mode_address) != 10)
			{
				reset(generation);
				costume_panel::publish({generation});
				return;
			}
			const u32 scene = practice::native::word(t5dr::scene_address);
			if (scene == 3 && !current.selecting)
			{
				reset(generation);
				current.selecting = true;
			}
			if (scene == 2 && current.selecting && !current.written)
			{
				current.written = true;
				for (u32 side = 0; side < 2; ++side)
				{
					const u32 row = practice::native::word(0x10093f50 + side * 4);
					const auto* base = custom_look::catalogue_row(row);
					if (!base || !current.picks[side])
						continue;
					const auto wanted = current.picks[side]->look.row == row ? current.picks[side]->look : custom_look::look{row};
					std::array<u8, custom_look::blocks_size> saved{}, held{};
					bool written = vm::try_access(custom_look::save_blocks_address(base->fighter), saved.data(), saved.size(), false);
					auto blocks = custom_look::record_blocks(saved, wanted);
					written = written && vm::try_access(custom_look::record_address(side), blocks.data(), blocks.size(), true) &&
					          vm::try_access(custom_look::record_address(side), held.data(), held.size(), false) && held == blocks;
					pbd_custom_editor_log.notice("Practice select costume: side %u row %u slot %u written %u", side, row,
						current.picks[side]->slot ? *current.picks[side]->slot + 1 : 0, written);
				}
			}
			if (scene != 3)
				costume_panel::publish({generation});
			// Stock Practice visits selector exit scenes 4 and 5 before its resource scene. Keep the pick through them.
			if (scene == 17)
				current.selecting = false;
		}
		catch (...)
		{
			pbd_custom_editor_log.warning("Practice costume card unavailable; play continues.");
		}
	}
} // namespace pbd::practice_costume
