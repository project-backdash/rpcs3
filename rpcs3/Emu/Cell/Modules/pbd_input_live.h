#pragma once

// Commands drive first-run checkpoint input one pad poll at a time and publish observations atomically.
// Checkpoint requests validate the native select state and neutral pads before saving.

#include "Emu/PBD/pbd_input_command_parser.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_checkpoint_compatibility.h"
#include "Emu/PBD/pbd_t5dr_memory.h"
#include "util/endian.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbd
{
	namespace test
	{
		struct input_live_fixture;
	}

	class input_live : public input_command_model
	{
	public:
		static input_live& get()
		{
			static input_live instance;
			return instance;
		}
		bool enabled() const
		{
			return m_enabled;
		}
		const std::string& refusal() const
		{
			return m_refusal;
		}

		// Called for port 0 on every non-intercepted pad poll. Returns the digital words to apply (sticks stay centred).
		std::pair<u16, u16> next(const pad_facts& pads)
		{
			m_poll++;
			if (const auto scene = read_field(field_ref{"scene", pbd::t5dr::scene_address, 4}); scene && *scene != m_scene_value)
			{
				m_scene_value = *scene;
				m_scene_since = m_poll;
			}
			if (m_finished)
				return {0, 0};
			if (!m_started)
			{
				m_started = true;
				m_last_command_poll = m_poll;
				write_observation(command{0, "ready"}, "ready", read_all(), {});
			}
			if (!m_active)
			{
				if (m_poll - m_last_command_poll > m_deadman_polls)
				{
					write_observation(command{m_seq, "deadman"}, "deadman", read_all(), {});
					finish(false);
					return {0, 0};
				}
				if (m_poll % 2 == 0)
					try_load_command();
				if (!m_active)
					return {0, 0};
			}
			return step(pads);
		}

		// Called when system input interception suppresses the poll: a press in progress is aborted.
		void note_intercepted()
		{
			m_intercepted++;
			if (m_active && m_phase != phase::ack && m_phase != phase::idle)
			{
				m_history_ok = false;
				write_observation(m_cmd, "intercepted", read_all(), {});
				end_command();
			}
		}

	private:
		friend struct test::input_live_fixture;
		enum class phase
		{
			idle,
			pre,
			press,
			release,
			ack
		};
		bool m_enabled = false, m_started = false, m_active = false, m_finished = false;
		std::string m_refusal;
		std::filesystem::path m_dir, m_verify_path;
		std::string m_run_id, m_exe_sha, m_checkpoint_kind = "vs";
		u64 m_poll = 0, m_phase_poll = 0, m_cmd_poll = 0, m_last_command_poll = 0, m_deadman_polls = 3600;
		u32 m_seq = 1, m_stable = 0, m_intercepted = 0;
		u32 m_press_poll = 0, m_release_poll = 0;
		u32 m_pressed_polls = 0, m_released_polls = 0; // guest pad reads that returned the pressed / released sample (consumption)
		phase m_phase = phase::idle;
		command m_cmd;
		std::vector<std::pair<std::string, u64>> m_pre; // field values when the command started (for `changed`)
		std::vector<std::string> m_history;             // one compact JSON row per completed command (checkpoint record)
		bool m_history_ok = true;
		u64 m_scene_value = ~u64{0}, m_scene_since = 0; // guest scene and the poll it last changed

		static std::string wide_env(const char* name)
		{
#ifdef _WIN32
			const wchar_t* value = pbd::env::find_wide(name);
			if (!value)
				return {};
			const std::wstring_view view(value);
			return std::string(view.begin(), view.end());
#else
			(void)name;
			return {};
#endif
		}

		input_live()
		{
#ifdef _WIN32
			const wchar_t* dir = pbd::env::find_wide("PBD_INPUT_LIVE_DIR");
			if (!dir || !*dir)
				return;
			m_dir = std::filesystem::path(dir);
			const bool practice_session_gate = wide_env("PBD_SESSION") == "1" &&
			                                   wide_env("PBD_PRACTICE_REWIND") == "1" &&
			                                   !wide_env("PBD_PRACTICE_REWIND_TEST").empty();
			const bool replay_session_gate = wide_env("PBD_SESSION") == "1" && !wide_env("PBD_REPLAY_V2_GAME").empty();
			for (const char* conflicting : {"PBD_INPUT_TAPE", "PBD_PEER_ROLE", "PBD_PAD_SYNTH", "PBD_INPUT_TAPE_SAVESTATE"})
			{
				const wchar_t* v = pbd::env::find_wide(conflicting);
				if (v && *v)
				{
					m_refusal = "conflicting environment variable set";
					return;
				}
			}
			if (!practice_session_gate && !replay_session_gate)
			{
				const wchar_t* session = pbd::env::find_wide("PBD_SESSION");
				if (session && *session)
				{
					m_refusal = "conflicting environment variable set";
					return;
				}
			}
			std::error_code ec;
			if (!m_dir.is_absolute() || !std::filesystem::is_directory(m_dir, ec))
			{
				m_refusal = "PBD_INPUT_LIVE_DIR must be an existing absolute directory";
				return;
			}
			m_run_id = wide_env("PBD_INPUT_LIVE_RUN_ID");
			m_exe_sha = wide_env("PBD_FIRST_RUN_EXE_SHA256");
			if (const auto kind = wide_env("PBD_FIRST_RUN_CHECKPOINT_KIND"); !kind.empty())
				m_checkpoint_kind = kind;
			if (m_run_id.empty() || m_run_id.size() > 80)
			{
				m_refusal = "PBD_INPUT_LIVE_RUN_ID missing";
				return;
			}
			if (m_checkpoint_kind != "vs" && m_checkpoint_kind != "practice")
			{
				m_refusal = "PBD_FIRST_RUN_CHECKPOINT_KIND must be vs or practice";
				return;
			}
			if (const wchar_t* verify = pbd::env::find_wide("PBD_FIRST_RUN_VERIFY_PATH"); verify && *verify)
				m_verify_path = verify;
			if (const auto deadman = wide_env("PBD_INPUT_LIVE_DEADMAN_POLLS"); !deadman.empty())
				m_deadman_polls = std::clamp<u64>(std::strtoull(deadman.c_str(), nullptr, 10), 300, 36000);
			m_enabled = true;
#endif
		}

		static std::string utf8(const std::filesystem::path& p)
		{
			const auto u = p.u8string();
			return std::string(u.begin(), u.end());
		}

		static std::optional<u64> read_field(const field_ref& f)
		{
			std::array<u8, 4> bytes{};
			if (!vm::try_access(f.addr, bytes.data(), f.width, false))
				return std::nullopt;
			u64 value = 0;
			for (u32 i = 0; i < f.width; i++)
				value = (value << 8) | bytes[i]; // guest memory is big-endian
			return value;
		}

		std::vector<std::pair<std::string, std::optional<u64>>> read_all() const
		{
			std::vector<std::pair<std::string, std::optional<u64>>> out;
			for (const auto& f : known_fields)
				out.emplace_back(f.name, read_field(field_ref{f.name, f.addr, f.width}));
			for (const auto& p : m_cmd.ack)
			{
				bool known = false;
				for (const auto& f : known_fields)
					known |= p.field.name == f.name;
				if (!known)
					out.emplace_back(p.field.name, read_field(p.field));
			}
			return out;
		}

		static std::string to_json(const std::vector<std::pair<std::string, std::optional<u64>>>& values)
		{
			std::string s = "{";
			for (usz i = 0; i < values.size(); i++)
			{
				if (i)
					s += ',';
				if (values[i].second)
					fmt::append(s, "\"%s\":%u", values[i].first, *values[i].second);
				else
					fmt::append(s, "\"%s\":null", values[i].first);
			}
			return s + "}";
		}

		void try_load_command()
		{
			const auto path = m_dir / fmt::format("cmd-%06u.txt", m_seq);
			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
				return;
			fs::file file(utf8(path));
			if (!file)
				return;
			const std::string text = file.to_string();
			file.close();
			command cmd;
			std::string error;
			input_command_parser reader{m_run_id, m_seq, cmd, error};
			if (!reader.parse(text))
			{
				command refused{m_seq, "refused"};
				write_observation(refused, "refused:" + error, read_all(), {});
				m_history.push_back(fmt::format("{\"seq\":%u,\"op\":\"refused\",\"status\":\"refused\"}", m_seq));
				m_history_ok = false;
				m_seq++;
				m_last_command_poll = m_poll;
				return;
			}
			m_cmd = std::move(cmd);
			m_active = true;
			m_cmd_poll = m_phase_poll = m_poll;
			m_last_command_poll = m_poll;
			m_stable = 0;
			m_press_poll = m_release_poll = 0;
			m_pressed_polls = m_released_polls = 0;
			m_pre.clear();
			for (const auto& p : m_cmd.ack)
				if (const auto v = read_field(p.field))
					m_pre.emplace_back(p.field.name, *v);
			m_phase = m_cmd.op == "press" ? phase::pre : m_cmd.op == "wait" ? phase::ack :
			                                                                  phase::idle;
		}

		bool evaluate() const
		{
			if (m_cmd.ack.empty())
				return true;
			bool any = false, all = true;
			for (const auto& p : m_cmd.ack)
			{
				const auto v = read_field(p.field);
				bool ok = false;
				if (v)
				{
					switch (p.op)
					{
					case cmp::eq: ok = *v == p.value; break;
					case cmp::ne: ok = *v != p.value; break;
					case cmp::lt: ok = *v < p.value; break;
					case cmp::gt: ok = *v > p.value; break;
					case cmp::changed:
						for (const auto& [name, before] : m_pre)
							if (name == p.field.name)
								ok = *v != before;
						break;
					}
				}
				any |= ok;
				all &= ok;
			}
			return m_cmd.ack_any ? any : all;
		}

		std::pair<u16, u16> step(const pad_facts& pads)
		{
			const u64 in_phase = m_poll - m_phase_poll;
			switch (m_phase)
			{
			case phase::idle:
				// observe / checkpoint / stop act at once
				if (m_cmd.op == "observe")
				{
					complete("done");
				}
				else if (m_cmd.op == "stop")
				{
					complete("stopped");
					finish(false);
				}
				else if (m_cmd.op == "checkpoint")
				{
					checkpoint(pads);
				}
				return {0, 0};
			case phase::pre:
				// at least two neutral polls before the press
				if (in_phase >= 2)
				{
					m_phase = phase::press;
					m_phase_poll = m_poll;
					m_press_poll = static_cast<u32>(m_poll - m_cmd_poll);
					m_pressed_polls = 1;
					return {m_cmd.digital1, m_cmd.digital2};
				}
				return {0, 0};
			case phase::press:
				if (m_pressed_polls >= m_cmd.hold_polls)
				{
					m_phase = m_cmd.release_polls == 1 ? phase::ack : phase::release;
					m_phase_poll = m_poll;
					m_release_poll = static_cast<u32>(m_poll - m_cmd_poll);
					m_released_polls = 1;
					return {0, 0};
				}
				m_pressed_polls++;
				return {m_cmd.digital1, m_cmd.digital2};
			case phase::release:
				if (++m_released_polls >= m_cmd.release_polls)
				{
					m_phase = phase::ack;
					m_phase_poll = m_poll;
				}
				return {0, 0};
			case phase::ack:
				if (evaluate())
				{
					if (++m_stable >= m_cmd.stable_polls)
						complete(m_cmd.ack.empty() ? "done" : "acked");
				}
				else
					m_stable = 0;
				if (m_active && in_phase >= m_cmd.timeout_polls)
					complete("timeout");
				return {0, 0};
			}
			return {0, 0};
		}

		void complete(const std::string& status)
		{
			const auto values = read_all();
			std::vector<std::string> files;
			for (const auto& d : m_cmd.dumps)
			{
				std::vector<u8> buffer(d.size);
				const auto name = fmt::format("dump-%06u-%08x.bin", m_cmd.seq, d.addr);
				if (vm::try_access(d.addr, buffer.data(), d.size, false))
				{
					fs::file out(utf8(m_dir / name), fs::rewrite);
					if (out && out.write(buffer.data(), buffer.size()) == buffer.size())
						files.push_back(name);
				}
			}
			write_observation(m_cmd, status, values, files);
			std::string row = fmt::format("{\"seq\":%u,\"op\":\"%s\",\"status\":\"%s\",\"poll\":%u,\"fields\":%s}", m_cmd.seq, m_cmd.op, status, m_poll, to_json(values));
			m_history.push_back(std::move(row));
			if (status == "timeout")
				m_history_ok = false;
			end_command();
		}

		void end_command()
		{
			m_active = false;
			m_phase = phase::idle;
			m_seq++;
			m_last_command_poll = m_poll;
		}

		void write_observation(const command& cmd, const std::string& status, const std::vector<std::pair<std::string, std::optional<u64>>>& values,
			const std::vector<std::string>& dumps) const
		{
			bool read = true;
			for (const auto& v : values)
				read &= v.second.has_value();
			std::string dump_list = "[";
			for (usz i = 0; i < dumps.size(); i++)
				fmt::append(dump_list, "%s\"%s\"", i ? "," : "", dumps[i]);
			dump_list += "]";
			const auto text = fmt::format("{\"schema\":\"dr2026-input-live-obs-v1\",\"run_id\":\"%s\",\"seq\":%u,\"op\":\"%s\",\"status\":\"%s\","
										  "\"poll\":%u,\"command_poll\":%u,\"press_offset\":%u,\"release_offset\":%u,\"buttons\":[%u,%u],\"intercepted_polls\":%u,"
										  "\"pressed_polls\":%u,\"released_polls\":%u,"
										  "\"fields_read\":%s,\"fields\":%s,\"dumps\":%s}\n",
				m_run_id, cmd.seq, cmd.op, status, m_poll, m_cmd_poll, m_press_poll, m_release_poll, cmd.digital1, cmd.digital2, m_intercepted,
				m_pressed_polls, m_released_polls,
				read ? "true" : "false", to_json(values), dump_list);
			const auto final_path = m_dir / fmt::format("obs-%06u.json", cmd.seq);
			const auto temp_path = m_dir / fmt::format("obs-%06u.json.tmp", cmd.seq);
			{
				fs::file out(utf8(temp_path), fs::rewrite);
				if (!out)
					return;
				out.write(text.data(), text.size());
			}
			fs::rename(utf8(temp_path), utf8(final_path), true);
		}

		struct checkpoint_reading
		{
			bool practice = false, prefs_read = false, keys_read = false, prefs_ok = false, keys_ok = false, fields_read = false;
			std::array<u8, 11> prefs{};
			std::array<u8, 16> keys{};
			std::optional<u64> scene, subscene, mode, source, mode_req, cursor1, cursor2;
			std::optional<u64> hand1, hand2, hand3, hand4, human, pad1, pad2, found1, found2, last1, last2;
			u64 stable_polls = 0;
		};
		struct check
		{
			const char* name;
			bool ok;
		};

		checkpoint_reading read_checkpoint()
		{
			checkpoint_reading capture;
			// Capture kinds share the scene-stability, mapping, neutral-pad and build-binding gates, but validate their native
			// gameplay modes separately. Practice keeps its one-human ownership and never rewrites the session save fields.
			const auto get = [](const char* name, u32 addr, u8 width)
			{
				return read_field(field_ref{name, addr, width});
			};
			capture.practice = m_checkpoint_kind == "practice";
			capture.prefs_read = vm::try_access(0x10684964, capture.prefs.data(), capture.prefs.size(), false);
			capture.keys_read = vm::try_access(0x10684971, capture.keys.data(), capture.keys.size(), false);
			capture.scene = get("scene", pbd::t5dr::scene_address, 4);
			capture.subscene = get("subscene", pbd::t5dr::subscene_address, 4);
			capture.mode = get("mode", pbd::t5dr::game_mode_address, 4);
			capture.source = get("rounds_source", 0x1008ebd4, 1);
			capture.mode_req = get("mode_req", 0x1008f064, 4);
			capture.cursor1 = get("cursor1", 0x10093f58, 4);
			capture.cursor2 = get("cursor2", 0x10093fd0, 4);
			capture.hand1 = get("handicap_p1", 0x1008ebf6, 2);
			capture.hand2 = get("handicap_p2", 0x1008ebfa, 2);
			capture.hand3 = get("handicap_s1", 0x10093fc0, 2);
			capture.hand4 = get("handicap_s2", 0x10094038, 2);
			capture.human = get("human_mask", 0x1008f0b0, 4);
			capture.pad1 = get("select_pad_p1", 0x10093f7b, 1);
			capture.pad2 = get("select_pad_p2", 0x10093ff3, 1);
			// The VS options digest requires saved characters 5/6 before capture; Practice preserves the save's character fields.
			capture.found1 = get("saved_last_p1", 0x10684990, 4);
			capture.found2 = get("saved_last_p2", 0x10684994, 4);
			if (!capture.practice && capture.found1 && capture.found2 && (*capture.found1 != 5 || *capture.found2 != 6))
			{
				be_t<u32> agreed1{5}, agreed2{6};
				// The verification record keeps the values found (saved_last_characters_found); a failed write fails the check below.
				vm::try_access(0x10684990, &agreed1, sizeof(agreed1), true);
				vm::try_access(0x10684994, &agreed2, sizeof(agreed2), true);
			}
			capture.last1 = get("saved_last_p1", 0x10684990, 4);
			capture.last2 = get("saved_last_p2", 0x10684994, 4);
			constexpr std::array<u8, 6> agreed_vs_prefs{1, 2, 2, 0, 0, 0};
			constexpr std::array<u8, 6> agreed_practice_prefs{1, 1, 2, 0, 0, 0};
			constexpr std::array<u8, 16> default_keys{0, 0, 0, 0, 2, 4, 3, 1, 0, 0, 0, 0, 2, 4, 3, 1};
			const auto& agreed_prefs = capture.practice ? agreed_practice_prefs : agreed_vs_prefs;
			capture.prefs_ok = capture.prefs_read && std::equal(agreed_prefs.begin(), agreed_prefs.end(), capture.prefs.begin());
			capture.keys_ok = capture.keys_read && capture.keys == default_keys;
			capture.stable_polls = m_scene_value == capture.scene.value_or(~u64{0}) ? m_poll - m_scene_since : 0;
			capture.fields_read = capture.prefs_read && capture.keys_read && capture.scene && capture.subscene && capture.mode && capture.source && capture.mode_req && capture.cursor1 && capture.cursor2 &&
			                      capture.hand1 && capture.hand2 && capture.hand3 && capture.hand4 && capture.human && capture.pad1 && capture.pad2 && capture.last1 && capture.last2;
			return capture;
		}

		std::vector<check> checkpoint_checks(const checkpoint_reading& capture, const pad_facts& pads) const
		{
			return capture.practice ? std::vector<check>{
										  {"fields_read", capture.fields_read}, {"history", m_history_ok},
										  {"practice_char_select", capture.fields_read && *capture.scene == 3 && *capture.subscene == 44 && *capture.mode == 10},
										  {"practice_mode_source", capture.fields_read && *capture.source == 1}, {"practice_options", capture.prefs_ok}, {"default_mappings", capture.keys_ok},
										  {"initial_cursors", capture.fields_read && *capture.cursor1 == 5 && *capture.cursor2 == 6},
										  {"pad_ownership", capture.fields_read && *capture.human == 1 && *capture.pad1 == 0 && *capture.pad2 == 1},
										  {"handicaps", capture.fields_read && *capture.hand1 == 0 && *capture.hand2 == 0}, {"no_pending_request", capture.fields_read && *capture.mode_req == 14},
										  {"scene_stable_120_polls_asset_proxy", capture.stable_polls >= 120}, {"pads", pads.virtual_pads && pads.port1_neutral},
										  // Practice has no peer options digest. Keep the saved fighter choices intact instead of imposing the VS defaults.
										  {"binding", !m_verify_path.empty() && !m_exe_sha.empty()}, {"saved_last_characters", capture.fields_read && capture.found1 && capture.found2 && capture.last1 == capture.found1 && capture.last2 == capture.found2}} :
			                          std::vector<check>{{"fields_read", capture.fields_read}, {"history", m_history_ok}, {"vs_char_select", capture.fields_read && *capture.scene == 3 && *capture.subscene == 44 && *capture.mode == 4}, {"first_to_3", capture.fields_read && capture.prefs[1] == 2 && *capture.source == 3}, {"agreed_options", capture.prefs_ok}, {"default_mappings", capture.keys_ok}, {"initial_cursors", capture.fields_read && *capture.cursor1 == 5 && *capture.cursor2 == 6},
										  // The select-screen words 0x10093fc0/0x10094038 read 0 at char-select entry: recorded, not required.
										  {"handicaps", capture.fields_read && *capture.hand1 == 100 && *capture.hand2 == 100}, {"no_pending_request", capture.fields_read && *capture.mode_req == 14}, {"scene_stable_120_polls_asset_proxy", capture.stable_polls >= 120}, {"pads", pads.virtual_pads && pads.port1_neutral}, {"binding", !m_verify_path.empty() && !m_exe_sha.empty()}, {"saved_last_characters", capture.fields_read && *capture.last1 == 5 && *capture.last2 == 6}};
		}

		void checkpoint(const pad_facts& pads)
		{
			const auto capture = read_checkpoint();
			const auto checks = checkpoint_checks(capture, pads);
			bool valid = true;
			std::string check_json = "{";
			for (usz i = 0; i < checks.size(); i++)
			{
				valid &= checks[i].ok;
				fmt::append(check_json, "%s\"%s\":%s", i ? "," : "", checks[i].name, checks[i].ok ? "true" : "false");
			}
			check_json += "}";
			std::string key_hex;
			for (u8 k : capture.keys)
				fmt::append(key_hex, "%02x", k);
			if (!m_verify_path.empty())
			{
				std::string observations = "[";
				for (usz i = 0; i < m_history.size(); i++)
					observations += (i ? "," : "") + m_history[i];
				observations += "]";
				const auto text = fmt::format(
					"{\"schema\":\"dr2026-first-run-verification-v2\",\"emitter\":\"dr2026-first-run-native\",\"input_source\":\"live-v1\",\"checkpoint_kind\":\"%s\","
					"\"run_id\":\"%s\",\"producer_executable_sha256\":\"%s\",\"checkpoint_compatibility_key\":\"%s\",\"capture_valid\":%s,\"checks\":%s,"
					"\"scene\":%u,\"subscene\":%u,\"effective_options\":{\"mode\":\"%s\",\"mode_word\":%u,\"rounds_preference_index\":%u,"
					"\"rounds_to_win\":%u},\"options_10684964_1068496e\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"keys_10684971_10684980\":\"%s\","
					"\"initial_cursors\":[%u,%u],\"handicaps\":[%u,%u,%u,%u],\"human_mask\":%u,\"saved_last_characters\":[%u,%u],\"saved_last_characters_found\":[%u,%u],\"mode_request\":%u,\"scene_stable_polls\":%u,"
					"\"pads\":{\"virtual_ports\":[0,1],\"connected\":%s,\"neutral\":%s,\"neutral_basis\":[\"live_channel_neutral_port0\",\"connected_ldd_port1\"]},"
					"\"commands_completed\":%u,\"observations\":%s,\"checkpoint_poll\":%u}\n",
					m_checkpoint_kind, m_run_id, m_exe_sha, pbd::checkpoint_compatibility::key, valid ? "true" : "false", check_json,
					capture.scene.value_or(~0u), capture.subscene.value_or(~0u), capture.mode && *capture.mode == 4 ? "vs_battle" : capture.mode && *capture.mode == 10 ? "practice" :
																																										  "unknown",
					capture.mode.value_or(~0u),
					capture.prefs[1], capture.source.value_or(0xff), capture.prefs[0], capture.prefs[1], capture.prefs[2], capture.prefs[3], capture.prefs[4], capture.prefs[5], capture.prefs[6], capture.prefs[7], capture.prefs[8], capture.prefs[9], capture.prefs[10],
					key_hex, capture.cursor1.value_or(~0u), capture.cursor2.value_or(~0u), capture.hand1.value_or(~0u), capture.hand2.value_or(~0u),
					capture.hand3.value_or(~0u), capture.hand4.value_or(~0u), capture.human.value_or(~0u), capture.last1.value_or(~0u), capture.last2.value_or(~0u), capture.found1.value_or(~0u), capture.found2.value_or(~0u),
					capture.mode_req.value_or(~0u), capture.stable_polls,
					pads.virtual_pads ? "true" : "false", pads.port1_neutral ? "true" : "false", static_cast<u32>(m_history.size()), observations, m_poll);
				fs::file out(utf8(m_verify_path), fs::rewrite);
				if (out)
					out.write(text.data(), text.size());
			}
			complete(valid ? "checkpoint_requested" : "checkpoint_refused");
			finish(valid);
		}

		void finish(bool save)
		{
			m_finished = true;
			Emu.CallFromMainThread([save]()
				{
					Emu.Kill(!save, save);
				});
		}
	};
} // namespace pbd
