// configure()'s steps for the checkpoint, probe, replay, Practice and correction switches, and the checks of the
// start state.
#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_env_parse.h"
#include "pbd_emulator_identity.h"
#include "pbd_replay_standby.h"
#include "pbd_replay_capture.h"
#include "pbd_spectator_standby.h"

#ifdef _WIN32
void pbd::rollback_session::read_checkpoint_pages(const wchar_t* checkpoint_pages, bool public_capture)
{
	if (public_capture)
	{
		const auto* trace = pbd::env::find_wide("PBD_TICK_TRACE");
		require(trace && *trace && std::filesystem::path(trace).is_absolute(), "replay_capture_requires_trace_directory");
		m_storage->checkpoint.checkpoint_pages = pbd::replay_capture::pbd_pages();
		m_storage->checkpoint.capture_directory = std::filesystem::path(trace).parent_path();
		m_storage->fault.capture = true;
		if (!m_storage->launch.direct())
			m_storage->replay.input_port = 2;
		m_storage->undo.deny.assign(pbd::replay_capture::pbd_undo_deny.begin(), pbd::replay_capture::pbd_undo_deny.end());
		m_storage->undo.admit_io_spans.assign(pbd::replay_capture::pbd_io_allowed.begin(), pbd::replay_capture::pbd_io_allowed.end());
		// A player run uses the compiled byte policy. Diagnostic overrides require the explicit probe route.
		for (const char* key : {"PBD_TICK_PROBE_OUTPUT", "PBD_TICK_EXCLUSIONS", "PBD_TICK_UNDO_ADMIT",
				 "PBD_TICK_UNDO_ADMIT_IO_SPANS", "PBD_TICK_UNDO_CAPTURE", "PBD_TICK_UNDO_DENY"})
		{
			const auto* value = pbd::env::find(key);
			require(!(value && *value), "replay_capture_policy_conflict");
		}
	}
	else
	{
		const auto capture_output = pbd::env::find_wide("PBD_TICK_PROBE_OUTPUT");
		require(capture_output && *capture_output && std::filesystem::path(checkpoint_pages).is_absolute() &&
					std::filesystem::path(capture_output).is_absolute(),
			"tick_probe_paths_invalid");
		const auto pages_utf8 = std::filesystem::path(checkpoint_pages).u8string();
		fs::file pages_file(std::string(pages_utf8.begin(), pages_utf8.end()), fs::read + fs::isfile);
		std::array<char, 8> magic{};
		u32 count = 0;
		require(!!pages_file && pages_file.read(magic.data(), 8) == 8 && !std::memcmp(magic.data(), "DRTWS001", 8) &&
					pages_file.read(&count, 4) == 4 && count && count <= 4096 && pages_file.size() == 12 + u64{count} * 4,
			"tick_probe_pages_invalid");
		m_storage->checkpoint.checkpoint_pages.resize(count);
		require(pages_file.read(m_storage->checkpoint.checkpoint_pages.data(), count * 4ull) == count * 4ull, "tick_probe_pages_short");
		for (u32 i = 0; i < count; ++i)
			require(!(m_storage->checkpoint.checkpoint_pages[i] & 4095) && (!i || m_storage->checkpoint.checkpoint_pages[i - 1] < m_storage->checkpoint.checkpoint_pages[i]), "tick_probe_pages_unsorted");
		m_storage->checkpoint.capture_directory = std::filesystem::path(capture_output);
	}
}

void pbd::rollback_session::configure_capture_runs()
{
	const auto count = m_storage->checkpoint.checkpoint_pages.size();
	m_storage->checkpoint.capture_tick_state = true;
	for (u32 i = 0; i < count; ++i)
	{
		if (!m_storage->checkpoint.checkpoint_runs.empty())
		{
			auto& last = m_storage->checkpoint.checkpoint_runs.back();
			if (m_storage->checkpoint.checkpoint_pages[i] == m_storage->checkpoint.checkpoint_pages[last.first] + 4096u * last.second)
			{
				last.second++;
				continue;
			}
		}
		m_storage->checkpoint.checkpoint_runs.emplace_back(i, 1u);
	}
	if (const auto helpers = pbd::env::find_wide("PBD_TICK_CAPTURE_THREADS"); helpers && *helpers)
	{
		const u32 n = std::min<u32>(static_cast<u32>(std::wcstoul(helpers, nullptr, 10)), 3u);
		if (n && !m_storage->checkpoint.checkpoint_pages.empty())
		{
			std::vector<pbd_capture_pool::job> jobs;
			if (!m_storage->checkpoint.checkpoint_runs.empty())
				for (const auto& run : m_storage->checkpoint.checkpoint_runs)
					jobs.push_back({m_storage->checkpoint.checkpoint_pages[run.first], u64{run.first} * 4096, run.second * 4096u});
			else
				for (usz i = 0; i < m_storage->checkpoint.checkpoint_pages.size(); i++)
					jobs.push_back({m_storage->checkpoint.checkpoint_pages[i], i * 4096ull, 4096u});
			m_storage->checkpoint.capture_pool = std::make_unique<pbd_capture_pool>();
			m_storage->checkpoint.capture_pool->start(jobs, n);
		}
	}
}

void pbd::rollback_session::configure_checkpoint_exclusions(bool public_capture)
{
	m_storage->checkpoint.memory.reset(m_storage->checkpoint.checkpoint_pages);
	for (const auto& [begin, end] : roundtrip_exclusions)
		m_storage->checkpoint.memory.exclude(begin, end, m_storage->checkpoint.checkpoint_pages);
	if (public_capture)
		for (const auto& [begin, end] : pbd::replay_capture::pbd_exclusions)
			m_storage->checkpoint.memory.exclude(begin, end, m_storage->checkpoint.checkpoint_pages);
	if (const auto extra = pbd::env::find_wide("PBD_TICK_EXCLUSIONS"); extra && *extra)
	{
		// Additional never-restored spans, one "hex-hex" (end exclusive) per line; '#' starts a comment.
		require(std::filesystem::path(extra).is_absolute(), "tick_exclusions_path_invalid");
		const auto extra_utf8 = std::filesystem::path(extra).u8string();
		fs::file list(std::string(extra_utf8.begin(), extra_utf8.end()), fs::read + fs::isfile);
		require(!!list && list.size() <= 65536, "tick_exclusions_unreadable");
		std::string text(static_cast<usz>(list.size()), '\0');
		require(list.read(text.data(), text.size()) == text.size(), "tick_exclusions_short_read");
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			u64 begin = 0, end = 0;
			require(pbd::env::parse_memory_range(std::string_view(line), begin, end), "tick_exclusions_range");
			m_storage->checkpoint.memory.exclude(begin, end, m_storage->checkpoint.checkpoint_pages);
			m_storage->checkpoint.exclusion_file_ranges++;
		}
	}
}

void pbd::rollback_session::configure_undo_checks()
{
	if (const auto admit = pbd::env::find_wide("PBD_TICK_UNDO_ADMIT"); admit && *admit)
	{
		// Diagnostic: admit undo classes refused by default ("rsx_io", "library"; comma separated).
		const std::wstring text(admit);
		for (usz start = 0; start <= text.size();)
		{
			const usz comma = std::min(text.find(L',', start), text.size());
			const std::wstring item = text.substr(start, comma - start);
			if (item == L"rsx_io")
				m_storage->undo.admit_rsx_io = true;
			else if (item == L"library")
				m_storage->undo.admit_library = true;
			else if (item == L"foreign")
				m_storage->fault.capture_foreign = true; // write-fault capture: also save pages first written by other threads
			else
				require(false, "tick_undo_admit_invalid");
			start = comma + 1;
		}
	}
	if (const auto spans = pbd::env::find_wide("PBD_TICK_UNDO_ADMIT_IO_SPANS"); spans && *spans)
	{
		const std::wstring text(spans);
		for (usz start = 0; start < text.size();)
		{
			const usz comma = std::min(text.find(L',', start), text.size());
			const std::wstring item = text.substr(start, comma - start);
			u64 lo = 0, hi = 0;
			require(pbd::env::parse_memory_range(std::wstring_view(item), lo, hi) && !(lo & 4095) && !(hi & 4095), "tick_undo_admit_io_spans_invalid");
			m_storage->undo.admit_io_spans.emplace_back(lo, hi);
			start = comma + 1;
		}
	}
	if (const auto capture = pbd::env::find_wide("PBD_TICK_UNDO_CAPTURE"); capture && *capture)
	{
		// "store" (default): per-store capture in the interpreter and in instrumented compiled objects. "fault": write-fault capture on
		// uninstrumented objects (see pbd_rollback_session.h).
		const std::wstring text(capture);
		if (text == L"fault")
			m_storage->fault.capture = true;
		else
			require(text == L"store", "tick_undo_capture_invalid");
	}
}

void pbd::rollback_session::configure_undo_denials()
{
	if (const auto deny = pbd::env::find_wide("PBD_TICK_UNDO_DENY"); deny && *deny)
	{
		// Diagnostic undo deny spans, same "hex-hex" format as PBD_TICK_EXCLUSIONS.
		require(std::filesystem::path(deny).is_absolute(), "tick_undo_deny_path_invalid");
		const auto deny_utf8 = std::filesystem::path(deny).u8string();
		fs::file list(std::string(deny_utf8.begin(), deny_utf8.end()), fs::read + fs::isfile);
		require(!!list && list.size() <= 65536, "tick_undo_deny_unreadable");
		std::string text(static_cast<usz>(list.size()), '\0');
		require(list.read(text.data(), text.size()) == text.size(), "tick_undo_deny_short_read");
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			u64 begin = 0, end = 0;
			require(pbd::env::parse_memory_range(std::string_view(line), begin, end), "tick_undo_deny_range");
			m_storage->undo.deny.emplace_back(begin, end);
		}
		require(!m_storage->undo.deny.empty(), "tick_undo_deny_empty");
	}
}

void pbd::rollback_session::configure_page_set()
{
	const auto* checkpoint_pages = pbd::env::find_wide("PBD_TICK_PROBE_PAGES");
	require(!m_storage->launch.direct() || !(checkpoint_pages && *checkpoint_pages), "session_policy_memory_conflict");
	const bool public_capture = !(checkpoint_pages && *checkpoint_pages) &&
	                            (m_storage->launch.direct() || pbd::replay_capture::pbd_requested([](const char* key)
																   {
																	   return pbd::env::find(key);
																   }));
	if ((checkpoint_pages && *checkpoint_pages) || public_capture)
	{
		read_checkpoint_pages(checkpoint_pages, public_capture);
		configure_capture_runs();
		configure_checkpoint_exclusions(public_capture);
		m_storage->checkpoint.memory.prepare(m_storage->checkpoint.checkpoint_pages);
		configure_undo_checks();
		configure_undo_denials();
	}
}

#endif
