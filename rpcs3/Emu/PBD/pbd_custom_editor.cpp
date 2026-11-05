#include "stdafx.h"
#include "pbd_custom_editor.h"
#include "pbd_custom_wire.h"
#include "pbd_custom_photo.h"
#include "pbd_custom_files.h"
#include "pbd_costume_panel.h"
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
			u64 photo_ticket = 0;
			u32 photo_wait = 0;
			bool photo_written = false, protocol = false, cancelled = false;
			std::optional<custom_files::customize> request;
			std::optional<custom_look::look> saved_look;
			save_panel panel;
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
		void finish(ppu_thread& ppu, trial& state, const char* reason, bool capture)
		{
			state.finished = true;
			custom_photo::pending.cancel(state.photo_ticket);
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
				unchanged ? "true" : "false", restored ? "true" : "false", state.photo_written ? "true" : "false");
			if (state.protocol)
			{
				const bool safe = capture && look && unchanged && restored;
				// A portrait needs a reset preview pose. Until that reset is available, saved slots use their numbered outlines.
				const auto text = custom_files::result(state.request.value_or(custom_files::customize{}),
					safe ? "saved" : state.cancelled ? "cancelled" :
													   "unavailable",
					state.panel.slot(), look, false);
				const bool written = custom_files::commit_result(state.output, text);
				costume_panel::publish({state.generation});
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

	bool trial_boundary(ppu_thread& ppu, u32 address) noexcept
	{
		if (address != t5dr::boundary_pc || ppu.id != t5dr::main_ppu_id || practice::native::in_native_call())
			return true;
		try
		{
			static thread_local trial state;
			const auto generation = static_cast<u64>(Emu.GetEmulationIdentifier());
			if (state.generation != generation)
			{
				custom_photo::pending.cancel(state.photo_ticket);
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
			if (state.protocol && state.panel.open() && !state.photo_ticket)
			{
				if (!admissible(value) || !put_word(0x10538e38, 4))
				{
					finish(ppu, state, "customize_save_context_changed", false);
					return true;
				}
				const auto decision = state.panel.step(pbd_local_pad_word());
				if (decision == save_action::save || decision == save_action::discard)
				{
					state.cancelled = decision == save_action::discard;
					finish(ppu, state, state.cancelled ? "customize_cancelled" : "customize_saved", !state.cancelled);
				}
				else if (decision == save_action::keep_editing)
				{
					state.saved_look.reset();
					state.photo_written = false;
					state.steps.resume();
					trial_pad.store(false, std::memory_order_release);
					costume_panel::publish({generation});
				}
				else
					costume_panel::publish_editor(generation, state.panel);
				return true;
			}
			if (state.photo_ticket)
			{
				if (!admissible(value) || ++state.photo_wait > 600)
					finish(ppu, state, "editor_photo_timeout_or_context_changed", false);
				else if (auto photo = custom_photo::pending.take(state.photo_ticket))
				{
					const auto png = custom_photo::encode_png(state.protocol ? custom_photo::portrait(*photo) : *photo);
					const auto folder = state.protocol ? state.request->picture_dir : state.output.parent_path();
					std::error_code ec;
					std::filesystem::create_directories(folder, ec);
					const auto path = (folder / (state.protocol ? "photo.png" : "editor-photo.png")).u8string();
					fs::file file(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
					state.photo_written = !png.empty() && png.size() <= custom_files::picture_limit && file && file.write(png.data(), png.size()) == png.size();
					file.close();
					if (state.protocol)
					{
						state.photo_ticket = 0;
						state.panel.photo_ready(true);
						costume_panel::publish_editor(generation, state.panel);
					}
					else
						finish(ppu, state, state.photo_written ? "shop_exit_captured" : "editor_photo_write_failed", true);
				}
				else if (!put_word(0x10538e38, 4))
					finish(ppu, state, "editor_photo_hold_failed", false);
				return true;
			}
			const auto next = state.steps.step(value);
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
					if (!state.saved_look || custom_look::catalogue_row(state.row)->fighter != custom_look::catalogue_row(state.request->start.row)->fighter ||
						!state.panel.begin(state.request->slot, state.request->filled) || !put_byte(0x10093f78, 6) || !put_byte(0x10093f79, 6))
					{
						finish(ppu, state, "customize_look_unavailable", false);
						return true;
					}
					costume_panel::publish_editor(generation, state.panel);
				}
				// Hold the stock item view and its controller while two new guest flips retire the menu transition.
				trial_pad.store(true, std::memory_order_release);
				trial_word.store(0, std::memory_order_release);
				state.photo_wait = 0;
				state.photo_ticket = custom_photo::pending.request(generation, custom_photo::pending.latest_flip() + 2);
				if (!state.photo_ticket || !put_word(0x10538e38, 4))
					finish(ppu, state, "editor_photo_request_failed", false);
			}
			if (!state.finished && !state.photo_ticket && state.exercise_frame && trial_pad.load(std::memory_order_acquire))
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
