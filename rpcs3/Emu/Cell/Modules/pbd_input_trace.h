#pragma once

#include "Emu/PBD/pbd_env.h"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"
#include "util/logs.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string_view>

LOG_CHANNEL(pbd_input_trace, "PBD input trace");

namespace pbd
{
	// An observation of an HLE pad poll, not a simulation frame or a savestate.
	struct input_observation
	{
		u32 port = 0;
		u32 data_address = 0;
		bool has_ppu = false;
		u32 ppu_id = 0;
		u32 ppu_pc = 0;
		u64 ppu_lr = 0;
		bool has_vblank = false;
		u64 vblank = 0;
		bool has_script_poll = false;
		u64 script_poll = 0;
		bool has_length = false;
		s32 length = 0;
		u32 button_count = 0;
		std::array<u16, 64> button_words{};
	};

	class input_trace_writer
	{
		static constexpr u64 max_bytes = 32 * 1024 * 1024;
		fs::file m_file;
		std::mutex m_mutex;
		std::atomic<bool> m_enabled{false};
		u64 m_sequence = 0;
		u64 m_bytes = 0;
		u64 m_event_limit = 100'000;
		const std::chrono::steady_clock::time_point m_origin = std::chrono::steady_clock::now();

		bool write_line(const std::string& line)
		{
			if (line.size() > max_bytes - m_bytes)
			{
				m_enabled.store(false, std::memory_order_relaxed);
				m_file.close();
				pbd_input_trace.notice("Input probe stopped at its 32 MiB byte limit.");
				return false;
			}
			if (m_file.write(line.data(), line.size()) != line.size())
			{
				m_enabled.store(false, std::memory_order_relaxed);
				m_file.close();
				pbd_input_trace.error("Input probe disabled after a trace write failure.");
				return false;
			}
			m_bytes += line.size();
			return true;
		}

		input_trace_writer() noexcept
		{
			try
			{
#ifdef _WIN32
				const wchar_t* requested_path = pbd::env::find_wide("PBD_INPUT_TRACE");
#else
				const char* requested_path = pbd::env::find("PBD_INPUT_TRACE");
#endif
				if (!requested_path || !*requested_path)
				{
					return;
				}
				const std::filesystem::path path(requested_path);
				if (!path.is_absolute())
				{
					pbd_input_trace.error("Input probe disabled: PBD_INPUT_TRACE must be an absolute path.");
					return;
				}
				if (const char* limit = pbd::env::find("PBD_INPUT_TRACE_MAX_EVENTS"))
				{
					const std::string_view text(limit);
					const auto parsed = std::from_chars(text.data(), text.data() + text.size(), m_event_limit);
					if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !m_event_limit || m_event_limit > 1'000'000)
					{
						pbd_input_trace.error("Input probe disabled: PBD_INPUT_TRACE_MAX_EVENTS must be 1 through 1000000.");
						return;
					}
				}
				const auto utf8 = path.u8string();
				if (!m_file.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile))
				{
					pbd_input_trace.error("Input probe disabled: could not create a new trace file. Existing files are never overwritten; the parent directory must exist.");
					return;
				}
				if (write_line(fmt::format("{\"type\":\"header\",\"schema\":1,\"source\":\"cellPadGetData\",\"clock\":\"host_steady_ns_since_probe_init\",\"max_events\":%u,\"max_bytes\":%u,\"flip_count_available\":false}\n", m_event_limit, max_bytes)))
				{
					m_enabled.store(true, std::memory_order_relaxed);
					pbd_input_trace.notice("Input probe enabled, capped at %u observations and 32 MiB.", m_event_limit);
				}
			}
			catch (...)
			{
				m_enabled.store(false, std::memory_order_relaxed);
				m_file.close();
				pbd_input_trace.error("Input probe disabled after a trace initialization failure.");
			}
		}

	public:
		static input_trace_writer& get()
		{
			static input_trace_writer instance;
			return instance;
		}

		bool enabled() const noexcept
		{
			return m_enabled.load(std::memory_order_relaxed);
		}

		// Call after releasing g_pad_mutex; trace disk I/O must not hold the pad lock.
		void append(const input_observation& value, s32 result) noexcept
		{
			try
			{
				std::lock_guard lock(m_mutex);
				if (!enabled())
				{
					return;
				}
				const auto host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_origin).count();
				const auto optional_number = [](bool present, u64 number)
				{
					return present ? fmt::format("%u", number) : std::string("null");
				};
				std::string buttons;
				for (u32 index = 0; index < value.button_count; ++index)
				{
					if (index)
						buttons += ',';
					buttons += fmt::format("%u", value.button_words[index]);
				}
				const bool has_digital = value.button_count >= 4;
				const std::string line = fmt::format(
					"{\"type\":\"pad_poll\",\"sequence\":%u,\"host_ns\":%u,\"ppu_thread\":%s,\"ppu_pc\":%s,\"ppu_lr\":%s,\"port\":%u,\"data_address\":%u,\"result\":%u,\"length\":%s,\"digital1\":%s,\"digital2\":%s,\"button_words\":[%s],\"vblank_count\":%s,\"flip_count\":null,\"diagnostic_tape_poll\":%s}\n",
					m_sequence, static_cast<u64>(host_ns), optional_number(value.has_ppu, value.ppu_id), optional_number(value.has_ppu, value.ppu_pc), optional_number(value.has_ppu, value.ppu_lr),
					value.port, value.data_address, static_cast<u32>(result), value.has_length ? fmt::format("%d", value.length) : std::string("null"),
					optional_number(has_digital, value.button_words[2]), optional_number(has_digital, value.button_words[3]), buttons, optional_number(value.has_vblank, value.vblank), optional_number(value.has_script_poll, value.script_poll));
				if (!write_line(line))
				{
					return;
				}
				if (++m_sequence >= m_event_limit)
				{
					m_enabled.store(false, std::memory_order_relaxed);
					m_file.close();
					pbd_input_trace.notice("Input probe stopped at its %u observation limit.", m_event_limit);
				}
			}
			catch (...)
			{
				// A probe failure must not become an emulated game error.
				m_enabled.store(false, std::memory_order_relaxed);
				pbd_input_trace.error("Input probe disabled after a trace serialization or I/O failure.");
			}
		}
	};
} // namespace pbd
