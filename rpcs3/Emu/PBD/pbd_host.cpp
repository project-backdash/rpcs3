#include "pbd_host.h"
#include "pbd_env.h"

#include <atomic>
#include <chrono>
#include <tuple>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace pbd::host
{
	namespace
	{
#ifdef _WIN32
		// Wine's own exports from ntdll (include/wine/library.h in Wine's source).
		using version_function = const char*(__cdecl*)();
		using host_version_function = void(__cdecl*)(const char** system, const char** release);

		// The fallback when wine-staging hides Wine's exports: Wine's registry key and its configuration tool, which a
		// Windows PC does not have together. Read only when the export is missing.
		bool wine_install() noexcept
		{
			HKEY key{};
			if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Wine", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
				return false;
			RegCloseKey(key);
			wchar_t path[MAX_PATH + 16]{};
			const UINT length = GetSystemDirectoryW(path, MAX_PATH);
			if (!length || length >= MAX_PATH)
				return false;
			wcscat_s(path, L"\\winecfg.exe");
			return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
		}

		facts detect() noexcept
		{
			facts out;
			const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			const auto version = ntdll ? reinterpret_cast<version_function>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
			const auto force = pbd::env::find("PBD_HOST_FORCE");
			std::tie(out.host, out.seen) = classify({true, version != nullptr, !version && wine_install(), force ? force : ""});
			const auto text = [](const char* value)
			{
				return printable(value ? value : "");
			};
			if (version)
			{
				out.version = text(version());
				if (const auto build = reinterpret_cast<version_function>(GetProcAddress(ntdll, "wine_get_build_id")))
					out.build = text(build());
				if (const auto system_version = reinterpret_cast<host_version_function>(GetProcAddress(ntdll, "wine_get_host_version")))
				{
					const char* system = nullptr;
					const char* release = nullptr;
					system_version(&system, &release);
					out.system = text(system);
					out.release = text(release);
				}
			}
			return out;
		}

		using probe_clock = std::chrono::steady_clock;

		double per_call(probe_clock::duration total, std::uint64_t calls, double unit_ns) noexcept
		{
			return calls ? std::chrono::duration<double, std::nano>(total).count() / unit_ns / static_cast<double>(calls) : -1;
		}

		// The probe's page. Its handler answers only a write fault on that page and leaves every other exception to the next one.
		std::atomic<std::uintptr_t> g_probe_page{0};

		LONG CALLBACK probe_fault(PEXCEPTION_POINTERS info)
		{
			const auto& record = *info->ExceptionRecord;
			const auto page = g_probe_page.load(std::memory_order_acquire);
			if (!page || record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 || record.ExceptionInformation[0] != 1 ||
				(record.ExceptionInformation[1] & ~std::uintptr_t{4095}) != page)
				return EXCEPTION_CONTINUE_SEARCH;
			DWORD old = 0;
			return VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
		}

		void probe_page(probe_result& out, const probe_counts& counts) noexcept
		{
			const auto page = static_cast<volatile std::uint32_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			if (!page)
				return;
			void* const address = const_cast<std::uint32_t*>(page);
			DWORD old = 0;
			std::uint64_t calls = 0;
			auto begin = probe_clock::now();
			for (std::uint32_t i = 0; i < counts.protect; i++)
				calls += VirtualProtect(address, 4096, PAGE_READONLY, &old) + VirtualProtect(address, 4096, PAGE_READWRITE, &old);
			if (calls == 2ull * counts.protect)
				out.protect_us = per_call(probe_clock::now() - begin, calls, 1000);
			g_probe_page.store(reinterpret_cast<std::uintptr_t>(address), std::memory_order_release);
			if (const auto handler = AddVectoredExceptionHandler(1, probe_fault))
			{
				probe_clock::duration total{};
				std::uint32_t faults = 0;
				for (; faults < counts.faults; faults++)
				{
					if (!VirtualProtect(address, 4096, PAGE_READONLY, &old))
						break;
					begin = probe_clock::now();
					*page = faults;
					total += probe_clock::now() - begin;
				}
				RemoveVectoredExceptionHandler(handler);
				if (faults == counts.faults)
					out.write_fault_us = per_call(total, faults, 1000);
			}
			g_probe_page.store(0, std::memory_order_release);
			VirtualFree(address, 0, MEM_RELEASE);
		}
#endif
	} // namespace

	const facts& current() noexcept
	{
#ifdef _WIN32
		static const facts value = detect();
#else
		static const facts value{};
#endif
		return value;
	}

	probe_result probe(const probe_counts& counts) noexcept
	{
		probe_result out;
#ifdef _WIN32
		FILETIME created{}, exited{}, kernel{}, user{};
		std::uint64_t answered = 0;
		auto begin = probe_clock::now();
		for (std::uint32_t i = 0; i < counts.thread_times; i++)
			answered += GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != 0;
		if (answered == counts.thread_times)
			out.thread_times_us = per_call(probe_clock::now() - begin, answered, 1000);
		ULONG64 cycles = 0;
		out.cycle_time = QueryThreadCycleTime(GetCurrentThread(), &cycles) != 0;
		probe_page(out, counts);
#endif
		volatile std::int64_t latest = 0;
		const auto clock_begin = std::chrono::steady_clock::now();
		for (std::uint32_t i = 0; i < counts.clock; i++)
			latest = std::chrono::steady_clock::now().time_since_epoch().count();
		if (counts.clock)
			out.clock_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - clock_begin).count() / counts.clock;
		return out;
	}
} // namespace pbd::host
