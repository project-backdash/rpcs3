#pragma once

#include "../CPU/CPUThread.h"

namespace pbd
{
	// A serviced owner may finish one hook before acknowledging asynchronous scheduler work.
	// Suspension is deferred only when LV2 has requested scheduler preemption; all other bits refuse.
	constexpr bool pbd_owner_flags_allowed(bs_t<cpu_flag> flags, bool admitted, bool scheduler_preemption) noexcept
	{
		constexpr auto deferred = cpu_flag::suspend + cpu_flag::pause + cpu_flag::memory + cpu_flag::signal +
		                          cpu_flag::notify + cpu_flag::yield + cpu_flag::preempt;
		return !flags || (admitted && !(flags - deferred) && (!(flags & cpu_flag::suspend) || scheduler_preemption));
	}

	// Service host scheduling flags through check_state before opening owner_guard; their timing cannot refuse a shared request.
	// Stop, debugger and diagnostic flags refuse entry, as does a suspension not raised by LV2 preemption.
	enum class pbd_owner_entry
	{
		ready,
		service,
		refuse_flags,
		refuse_suspension
	};
	constexpr pbd_owner_entry pbd_owner_service_entry(bs_t<cpu_flag> flags, bool scheduler_preemption) noexcept
	{
		constexpr auto serviced = cpu_flag::wait + cpu_flag::pause + cpu_flag::suspend + cpu_flag::memory + cpu_flag::signal +
		                          cpu_flag::notify + cpu_flag::yield + cpu_flag::preempt;
		if (!flags)
			return pbd_owner_entry::ready;
		if (flags - serviced)
			return pbd_owner_entry::refuse_flags;
		if (flags & cpu_flag::suspend && !scheduler_preemption)
			return pbd_owner_entry::refuse_suspension;
		return pbd_owner_entry::service;
	}
} // namespace pbd
