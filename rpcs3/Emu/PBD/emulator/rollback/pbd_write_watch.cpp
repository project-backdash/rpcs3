#include "stdafx.h"

#ifdef _WIN32
#include <windows.h>
#endif

// The owner timeline's host cycle reader (pbd_owner_timeline.h). It lives in its own unit because pbd_fn_hook.cpp
// must not include <windows.h>, which changes that file's behaviour.
namespace pbd::owner_timeline
{
	u64 thread_cycles()
	{
#ifdef _WIN32
		ULONG64 cycles = 0;
		QueryThreadCycleTime(GetCurrentThread(), &cycles);
		return cycles;
#else
		return 0;
#endif
	}
} // namespace pbd::owner_timeline
