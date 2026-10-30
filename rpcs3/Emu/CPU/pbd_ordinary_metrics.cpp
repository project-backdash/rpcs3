#include "stdafx.h"
#include "pbd_ordinary_metrics.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_t5dr_memory.h"
#include "Emu/RSX/pbd_frame_presentation.h"
#include "Emu/RSX/RSXThread.h"
#include "CPUThread.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Cell/pbd_spu_checkpoint.h"
#include "Emu/IdManager.h"
#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_locking.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include <cstring>
#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#endif

namespace pbd::stall_metrics
{
	namespace
	{
		queue<> records;
		std::atomic<std::uint32_t> current_boundary{0};
		std::uint64_t now_ns() noexcept
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		struct cost
		{
			std::atomic<u64> calls{0}, elapsed_ns{0}, maximum_ns{0};
			void add(u64 elapsed) noexcept
			{
				calls.fetch_add(1, std::memory_order_relaxed);
				elapsed_ns.fetch_add(elapsed, std::memory_order_relaxed);
				auto previous = maximum_ns.load(std::memory_order_relaxed);
				while (previous < elapsed && !maximum_ns.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed))
				{
				}
			}
			void append(std::string& text, const char* name, u64 at)
			{
				fmt::append(text, "%u,%u,0,0,%s_cost,%u,%u,0,0,0,0,0,0,0\n", at, at, name,
					elapsed_ns.exchange(0, std::memory_order_relaxed), calls.exchange(0, std::memory_order_relaxed));
				fmt::append(text, "%u,%u,0,0,%s_max,%u,0,0,0,0,0,0,0,0\n", at, at, name,
					maximum_ns.exchange(0, std::memory_order_relaxed));
			}
		} producer_cost, writer_cost;
		cpu_sample sample() noexcept
		{
			cpu_sample out;
#ifdef _WIN32
			FILETIME created{}, exited{}, kernel{}, user{};
			ULONG64 cycles = 0;
			out.valid = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) &&
			            QueryThreadCycleTime(GetCurrentThread(), &cycles);
			out.kernel = (std::uint64_t{kernel.dwHighDateTime} << 32) | kernel.dwLowDateTime;
			out.user = (std::uint64_t{user.dwHighDateTime} << 32) | user.dwLowDateTime;
			out.cycles = cycles;
#endif
			return out;
		}
	} // namespace
	bool requested() noexcept
	{
		static generation_flag enabled;
		return enabled.get(static_cast<u64>(Emu.GetEmulationIdentifier()), []
			{
				records.restart();
				const auto* path = pbd::env::find("PBD_ORDINARY_OUTPUT");
				const auto* session = pbd::env::find("PBD_SESSION");
				return path && *path && session && std::string_view(session) == "1";
			});
	}
	void boundary(std::uint32_t index) noexcept
	{
		current_boundary.store(index, std::memory_order_relaxed);
	}
	void point(const char* site, std::uint32_t represented_boundary, std::uint64_t bytes, std::uint64_t detail) noexcept
	{
		if (!requested())
			return;
		record value;
		value.site = site;
		value.begin_ns = value.end_ns = now_ns();
		value.boundary = represented_boundary;
		value.bytes = bytes;
		value.detail = detail;
#ifdef _WIN32
		value.thread = GetCurrentThreadId();
#endif
		records.push(value);
		producer_cost.add(now_ns() - value.begin_ns);
	}
	scope::scope(const char* site, std::uint64_t bytes, std::uint64_t detail, bool enabled, std::uint64_t minimum, sampling samples) noexcept
		: minimum_ns(minimum), sample_cpu(samples == sampling::cpu)
	{
		if (!enabled || !requested())
			return;
		const auto cost_begin = now_ns();
		value.site = site;
		value.bytes = bytes;
		value.detail = detail;
		value.boundary = current_boundary.load(std::memory_order_relaxed);
#ifdef _WIN32
		value.thread = GetCurrentThreadId();
#endif
		if (sample_cpu)
			value.before = sample();
		value.begin_ns = now_ns();
		construction_ns = value.begin_ns - cost_begin;
	}
	void scope::finish() noexcept
	{
		if (!value.site)
			return;
		value.end_ns = now_ns();
		if (value.end_ns - value.begin_ns >= minimum_ns)
		{
			if (sample_cpu)
				value.after = sample();
			records.push(value);
		}
		producer_cost.add(construction_ns + now_ns() - value.end_ns);
		value.site = nullptr;
	}
	scope::~scope()
	{
		finish();
	}
	void drain(bool ending) noexcept
	{
		if (!requested())
			return;
		const auto drain_begin = now_ns();
		// Only the existing session writer opens or writes this file.
		static std::unique_ptr<fs::file> output;
		static generation_flag opened;
		static u64 next_cost_ns = 0;
		try
		{
			opened.get(static_cast<u64>(Emu.GetEmulationIdentifier()), [&]
				{
					output.reset();
#ifdef _WIN32
					const std::filesystem::path directory(pbd::env::find_wide("PBD_ORDINARY_OUTPUT"));
#else
					const std::filesystem::path directory(pbd::env::find("PBD_ORDINARY_OUTPUT"));
#endif
					const auto path = (directory / "stall-metrics.csv").u8string();
					output = std::make_unique<fs::file>(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
					if (*output)
					{
						const std::string header = "begin_ns,end_ns,thread,boundary,site,bytes,detail,kernel_begin_100ns,kernel_end_100ns,user_begin_100ns,user_end_100ns,cycles_begin,cycles_end,cpu_valid\n";
						if (output->write(header.data(), header.size()) != header.size())
							output.reset();
					}
					else
						output.reset();
					return static_cast<bool>(output);
				});
			std::string text;
#ifdef _WIN32
			static std::uint64_t next_resources_ns = 0;
			const auto at = now_ns();
			if (output && at >= next_resources_ns)
			{
				next_resources_ns = at + 5'000'000'000ull;
				// Bracket the wall-clock observation: consumers can map steady events to launcher UTC and bound sampling error.
				const auto utc_begin = now_ns();
				const auto utc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
				const auto utc_end = now_ns();
				fmt::append(text, "%u,%u,0,0,clock_anchor,%u,0,0,0,0,0,0,0,0\n", utc_begin, utc_end, static_cast<u64>(utc_ns));
				PROCESS_MEMORY_COUNTERS_EX memory{};
				memory.cb = sizeof(memory);
				if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
				{
					fmt::append(text, "%u,%u,0,0,process_memory,%u,%u,0,0,0,0,0,0,0\n", at, at, memory.PrivateUsage, memory.WorkingSetSize);
					fmt::append(text, "%u,%u,0,0,process_page_faults,%u,%u,0,0,0,0,0,0,0\n", at, at, memory.PageFaultCount, memory.PeakWorkingSetSize);
				}
				DWORD handles = 0;
				if (GetProcessHandleCount(GetCurrentProcess(), &handles))
					fmt::append(text, "%u,%u,0,0,process_handles,%u,0,0,0,0,0,0,0,0\n", at, at, handles);
				const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
				if (snapshot != INVALID_HANDLE_VALUE)
				{
					THREADENTRY32 thread{};
					thread.dwSize = sizeof(thread);
					unsigned count = 0;
					if (Thread32First(snapshot, &thread))
						do
						{
							count += thread.th32OwnerProcessID == GetCurrentProcessId();
						} while (Thread32Next(snapshot, &thread));
					CloseHandle(snapshot);
					fmt::append(text, "%u,%u,0,0,process_threads,%u,0,0,0,0,0,0,0,0\n", at, at, count);
				}
			}
#endif
			// A terminal drain covers the whole ring, including a backlog larger than a normal writer batch.
			if (ending)
				records.close();
			records.consume([&](const record& r)
				{
					if (!output)
						return;
					fmt::append(text, "%u,%u,%u,%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", r.begin_ns, r.end_ns, r.thread, r.boundary,
						r.site, r.bytes, r.detail, r.before.kernel, r.after.kernel, r.before.user, r.after.user,
						r.before.cycles, r.after.cycles, r.before.valid && r.after.valid ? 1u : 0u);
				},
				ending ? records.capacity : 8192);
			const auto cost_at = now_ns();
			if (ending || cost_at >= next_cost_ns)
			{
				next_cost_ns = cost_at + 5'000'000'000ull;
				producer_cost.append(text, "trace_producer", cost_at);
				writer_cost.append(text, "trace_writer", cost_at);
			}
			if (ending)
				fmt::append(text, "0,0,0,0,trace_end,0,%u,0,0,0,0,0,0,0\n", records.dropped());
			if (output && output->write(text.data(), text.size()) != text.size())
				output.reset();
			if (ending)
				output.reset();
		}
		catch (...)
		{
			output.reset();
		}
		writer_cost.add(now_ns() - drain_begin);
	}
} // namespace pbd::stall_metrics

// Ordinary execution measurement. No replay, instruction interception, state
// mutation, or GPU readback. Per-thread buffers are exported after the window.
namespace pbd::ordinary_metrics
{
#ifdef _WIN32
	namespace
	{
		using timer = std::chrono::steady_clock;
		u64 now() noexcept
		{
			LARGE_INTEGER q{};
			QueryPerformanceCounter(&q);
			return q.QuadPart;
		}
		u64 filetime(const FILETIME& x) noexcept
		{
			return (u64{x.dwHighDateTime} << 32) | x.dwLowDateTime;
		}
		struct frame
		{
			u64 qpc, kernel, user, cycles;
			u32 counter, pc, p1_health, p2_health, inputs_valid;
			std::array<u32, 4> inputs;
		};
		struct producer_row
		{
			u64 qpc, target, bytes, detail;
			u32 counter, host_tid, source_cpu;
			const char* site;
		};
		struct span
		{
			u64 begin, end;
			u32 kind, counter;
			const char* name;
			u64 begin_cycles, end_cycles;
		};
		struct measurement
		{
			bool enabled = false, scopes = false, cpu_scopes = false, producers = false;
			std::mutex producer_mutex;
			std::vector<producer_row> producer_rows;
			u64 producer_dropped = 0;
			std::atomic<bool> cycles_valid{true};
			std::atomic<bool> main_done{false}, rsx_done{false};
			std::atomic<u32> counter{0};
			std::atomic<u32> main_tid{0};
			u32 last_counter = 0, target = 721, first_counter = 0;
			u64 frequency = 0, frame_measurement_ticks = 0, hle_measurement_ticks = 0;
			std::filesystem::path directory;
			std::vector<frame> frames;
			std::vector<span> main_spans, rsx_spans;
			measurement() noexcept
			{
				// Continuous sessions use the background trace; startup-buffer export must not block a fighting PPU.
				if (pbd::stall_metrics::requested())
					return;
				const auto path = pbd::env::find_wide("PBD_ORDINARY_OUTPUT");
				if (!path || !*path)
					return;
				try
				{
					directory = std::filesystem::path(path);
					if (!directory.is_absolute() || !std::filesystem::is_directory(directory))
						return;
					const auto mode = pbd::env::find_wide("PBD_ORDINARY_SCOPES");
					scopes = mode && std::wstring_view(mode) == L"1";
					const auto cpu_mode = pbd::env::find_wide("PBD_ORDINARY_CPU_SCOPES");
					cpu_scopes = scopes && cpu_mode && std::wstring_view(cpu_mode) == L"1";
					const auto producer_mode = pbd::env::find_wide("PBD_ORDINARY_PRODUCERS");
					producers = producer_mode && std::wstring_view(producer_mode) == L"1";
					if (producers)
						producer_rows.reserve(32768);
					if (const auto first = pbd::env::find_wide("PBD_ORDINARY_FIRST_COUNTER"); first && *first)
					{
						// A generation loaded from an input-entry capture at counter N first
						// observes N+1 and still closes at1968. Any other value disables.
						u32 value = 0;
						for (auto digit = first; *digit; ++digit)
						{
							if (*digit < L'0' || *digit > L'9' || value > 100000)
								return;
							value = value * 10 + (*digit - L'0');
						}
						if (value < 1249 || value > 1968)
							return;
						first_counter = value;
						target = 1969 - value;
					}
					frames.reserve(target);
					main_spans.reserve(1000000);
					rsx_spans.reserve(3000);
					LARGE_INTEGER value{};
					QueryPerformanceFrequency(&value);
					frequency = value.QuadPart;
					enabled = true;
				}
				catch (...)
				{
					enabled = false;
				}
			}
			void write_spans(const char* filename, const std::vector<span>& rows)
			{
				const auto path = (directory / filename).u8string();
				fs::file out(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
				if (!out)
					return;
				std::string text = "begin_qpc,end_qpc,kind,counter,name,begin_cycles,end_cycles\n";
				for (const auto& row : rows)
					text += fmt::format("%u,%u,%u,%u,%s,%u,%u\n", row.begin, row.end, row.kind, row.counter, row.name, row.begin_cycles, row.end_cycles);
				out.write(text.data(), text.size());
			}
			void finish_main() noexcept
			{
				main_done.store(true, std::memory_order_release);
				const u64 export_start = now();
				try
				{
					const auto path = (directory / "ordinary-frames.csv").u8string();
					fs::file out(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
					std::string text = "qpc,kernel_100ns,user_100ns,cycles,counter,pc,p1_health,p2_health,inputs_valid,input0,input1,mapped0,mapped1\n";
					for (const auto& row : frames)
						text += fmt::format("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", row.qpc, row.kernel, row.user, row.cycles, row.counter, row.pc, row.p1_health, row.p2_health, row.inputs_valid, row.inputs[0], row.inputs[1], row.inputs[2], row.inputs[3]);
					if (out)
						out.write(text.data(), text.size());
					out.close();
					write_spans("ordinary-main-spans.csv", main_spans);
					if (producers)
					{
						std::lock_guard lock(producer_mutex);
						const auto path = (directory / "ordinary-producers.csv").u8string();
						fs::file file(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
						std::string rows = "qpc,counter,host_tid,source_cpu,site,target,bytes,detail\n";
						for (const auto& r : producer_rows)
							rows += fmt::format("%u,%u,%u,%u,%s,%u,%u,%u\n", r.qpc, r.counter, r.host_tid, r.source_cpu, r.site, r.target, r.bytes, r.detail);
						if (file)
							file.write(rows.data(), rows.size());
					}
					const auto meta = (directory / "ordinary-main.json").u8string();
					fs::file file(std::string(meta.begin(), meta.end()), fs::write_new + fs::isfile);
					text = fmt::format("{\"schema\":1,\"qpc_frequency\":%u,\"main_tid\":%u,\"frames\":%u,\"spans\":%u,\"scope_mode\":%s,\"frame_probe_ticks\":%u,\"hle_probe_ticks\":%u,\"export_ticks\":%u,\"decoder\":\"%s\",\"boundary\":\"first_main_HLE_after_input_counter_change\"}",
						frequency, main_tid.load(), frames.size(), main_spans.size(), scopes ? "true" : "false", frame_measurement_ticks, hle_measurement_ticks, now() - export_start, g_cfg.core.ppu_decoder.to_string());
					text.pop_back();
					text += fmt::format(",\"cpu_scope_mode\":%s,\"cpu_cycles_valid\":%s}", cpu_scopes ? "true" : "false", cycles_valid.load() ? "true" : "false");
					text.pop_back();
					text += fmt::format(",\"producer_mode\":%s,\"producer_dropped\":%u}", producers ? "true" : "false", producer_dropped);
					text.pop_back();
					text += ",\"checkpoint_fail_closed\":false}";
					if (file)
						file.write(text.data(), text.size());
				}
				catch (...)
				{
				}
			}
		};
		measurement& get() noexcept
		{
			static measurement value;
			return value;
		}
		u64 cycle_sample() noexcept
		{
			auto& m = get();
			if (!m.cpu_scopes)
				return 0;
			ULONG64 value = 0;
			if (!QueryThreadCycleTime(GetCurrentThread(), &value))
				m.cycles_valid.store(false);
			return value;
		}
		struct pending
		{
			u64 begin;
			const char* name;
			u64 cycles;
		};
		thread_local std::vector<pending> hle_stack;
		bool is_main() noexcept
		{
			auto* cpu = get_current_cpu_thread();
			return cpu && cpu->id == pbd::t5dr::main_ppu_id;
		}
		void append(u64 begin, u32 kind, const char* name, u64 cycles) noexcept
		{
			auto& m = get();
			if (!begin)
				return;
			const auto end_cycles = kind == 4 ? 0 : cycle_sample();
			const auto end = now();
			if (kind == 4)
			{
				if (m.rsx_done.load(std::memory_order_acquire))
					return;
				if (m.rsx_spans.size() < 3000)
					m.rsx_spans.push_back({begin, end, kind, m.counter.load(), name, cycles, end_cycles});
				if (m.main_done.load(std::memory_order_acquire))
				{
					try
					{
						m.write_spans("ordinary-presentation.csv", m.rsx_spans);
					}
					catch (...)
					{
					}
					m.rsx_done.store(true, std::memory_order_release);
				}
			}
			else if (!m.main_done.load(std::memory_order_acquire) && m.main_spans.size() < 1000000)
				m.main_spans.push_back({begin, end, kind, m.counter.load(), name, cycles, end_cycles});
		}
	} // namespace
	void producer(const char* site, u64 target, u64 bytes, u64 detail) noexcept
	{
		auto& m = get();
		if (!m.producers || !m.counter.load(std::memory_order_acquire) || m.main_done.load(std::memory_order_acquire))
			return;
		std::lock_guard lock(m.producer_mutex);
		if (m.main_done.load(std::memory_order_acquire))
			return;
		if (m.producer_rows.size() == 32768)
		{
			++m.producer_dropped;
			return;
		}
		const auto* cpu = get_current_cpu_thread();
		m.producer_rows.push_back({now(), target, bytes, detail, m.counter.load(), GetCurrentThreadId(), cpu ? cpu->id : 0, site});
	}
	void rewind_frames(u32 counter) noexcept
	{
		auto& m = get();
		if (!m.enabled || m.main_done.load())
			return;
		while (!m.frames.empty() && m.frames.back().counter > counter)
			m.frames.pop_back();
		m.last_counter = counter;
		m.counter.store(counter, std::memory_order_release);
	}
	bool requested() noexcept
	{
		return get().enabled;
	}
	bool capture_allowed() noexcept
	{
		auto& m = get();
		return !m.enabled || (m.main_done.load() && m.rsx_done.load());
	}
	void hle_begin(cpu_thread& cpu, const char* name) noexcept
	{
		if (cpu.id != pbd::t5dr::main_ppu_id)
			return;
		auto& m = get();
		if (!m.enabled || m.main_done.load(std::memory_order_relaxed))
			return;
		const u64 entered = now();
		if (Emu.GetTitleID() != pbd::t5dr::title_id || !vm::check_addr(0x104cce40, vm::page_readable, 4))
			return;
		const u32 counter = vm::read32(0x104cce40);
		if (counter < 1248 || counter > 1000000)
			return;
		// Fresh boot may visit menus before this allocation becomes battle state.
		// Admit only the reference fight selectors; this does not prove state equality.
		if (m.frames.empty() && (!vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 8) ||
									vm::read32(pbd::t5dr::scene_address) != 17 || vm::read32(pbd::t5dr::subscene_address) != 44))
			return;
		m.main_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
		if (counter != m.last_counter && hle_stack.empty())
		{
			const u64 began = now();
			FILETIME created{}, exited{}, kernel{}, user{};
			ULONG64 cycles = 0;
			if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) ||
				!QueryThreadCycleTime(GetCurrentThread(), &cycles))
				return;
			u32 p1_health = 0, p2_health = 0;
			if (m.producers && vm::check_addr(0x100906ec, vm::page_readable, 4) && vm::check_addr(0x10090fbc, vm::page_readable, 4))
			{
				p1_health = vm::read32(0x100906ec);
				p2_health = vm::read32(0x10090fbc);
			}
			// Observe the settled consumer outputs already checked by the update latch;
			// do not inject input or infer neutral input from an unreadable address.
			std::array<u32, 4> inputs{};
			u32 inputs_valid = 0;
			if (m.producers && vm::check_addr(0x10099fac, vm::page_readable, 16))
			{
				for (u32 i = 0; i < 4; ++i)
					inputs[i] = vm::read32(0x10099fac + 4 * i);
				inputs_valid = 1;
			}
			m.frames.push_back({began, filetime(kernel), filetime(user), cycles, counter, static_cast<ppu_thread&>(cpu).cia, p1_health, p2_health, inputs_valid, inputs});
			m.last_counter = counter;
			m.counter.store(counter, std::memory_order_release);
			pbd::frame_presentation::mark_input(counter);
			m.frame_measurement_ticks += now() - began;
			// A misaligned loaded generation never closes, so no image is published.
			if (m.first_counter && m.frames.size() == 1 && counter != m.first_counter)
				m.target = umax;
			if (m.frames.size() >= m.target)
			{
				m.finish_main();
				return;
			}
		}
		if (m.scopes)
			hle_stack.push_back({now(), name, cycle_sample()});
		m.hle_measurement_ticks += now() - entered;
	}
	void hle_end(cpu_thread& cpu) noexcept
	{
		if (cpu.id != pbd::t5dr::main_ppu_id || hle_stack.empty())
			return;
		const auto value = hle_stack.back();
		hle_stack.pop_back();
		append(value.begin, 1, value.name, value.cycles);
	}
	scope::scope(u32 value, const char* label, bool enabled, u64 continuous_minimum_ns) noexcept
		: continuous(label, 0, value, enabled, continuous_minimum_ns), name(label), kind(value)
	{
		auto& m = get();
		if (enabled && m.enabled && !m.main_done.load(std::memory_order_relaxed) && (value == 4 || (m.scopes && is_main())))
			started = now();
		// Let the first post-window flip export its own buffer, outside measurement.
		else if (enabled && m.enabled && value == 4 && !m.rsx_done.load())
			started = now();
		if (started && value != 4)
			cycles = cycle_sample();
	}
	scope::~scope()
	{
		append(started, kind, name, cycles);
	}
#else
	void producer(const char*, u64, u64, u64) noexcept {}
	void rewind_frames(u32) noexcept {}
	bool requested() noexcept
	{
		return false;
	}
	bool capture_allowed() noexcept
	{
		return true;
	}
	void hle_begin(cpu_thread&, const char*) noexcept {}
	void hle_end(cpu_thread&) noexcept {}
	scope::scope(u32 value, const char* label, bool enabled, u64 continuous_minimum_ns) noexcept
		: continuous(label, 0, value, enabled, continuous_minimum_ns), name(nullptr), kind(0) {}
	scope::~scope() = default;
#endif
} // namespace pbd::ordinary_metrics
