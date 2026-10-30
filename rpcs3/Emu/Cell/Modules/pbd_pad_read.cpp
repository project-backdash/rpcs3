#include "stdafx.h"
#include "pbd_pad_read.h"
#include "cellPad.h"
#include "pbd_input_trace.h"
#include "pbd_input_script_runtime.h"
#include "pbd_input_live.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/PBD/pbd_settings_runtime.h"
#include "Emu/Cell/pbd_versus_native.h"
#include "Emu/Cell/pbd_rollback_session.h"
#include "Emu/Io/pad_types.h"
#include "Emu/Memory/vm.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/pbd_session_mode.h"
#include "Input/pad_thread.h"
#include "Utilities/File.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pbd::pad_read
{
	namespace
	{
		using port_pads = std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM>;

		// PBD canonical pad record. In a peer session (PBD_PAD_CANONICAL=1, set by the host tooling whenever a
		// peer role is set) the game must see identical pad records on both machines apart from the digital words, which the
		// update latch injects for both ports from the published inputs. A real controller's record carries device-specific
		// data (record length changing with the sensor/pressure settings, sensor words, pressure values, stick offsets) that the
		// game copies into its input region (combat region 2): without this the peers' state hashes differ even
		// with equal injected inputs. The record is rebuilt the way the diagnostic tape builds it,
		// sticks centred (the left stick is folded into the d-pad bits first so an analog stick still moves the fighter).
		bool pbd_pad_canonical()
		{
			static const bool enabled = []
			{
				const char* value = pbd::env::find("PBD_PAD_CANONICAL");
				return value && value[0] == '1' && value[1] == 0;
			}();
			return enabled;
		}

		// Practice owns START+SELECT. The raw host state remains available to the Practice controller, while the guest receives the
		// same policy as an online session: SELECT is always consumed, and START is consumed from the combination until it is released.
		bool pbd_practice_owns_input()
		{
			static const bool enabled = []
			{
				const char* value = pbd::env::find("PBD_PRACTICE_REWIND");
				return value && value[0] == '1' && value[1] == 0;
			}();
			return enabled;
		}

#ifdef PBD_DIAGNOSTIC_BUILD
		// PBD_PAD_SYNTH="<seed>" replaces real pads' digital words with directions and face buttons held for 40..280 ms of
		// host time. System buttons are excluded and virtual (LDD) pads stay neutral.
		struct synth_state
		{
			bool enabled = false;
			u64 random = 0;
			u64 next_change_us[2]{};
			u16 d1[2]{}, d2[2]{};
			u64 wander_until_us[2]{}, act_until_us[2]{};
		};

		synth_state& synth()
		{
			static synth_state state = []
			{
				synth_state s;
#ifdef _WIN32
				if (const wchar_t* value = pbd::env::find_wide("PBD_PAD_SYNTH"); value && *value)
				{
					s.enabled = true;
					s.random = 0x9e3779b97f4a7c15ull ^ (std::wcstoull(value, nullptr, 10) + 1) * 0xbf58476d1ce4e5b9ull;
				}
#endif
				return s;
			}();
			return state;
		}

		// The generator's next value (xorshift64*).
		u64 synth_next(synth_state& state)
		{
			u64 x = state.random;
			x ^= x >> 12;
			x ^= x << 25;
			x ^= x >> 27;
			state.random = x;
			return x * 0x2545f4914f6cdd1dull;
		}

		// Test only: PBD_PAD_SYNTH_MENU_SAFE=1 withholds Circle (back / cancel) while the game is not in a battle scene
		// (selector 0x1008f070 not 17/16), so an unattended synthetic session picks characters and stages and rematches instead of backing out of
		// VS mode into the main menu. The synthetic words are otherwise unchanged; fights keep all four face buttons.
		bool synth_menu_safe()
		{
			static const bool menu_safe = []
			{
#ifdef _WIN32
				const wchar_t* value = pbd::env::find_wide("PBD_PAD_SYNTH_MENU_SAFE");
				return value && std::wstring_view(value) == L"1";
#else
				return false;
#endif
			}();
			return menu_safe;
		}

		// Test only: PBD_PAD_SYNTH_MENU_WANDER=1. On character / stage select (scene 3) the plain generator
		// confirms within a fraction of a second (Cross in a quarter of its changes), so the cursor hardly moves and the same characters come
		// up match after match. Here each port alternates a WANDER stretch of 0.5-6 s (directions only, face buttons withheld) with an ACT
		// stretch of 0.2-0.8 s (generated words unchanged), so picks and the stage browse land on random entries.
		bool synth_menu_wander()
		{
			static const bool menu_wander = []
			{
#ifdef _WIN32
				const wchar_t* value = pbd::env::find_wide("PBD_PAD_SYNTH_MENU_WANDER");
				return value && std::wstring_view(value) == L"1";
#else
				return false;
#endif
			}();
			return menu_wander;
		}

		bool pbd_pad_synth(u32 port, u16& digital1, u16& digital2)
		{
			auto& state = synth();
			if (!state.enabled || port > 1)
				return false;
			static std::mutex mutex;
			std::lock_guard lock(mutex);
			const u64 now_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			if (now_us >= state.next_change_us[port])
			{
				constexpr u16 directions[8] = {0, 0, CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_RIGHT, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_UP,
					CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_RIGHT};
				state.d1[port] = directions[synth_next(state) % 8];
				u16 buttons = 0;
				const u64 r = synth_next(state);
				if (r % 3 == 0)
					buttons |= CELL_PAD_CTRL_SQUARE;
				if ((r >> 8) % 3 == 0)
					buttons |= CELL_PAD_CTRL_TRIANGLE;
				if ((r >> 16) % 4 == 0)
					buttons |= CELL_PAD_CTRL_CROSS;
				if ((r >> 24) % 4 == 0)
					buttons |= CELL_PAD_CTRL_CIRCLE;
				state.d2[port] = buttons;
				state.next_change_us[port] = now_us + 40000 + synth_next(state) % 240000;
			}
			digital1 = state.d1[port];
			digital2 = state.d2[port];
			if (synth_menu_safe() && vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4))
			{
				const u32 scene = vm::read32(pbd::t5dr::scene_address);
				if (scene != 17u && scene != 16u)
					digital2 &= ~CELL_PAD_CTRL_CIRCLE;
			}
			if (synth_menu_wander() && vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4) && vm::read32(pbd::t5dr::scene_address) == 3u)
			{
				if (now_us >= state.act_until_us[port])
				{
					const u64 r = synth_next(state);
					state.wander_until_us[port] = now_us + 500000 + r % 5500000;
					state.act_until_us[port] = state.wander_until_us[port] + 200000 + (r >> 32) % 600000;
				}
				if (now_us < state.wander_until_us[port])
					digital2 = 0;
			}
			return true;
		}

		// The record a pad with this setting reports for these buttons; the sticks are RX, RY, LX, LY.
#endif
		void pbd_fill_canonical(CellPadData& out, u32 setting, u16 digital1, u16 digital2,
			const std::array<u8, 4>& sticks = {128, 128, 128, 128})
		{
			out.len = setting & CELL_PAD_SETTING_SENSOR_ON ? CELL_PAD_LEN_CHANGE_SENSOR_ON :
			          setting & CELL_PAD_SETTING_PRESS_ON  ? CELL_PAD_LEN_CHANGE_PRESS_ON :
			                                                 CELL_PAD_LEN_CHANGE_DEFAULT;
			std::fill(std::begin(out.button), std::end(out.button), 0);
			out.button[1] = (0x7 << 4) | std::min(out.len / 2, 15);
			out.button[CELL_PAD_BTN_OFFSET_DIGITAL1] = digital1;
			out.button[CELL_PAD_BTN_OFFSET_DIGITAL2] = digital2;
			for (u32 i = 0; i < 4; ++i)
				out.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X + i] = sticks[i];
			if (setting & CELL_PAD_SETTING_PRESS_ON)
			{
				constexpr std::array<std::pair<u32, u16>, 4> d1_pressure = {{{CELL_PAD_BTN_OFFSET_PRESS_RIGHT, CELL_PAD_CTRL_RIGHT},
					{CELL_PAD_BTN_OFFSET_PRESS_LEFT, CELL_PAD_CTRL_LEFT}, {CELL_PAD_BTN_OFFSET_PRESS_UP, CELL_PAD_CTRL_UP},
					{CELL_PAD_BTN_OFFSET_PRESS_DOWN, CELL_PAD_CTRL_DOWN}}};
				constexpr std::array<std::pair<u32, u16>, 8> d2_pressure = {{{CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE, CELL_PAD_CTRL_TRIANGLE},
					{CELL_PAD_BTN_OFFSET_PRESS_CIRCLE, CELL_PAD_CTRL_CIRCLE}, {CELL_PAD_BTN_OFFSET_PRESS_CROSS, CELL_PAD_CTRL_CROSS},
					{CELL_PAD_BTN_OFFSET_PRESS_SQUARE, CELL_PAD_CTRL_SQUARE}, {CELL_PAD_BTN_OFFSET_PRESS_L1, CELL_PAD_CTRL_L1},
					{CELL_PAD_BTN_OFFSET_PRESS_R1, CELL_PAD_CTRL_R1}, {CELL_PAD_BTN_OFFSET_PRESS_L2, CELL_PAD_CTRL_L2},
					{CELL_PAD_BTN_OFFSET_PRESS_R2, CELL_PAD_CTRL_R2}}};
				for (const auto& [offset, bit] : d1_pressure)
					out.button[offset] = digital1 & bit ? 255 : 0;
				for (const auto& [offset, bit] : d2_pressure)
					out.button[offset] = digital2 & bit ? 255 : 0;
			}
			if (setting & CELL_PAD_SETTING_SENSOR_ON)
			{
				out.button[CELL_PAD_BTN_OFFSET_SENSOR_X] = DEFAULT_MOTION_X;
				out.button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = DEFAULT_MOTION_Y;
				out.button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = DEFAULT_MOTION_Z;
				out.button[CELL_PAD_BTN_OFFSET_SENSOR_G] = DEFAULT_MOTION_G;
			}
		}

		// The pad's accumulated state (not the possibly "no change" record just written): digital words as the device
		// reports them, the left stick folded into directions when the handler provides one (0/0 = no analog data).
		void canonical_record(const port_read& r, const Pad& pad)
		{
			const CellPadData& live = pad.ldd ? pad.ldd_data : pad.data;
			u16 digital1 = live.button[CELL_PAD_BTN_OFFSET_DIGITAL1], digital2 = live.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
			const u16 lx = live.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X], ly = live.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
			if (!pad.ldd && (lx || ly))
			{
				if (lx < 64)
					digital1 |= CELL_PAD_CTRL_LEFT;
				if (lx > 192)
					digital1 |= CELL_PAD_CTRL_RIGHT;
				if (ly < 64)
					digital1 |= CELL_PAD_CTRL_UP;
				if (ly > 192)
					digital1 |= CELL_PAD_CTRL_DOWN;
			}
#ifdef PBD_DIAGNOSTIC_BUILD
			if (!pad.ldd)
				pbd_pad_synth(r.port, digital1, digital2);
#endif
			pbd_fill_canonical(r.data, r.config.port_setting[r.port], digital1, digital2);
		}

		// Whether both game ports are connected virtual (LDD) pads.
		bool virtual_pads(const port_read& r, const port_pads& pads)
		{
			return pads.size() >= 2 && pads[0] && pads[1] && pads[0]->ldd && pads[1]->ldd &&
			       r.config.is_reportedly_connected(0) && r.config.is_reportedly_connected(1) && pads[0]->is_connected() && pads[1]->is_connected();
		}

		// Closed-loop first-run channel (pbd_input_live.h). Port 0 only; port 1 stays the virtual pad.
		void live_input(const port_read& r, const port_pads& pads)
		{
			pbd::input_live::pad_facts facts;
			facts.virtual_pads = virtual_pads(r, pads);
			if (facts.virtual_pads)
			{
				const CellPadData& other = pads[1]->ldd_data;
				facts.port1_neutral = other.button[CELL_PAD_BTN_OFFSET_DIGITAL1] == 0 && other.button[CELL_PAD_BTN_OFFSET_DIGITAL2] == 0 &&
				                      other.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] == 128 && other.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] == 128 &&
				                      other.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] == 128 && other.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] == 128;
			}
			const auto [digital1, digital2] = pbd::input_live::get().next(facts);
			pbd_fill_canonical(r.data, r.config.port_setting[r.port], digital1, digital2);
		}

		// First-run setup: after every press, release or wait action, the menu state just before the next transition. The
		// record goes to the verification report only; the input script alone decides what is pressed.
		struct first_run_observation
		{
			u64 poll = 0;
			usz event_index = 0, action = 0;
			bool fields_read = false;
			u32 scene = ~0u, subscene = ~0u, mode = ~0u;
			std::array<u8, 6> option_bytes{};
			u8 rounds_preference = 0xff, rounds_to_win = 0xff, mode_copy = 0xff;
		};

		std::vector<first_run_observation>& first_run_observations()
		{
			static std::vector<first_run_observation> rows;
			return rows;
		}

		void observe_first_run(const pbd::input_script_output& scripted)
		{
			first_run_observation row;
			row.poll = scripted.poll;
			row.event_index = scripted.event_index;
			row.action = scripted.completed_action;
			std::array<be_t<u32>, 2> scene{};
			be_t<u32> mode{};
			row.fields_read = vm::try_access(pbd::t5dr::scene_address, scene.data(), sizeof(scene), false) &&
			                  vm::try_access(pbd::t5dr::game_mode_address, &mode, sizeof(mode), false) &&
			                  vm::try_access(0x10684964, row.option_bytes.data(), row.option_bytes.size(), false) &&
			                  vm::try_access(0x1008ebd4, &row.mode_copy, sizeof(row.mode_copy), false);
			if (row.fields_read)
			{
				row.scene = scene[0];
				row.subscene = scene[1];
				row.mode = mode;
				row.rounds_preference = row.option_bytes[1];
				row.rounds_to_win = row.rounds_preference < 0xff ? row.rounds_preference + 1 : 0xff;
			}
			first_run_observations().push_back(row);
			pbd_input_script_log.notice("First-run observation action %u/%u poll %u: scene %u/%u mode %u rounds raw %u decoded %u copy %u",
				row.action, scripted.completed_actions_total, row.poll, row.scene, row.subscene, row.mode,
				row.rounds_preference, row.rounds_to_win, row.mode_copy);
		}

		std::string first_run_observations_json()
		{
			const auto& rows = first_run_observations();
			std::string observations = "[";
			for (usz i = 0; i < rows.size(); ++i)
			{
				const auto& row = rows[i];
				if (i)
					observations += ',';
				fmt::append(observations,
					"{\"action\":%u,\"event_index\":%u,\"poll\":%u,\"fields_read\":%s,"
					"\"scene\":%u,\"subscene\":%u,\"mode_word\":%u,\"options_10684964_10684969\":[%u,%u,%u,%u,%u,%u],"
					"\"rounds_preference_index\":%u,\"rounds_to_win\":%u,\"mode_copy\":%u}",
					row.action, row.event_index, row.poll, row.fields_read ? "true" : "false", row.scene, row.subscene, row.mode,
					row.option_bytes[0], row.option_bytes[1], row.option_bytes[2], row.option_bytes[3], row.option_bytes[4], row.option_bytes[5],
					row.rounds_preference, row.rounds_to_win, row.mode_copy);
			}
			observations += ']';
			return observations;
		}

		// Bind a first-run save request to values read from guest memory at the
		// exact final tape poll. This record deliberately does not claim that the asynchronous savestate
		// write has completed or hash the resulting host file; the host tooling adds that separately
		// after RPCS3 reports "Saved savestate!". Absent/unreadable/wrong values leave capture_valid false,
		// and the host wrapper refuses publication. Only the first-run checkpoint tooling sets this switch.
		bool write_first_run_verification(const port_read& r, const port_pads& pads, const pbd::input_script_output& scripted,
			const wchar_t* verification_path, const wchar_t* run_id_wide, const wchar_t* exe_sha_wide)
		{
			std::array<be_t<u32>, 2> scene{};
			be_t<u32> mode{};
			u8 rounds_preference = 0xff, rounds_source = 0xff;
			const bool fields_read = vm::try_access(pbd::t5dr::scene_address, scene.data(), sizeof(scene), false) &&
			                         vm::try_access(pbd::t5dr::game_mode_address, &mode, sizeof(mode), false) &&
			                         vm::try_access(0x10684965, &rounds_preference, sizeof(rounds_preference), false) &&
			                         vm::try_access(0x1008ebd4, &rounds_source, sizeof(rounds_source), false);
			const bool final_neutral = scripted.sample.digital1 == 0 && scripted.sample.digital2 == 0 &&
			                           scripted.sample.sticks[0] == 128 && scripted.sample.sticks[1] == 128 &&
			                           scripted.sample.sticks[2] == 128 && scripted.sample.sticks[3] == 128;
			const bool virtual_ports = virtual_pads(r, pads);
			const auto neutral = [](const CellPadData& value)
			{
				return value.button[CELL_PAD_BTN_OFFSET_DIGITAL1] == 0 && value.button[CELL_PAD_BTN_OFFSET_DIGITAL2] == 0 &&
				       value.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] == 128 && value.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] == 128 &&
				       value.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] == 128 && value.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] == 128;
			};
			const bool pads_neutral = final_neutral && virtual_ports && neutral(pads[1]->ldd_data);
			const bool observations_complete = first_run_observations().size() == scripted.completed_actions_total;
			const bool capture_valid = fields_read && observations_complete && u32{scene[0]} == 3 && u32{scene[1]} == 44 && u32{mode} == 4 &&
			                           rounds_preference == 2 && rounds_source == 3 && pads_neutral;
			const std::wstring_view run_wide(run_id_wide), sha_wide(exe_sha_wide);
			const std::string run_id(run_wide.begin(), run_wide.end()), exe_sha(sha_wide.begin(), sha_wide.end());
			const std::string observations = first_run_observations_json();
			const auto text = fmt::format(
				"{\"schema\":\"dr2026-first-run-verification-v1\",\"emitter\":\"dr2026-first-run-native\","
				"\"run_id\":\"%s\",\"producer_executable_sha256\":\"%s\",\"capture_valid\":%s,"
				"\"scene\":%u,\"subscene\":%u,\"effective_options\":{\"mode\":\"%s\",\"mode_word\":%u,"
				"\"rounds_preference_index\":%u,\"rounds_to_win\":%u},"
				"\"pads\":{\"virtual_ports\":[0,1],\"connected\":%s,\"neutral\":%s,"
				"\"neutral_basis\":[\"final_input_tape_sample_port0\",\"connected_ldd_port1\"]},"
				"\"load_events\":{\"observed\":[\"input_tape_final\",\"scene_state_read\",\"options_read\","
				"\"virtual_pad_state_read\",\"savestate_request\"],\"unobserved\":[\"savedata_file_reads\","
				"\"archive_reads\",\"decoder_data_tasks\",\"spu_audio\",\"rsx_upload_flip_vblank\","
				"\"savestate_write_completion\"]},\"observations_complete\":%s,\"expected_actions\":%u,"
				"\"observations\":%s,\"tape_final_poll\":%u}\n",
				run_id, exe_sha, capture_valid ? "true" : "false", fields_read ? u32{scene[0]} : ~0u,
				fields_read ? u32{scene[1]} : ~0u, fields_read && u32{mode} == 4 ? "vs_battle" : "unknown",
				fields_read ? u32{mode} : ~0u, rounds_preference, rounds_source, virtual_ports ? "true" : "false",
				pads_neutral ? "true" : "false", observations_complete ? "true" : "false", scripted.completed_actions_total,
				observations, scripted.poll);
			const auto path_utf8 = std::filesystem::path(verification_path).u8string();
			if (fs::file out(std::string(path_utf8.begin(), path_utf8.end()), fs::rewrite); out)
			{
				out.write(text.data(), text.size());
				pbd_input_script_log.warning("First-run native verification at tape poll %u: %s", scripted.poll, capture_valid ? "PASS" : "REFUSED");
			}
			else
			{
				pbd_input_script_log.error("First-run native verification output could not be opened; identity publication will refuse");
			}
			return capture_valid;
		}

		// PBD_INPUT_TAPE_SAVESTATE=1 saves a savestate and stops when the tape's LAST event is applied,
		// so a character-select savestate can be made from a fresh boot without keyboard input or window focus.
		void finish_script(const port_read& r, const port_pads& pads, const pbd::input_script_output& scripted, const wchar_t* verification_path)
		{
			static atomic_t<bool> pbd_script_saved{false};
			const wchar_t* flag = pbd::env::find_wide("PBD_INPUT_TAPE_SAVESTATE");
			if (!flag || std::wstring_view(flag) != L"1" || pbd_script_saved.exchange(true))
				return;
			const wchar_t* run_id_wide = pbd::env::find_wide("PBD_FIRST_RUN_RUN_ID");
			const wchar_t* exe_sha_wide = pbd::env::find_wide("PBD_FIRST_RUN_EXE_SHA256");
			bool verified = !verification_path || !*verification_path;
			if (verification_path && *verification_path && run_id_wide && *run_id_wide && exe_sha_wide && *exe_sha_wide)
				verified = write_first_run_verification(r, pads, scripted, verification_path, run_id_wide, exe_sha_wide);
			pbd_input_script_log.warning("Input tape finished on port %u at poll %u: %s and stopping (PBD_INPUT_TAPE_SAVESTATE)",
				r.port, scripted.poll, verified ? "saving a savestate" : "verification refused; not saving");
			Emu.CallFromMainThread([verified]()
				{
					Emu.Kill(!verified, verified);
				});
		}

		void script_input(const port_read& r, const port_pads& pads, const pbd::input_script_output& scripted)
		{
			const wchar_t* verification_path = pbd::env::find_wide("PBD_FIRST_RUN_VERIFY_PATH");
			if (r.port == 0 && verification_path && *verification_path && scripted.completed_action)
				observe_first_run(scripted);
			if (scripted.poll % 300 == 0)
				pbd_input_script_log.notice("Input tape port %u at poll %u (digital1 0x%x digital2 0x%x)", r.port, scripted.poll, scripted.sample.digital1, scripted.sample.digital2);
			if (scripted.final)
				finish_script(r, pads, scripted, verification_path);
			pbd_fill_canonical(r.data, r.config.port_setting[r.port], scripted.sample.digital1, scripted.sample.digital2,
				scripted.sample.sticks);
			if (r.observation)
			{
				r.observation->has_script_poll = true;
				r.observation->script_poll = scripted.poll;
			}
			// Force the real device state to be sent on the first poll after release.
			if (scripted.final)
				pads[r.port]->m_buffer_cleared = true;
		}

		// Explicit diagnostic override. Device connection/error behavior is retained;
		// system input interception pauses the tape. Background focus does not. True when a script wrote the record.
		bool scripted_input(const port_read& r, const port_pads& pads)
		{
			const PadInfo& rinfo = r.handler.GetInfo();
			if (!(rinfo.system_info & CELL_PAD_INFO_INTERCEPTED) && !rinfo.ignore_input)
			{
				if (r.port == 0 && pbd::input_live::get().enabled())
				{
					live_input(r, pads);
					return true;
				}
				if (const auto scripted = pbd::get_input_script().next(r.port))
				{
					script_input(r, pads, *scripted);
					return true;
				}
				return false;
			}
			if (r.port == 0 && pbd::input_live::get().enabled())
			{
				pbd::input_live::get().note_intercepted();
			}
			return false;
		}

		// The replay latch injects both sides from its ledger. Real host input belongs exclusively to the viewer,
		// including before the first fight and during a paused menu; read_pad() still sees m_buttons_external above.
		// Never neutralize LDD/tape sources, which are also used by the replay's checkpoint setup.
		void neutral_for_viewer(const port_read& r, const Pad& pad, bool scripted)
		{
			static const bool pbd_viewer_owns_input = []
			{
				const auto viewer = pbd::env::find("PBD_REPLAY_VIEWER");
				const auto controls = pbd::env::find("PBD_REPLAY_CONTROLS");
				return viewer && std::string_view(viewer) == "1" && controls && std::string_view(controls) == "1";
			}();
			if (pbd_viewer_owns_input && !pad.ldd && !scripted && r.data.len > CELL_PAD_BTN_OFFSET_DIGITAL2)
			{
				r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL1] = 0;
				r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL2] = 0;
				for (u32 i = CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X; i <= CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y; ++i)
					r.data.button[i] = 128;
				for (u32 i = CELL_PAD_BTN_OFFSET_PRESS_RIGHT; i <= CELL_PAD_BTN_OFFSET_PRESS_R2; ++i)
					r.data.button[i] = 0;
			}
		}

		// Every guest reader receives neutral words while Practice owns the paused frame, or while the in-game Settings menu
		// holds this PC's controllers in Practice or offline play (until their buttons are released after it closes),
		// including the second port.
		void neutral_for_menus(const port_read& r)
		{
			const bool pbd_menu_owns_ports = pbd_practice_owns_input() || pbd::session_mode::offline_play();
			if (pbd::practice::input_claim.load(std::memory_order_acquire) ||
				(pbd_menu_owns_ports && pbd::settings_menu::claimed()))
			{
				for (u32 offset = CELL_PAD_BTN_OFFSET_DIGITAL1; offset < std::min<u32>(r.data.len, CELL_PAD_MAX_CODES); ++offset)
					r.data.button[offset] = offset >= CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X && offset <= CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y ? 128 : 0;
			}
		}

		// Offline play's Versus (Emu/Cell/pbd_versus.h): at its character select Triangle opens Controller Setup instead of reaching
		// the game, and while a menu, a panel or a steered select holds a port the game gets the word it gives, sticks centred.
		// It steps on every read, since a read with nothing new carries no record (len 0) and the game keeps the last one: a held
		// port therefore gets a whole record each read, and once it is let go the pad resends its own state.
		void versus(const port_read& r)
		{
			if (!pbd::session_mode::offline_play() || r.port >= 2 || pbd::settings_menu::claimed())
				return;
			static std::array<bool, 2> holding{};
			const auto given = pbd::versus::at_read(r.port);
			if (given.word)
			{
				holding[r.port] = true;
				if (r.data.len < CELL_PAD_LEN_CHANGE_DEFAULT)
					r.data.len = CELL_PAD_LEN_CHANGE_DEFAULT;
				r.data.button[0] = 0;
				r.data.button[1] = (0x7 << 4) | std::min<u32>(r.data.len / 2, 15);
				for (u32 offset = CELL_PAD_BTN_OFFSET_DIGITAL1; offset < std::min<u32>(r.data.len, CELL_PAD_MAX_CODES); ++offset)
					r.data.button[offset] = offset >= CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X && offset <= CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y ? 128 : 0;
				r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL1] = *given.word >> 8;
				r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL2] = *given.word & 0xff;
				return;
			}
			if (holding[r.port])
			{
				holding[r.port] = false;
				r.handler.GetPads()[r.port]->m_buffer_cleared = true;
			}
			if (r.data.len <= CELL_PAD_BTN_OFFSET_DIGITAL2)
				return;
			if (given.hidden || given.added)
			{
				const u16 digital1 = r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL1], digital2 = r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
				r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL1] = ((digital1 & ~(given.hidden >> 8)) | (given.added >> 8)) & 0xff;
				r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL2] = ((digital2 & ~given.hidden) | given.added) & 0xff;
				if (r.data.len > CELL_PAD_BTN_OFFSET_PRESS_L1)
				{
					// The pressure bytes follow the buttons the game now gets: Triangle's is the press L1 gave it.
					const u16 l1 = r.data.button[CELL_PAD_BTN_OFFSET_PRESS_L1];
					r.data.button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = (given.added & CELL_PAD_CTRL_TRIANGLE) ? (l1 ? l1 : u16{255}) : u16{0};
					if (given.hidden & CELL_PAD_CTRL_L1)
						r.data.button[CELL_PAD_BTN_OFFSET_PRESS_L1] = 0;
				}
			}
		}

		// Practice consumes SELECT, and START from the START+SELECT combination until it is released.
		void practice_start_select(const port_read& r)
		{
			if (pbd_practice_owns_input() && r.data.len > CELL_PAD_BTN_OFFSET_DIGITAL1)
			{
				static std::array<bool, CELL_MAX_PADS> start_held{};
				const u16 raw = r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
				const bool select = (raw & CELL_PAD_CTRL_SELECT) != 0;
				const bool start = (raw & CELL_PAD_CTRL_START) != 0;
				start_held[r.port] = start && (select || start_held[r.port]);
				r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL1] = raw & ~CELL_PAD_CTRL_SELECT;
				if (start_held[r.port])
					r.data.button[CELL_PAD_BTN_OFFSET_DIGITAL1] &= ~CELL_PAD_CTRL_START;
			}
		}

		// The record the game got, for the input diagnostics.
		void observe(const port_read& r)
		{
			if (r.observation)
			{
				r.observation->has_length = true;
				r.observation->length = r.data.len;
				if (r.observation->length > 0)
				{
					r.observation->button_count = std::min<u32>(static_cast<u32>(r.observation->length), static_cast<u32>(r.observation->button_words.size()));
					for (u32 index = 0; index < r.observation->button_count; ++index)
					{
						r.observation->button_words[index] = r.data.button[index];
					}
				}
			}
		}
	} // namespace

	void note_read(u32 port)
	{
		if (port < 2)
			g_pbd_pad_getdata_ns[port].store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed); // when the game last read this port, for the input-age measurement
	}

	void after_read(const port_read& r)
	{
		const auto& pads = r.handler.GetPads();
		const auto& pad = pads[r.port];
		// This read has taken the buttons this PC's controller tapped since the game's previous read (Input/pbd_controller_ports.h).
		r.handler.pbd_game_read(*pad);
		if (pbd_pad_canonical())
			canonical_record(r, *pad);
		const bool scripted = scripted_input(r, pads);
		neutral_for_viewer(r, *pad, scripted);
		neutral_for_menus(r);
		versus(r);
		practice_start_select(r);
		observe(r);
	}

	error_code observed(u32 port, vm::ptr<CellPadData> data, get_data_function get_data)
	{
		auto& trace_writer = pbd::input_trace_writer::get();
		if (!trace_writer.enabled())
		{
			return get_data(port, data, nullptr);
		}

		pbd::input_observation observation;
		observation.port = port;
		observation.data_address = data.addr();
		if (const auto ppu = get_current_cpu_thread<ppu_thread>())
		{
			observation.has_ppu = true;
			observation.ppu_id = ppu->id;
			observation.ppu_pc = ppu->cia;
			observation.ppu_lr = ppu->lr;
		}
		if (const auto rsx = rsx::get_current_renderer())
		{
			observation.has_vblank = true;
			observation.vblank = rsx->vblank_count.load();
			// int_flip_index is non-atomic RSX-owned state; do not race it here.
		}
		const error_code result = get_data(port, data, &observation);
		trace_writer.append(observation, result);
		return result;
	}
} // namespace pbd::pad_read
