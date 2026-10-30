#include "pbd_test.h"

#include "Emu/PBD/pbd_owner_flags.h"

#include <array>

namespace
{
	constexpr std::array deferred{
		cpu_flag::suspend, cpu_flag::pause, cpu_flag::memory, cpu_flag::signal,
		cpu_flag::notify, cpu_flag::yield, cpu_flag::preempt};
	constexpr auto empty = cpu_flag::stop - cpu_flag::stop;
	static_assert(pbd::pbd_owner_flags_allowed(empty, false, false));
	static_assert(pbd::pbd_owner_flags_allowed(cpu_flag::pause, true, false));
	static_assert(pbd::pbd_owner_flags_allowed(cpu_flag::suspend, true, true));
	static_assert(!pbd::pbd_owner_flags_allowed(cpu_flag::suspend, true, false));
	static_assert(!pbd::pbd_owner_flags_allowed(cpu_flag::notify, false, true));
	static_assert(!pbd::pbd_owner_flags_allowed(cpu_flag::wait + cpu_flag::notify, true, true));
} // namespace

PBD_TEST(PbdOwnerFlags, StrictOutsideAllowedScope)
{
	for (const bool preemption : {false, true})
	{
		pbd::test::check(pbd::pbd_owner_flags_allowed(empty, false, preemption), "zero flags need no admission");
		for (u32 bit = 0; bit < 32; ++bit)
		{
			pbd::test::check(!pbd::pbd_owner_flags_allowed(static_cast<cpu_flag>(bit), false, preemption),
				"every nonzero flag, including unknown bits, refuses outside admission");
		}
	}
}

PBD_TEST(PbdOwnerFlags, DeferredCombinations)
{
	for (u32 subset = 0; subset < (1u << deferred.size()); ++subset)
	{
		auto flags = empty;
		for (u32 bit = 0; bit < deferred.size(); ++bit)
			if (subset & (1u << bit))
				flags += deferred[bit];
		for (const bool preemption : {false, true})
		{
			const bool expected = !(flags & cpu_flag::suspend) || preemption;
			pbd::test::check(pbd::pbd_owner_flags_allowed(flags, true, preemption) == expected,
				"each deferred combination passes only with acknowledged scheduler suspension");
		}
	}
}

PBD_TEST(PbdOwnerFlags, RefusedBitsDominate)
{
	auto allowed = empty;
	for (const auto flag : deferred)
		allowed += flag;
	for (u32 bit = 0; bit < 32; ++bit)
	{
		const auto flag = static_cast<cpu_flag>(bit);
		if (allowed & flag)
			continue;
		for (const bool preemption : {false, true})
		{
			pbd::test::check(!pbd::pbd_owner_flags_allowed(flag, true, preemption),
				"unlisted flags and unknown bits refuse even inside admission");
			pbd::test::check(!pbd::pbd_owner_flags_allowed(allowed + flag, true, preemption),
				"scheduler flags cannot hide an unlisted flag");
		}
	}
}

// Scheduler work may arrive before servicing, between servicing and admission, or inside the owner guard.
// Service it on entry and defer it after servicing so host timing cannot refuse the same boundary on one peer.
PBD_TEST(PbdOwnerFlags, HostTimedWorkNeverRefusesARequest)
{
	for (u32 subset = 0; subset < (1u << deferred.size()); ++subset)
	{
		auto flags = empty;
		for (u32 bit = 0; bit < deferred.size(); ++bit)
			if (subset & (1u << bit))
				flags += deferred[bit];
		for (const bool preemption : {false, true})
		{
			// A suspension LV2 did not raise for preemption is not scheduler work: refused wherever it is seen.
			if (flags & cpu_flag::suspend && !preemption)
			{
				pbd::test::check(pbd::pbd_owner_service_entry(flags, preemption) == pbd::pbd_owner_entry::refuse_suspension);
				pbd::test::check(!pbd::pbd_owner_flags_allowed(flags, true, preemption));
				continue;
			}
			const auto entry = pbd::pbd_owner_service_entry(flags, preemption);
			pbd::test::check(entry == (flags ? pbd::pbd_owner_entry::service : pbd::pbd_owner_entry::ready),
				"host-timed work on entry is serviced, never refused (pause from suspend_all included)");
			pbd::test::check(pbd::pbd_owner_flags_allowed(flags, true, preemption),
				"the same work raised after servicing is accepted as the owner_guard opened next defers it");
		}
	}
	// The thread's own wait flag is serviced on entry too.
	pbd::test::check(pbd::pbd_owner_service_entry(cpu_flag::wait + cpu_flag::suspend, true) == pbd::pbd_owner_entry::service);
}

PBD_TEST(PbdOwnerFlags, StopDebugAndDiagnosticRequestsRefuseOnEntry)
{
	auto serviced = cpu_flag::wait + cpu_flag::pause + cpu_flag::suspend;
	for (const auto flag : deferred)
		serviced += flag;
	for (u32 bit = 0; bit < 32; ++bit)
	{
		const auto flag = static_cast<cpu_flag>(bit);
		if (serviced & flag)
			continue;
		for (const bool preemption : {false, true})
		{
			pbd::test::check(pbd::pbd_owner_service_entry(flag, preemption) == pbd::pbd_owner_entry::refuse_flags,
				"stop, exit, debugger, temp, pending and unknown bits are never serviced for a request");
			pbd::test::check(pbd::pbd_owner_service_entry(serviced + flag, preemption) == pbd::pbd_owner_entry::refuse_flags,
				"scheduler work cannot hide a stop or debugger request");
		}
	}
}
