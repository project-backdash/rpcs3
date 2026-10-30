// Undo capture for rollback: the store reports and write faults that save a page before the tick changes it, the
// timebase journal and the syscall timing of re-executed ticks.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"

std::atomic<bool> g_pbd_tick_undo_enabled{false};

// PBD: static|marked page bitmap mirrored for the compiled inline fast path (one bit per 4 KiB page of the
// 32-bit guest space). Written only by the owner PPU; other PPU threads may read it, which is harmless because the
// callee rejects them by thread id anyway.
alignas(64) u64 g_pbd_undo_skip_bits[16384]{};

bool pbd_llvm_undo_requested()
{
	// Read once: the answer must not change between modules of one run, or objects would disagree.
	static const bool requested = []
	{
#ifdef _WIN32
		const auto value = pbd::env::find_wide("PBD_LLVM_UNDO");
		return value && std::wstring_view(value) == L"1";
#else
		return false;
#endif
	}();
	return requested;
}

// PBD JIT COPY16 switch (see the translate loop). Read once; also names the object-cache variant (PPUThread.cpp).
bool pbd_jit_copy16_enabled()
{
	static const bool on = []
	{
		const char* v = pbd::env::find("PBD_JIT_COPY16");
		return v && v[0] == '1';
	}();
	return on;
}

u32 pbd_jit_inline_leaf_mode()
{
	// Off unless the launcher sets it, as PBD_JIT_SCALAR_OPT: runs without PBD_ switches compile as stock.
	const char* value = pbd::env::find("PBD_JIT_INLINE_LEAF");
	return value && std::string_view(value) == "1" ? 1u : value && std::string_view(value) == "2" ? 2u :
	                                                                                                0u;
}

u32 pbd_jit_scalar_mode()
{
	// Read at each compile: a warm reboot in the same process may use another configuration.
	// Keep this pass opt-in so stock compilation preserves the game's HUD in Practice.
	const char* value = pbd::env::find("PBD_JIT_SCALAR_OPT");
	return value && value[0] >= '1' && value[0] <= '3' && value[1] == '\0' ? static_cast<u32>(value[0] - '0') : 0u;
}

u64 pbd::rollback_session::gpu_elapsed_time(ppu_thread& ppu, u64 measured) const noexcept
{
	// The GCM timer thread measures host GPU time asynchronously. Feeding it into the game's second load-monitor
	// bank can prune effects on only one peer, changing their shared rand() stream. GPU work occupies no logical
	// time within a battle-clock frame, just as CPU work between the frame's timebase samples does.
	return ppu.id == pbd::t5dr::main_ppu_id && m_storage && m_storage->battle_clock.enabled && m_storage->battle_clock.version == 2 ? 0 : measured;
}

u64 pbd::rollback_session::tick_timebase(ppu_thread& ppu, u32 pc, u64 live) noexcept
{
	if (ppu.id == pbd::t5dr::main_ppu_id && m_storage && m_storage->battle_clock.enabled)
	{
		// Battle clock: host time and read order never advance guest time; see battle_clock_state::enabled.
		auto& s = *m_storage;
		if (!s.battle_clock.anchor_set)
		{
			if (!g_pbd_restored_timebase_valid)
			{
				s.battle_clock.early_read = true;
				return live;
			}
			s.battle_clock.anchor = g_pbd_restored_timebase;
			s.battle_clock.anchor_set = true;
		}
		s.battle_clock.reads++;
		if (s.battle_clock.tick < 0)
			return s.battle_clock.anchor;
		if (s.battle_clock.version == 2 && pc == 0x36c4b8 && !s.battle_clock.frame_started)
		{
			s.battle_clock.frame_started = true;
			s.battle_clock.frame_samples++;
		}
		const u64 steps = static_cast<u64>(s.battle_clock.tick) + 1 + (s.battle_clock.frame_started ? 1 : 0);
		return s.battle_clock.anchor + steps * s.battle_clock.freq / 60;
	}
	if (ppu.id != pbd::t5dr::main_ppu_id || !m_active.load() || !m_storage || !(m_storage->roundtrip.active || m_storage->correction.active))
		return live;
	auto& data = *m_storage;
	if (data.correction.active)
	{
		if (data.correction.clock.replaying)
		{
			const auto& journal = data.checkpoint.ring[data.correction.clock.replay_tick % data.checkpoint.ring.size()].clock;
			if (data.correction.clock.cursor < journal.size() && journal[data.correction.clock.cursor].first == pc)
				return journal[data.correction.clock.cursor++].second;
			data.correction.clock.error = true;
			return live;
		}
		if (data.correction.clock.recording)
		{
			auto& journal = data.checkpoint.ring[data.correction.clock.tick % data.checkpoint.ring.size()].clock;
			if (journal.size() < journal.capacity())
				journal.emplace_back(pc, live);
			else
				data.correction.clock.error = true;
		}
		return live;
	}
	if (data.roundtrip.phase == 1)
	{
		if (data.roundtrip.clock.size() < data.roundtrip.clock.capacity())
			data.roundtrip.clock.emplace_back(pc, live);
		else
			data.roundtrip.clock_error = true;
		return live;
	}
	if (data.roundtrip.phase == 2)
	{
		if (data.roundtrip.clock_replay < data.roundtrip.clock.size() && data.roundtrip.clock[data.roundtrip.clock_replay].first == pc)
			return data.roundtrip.clock[data.roundtrip.clock_replay++].second;
		data.roundtrip.clock_error = true;
	}
	return live;
}

#include "../PBD/pbd_undo_capture.h"

void pbd::rollback_session::tick_undo_store(ppu_thread& ppu, u64 address, u32 bytes) noexcept
{
	if (ppu.id != pbd::t5dr::main_ppu_id || !m_active.load(std::memory_order_relaxed) || !m_storage)
		return;
	undo_capture{*this, *m_storage}.store(ppu, address, bytes);
}

// ---------------------------------------------------------------------------------------------------------------------
// Write-fault capture. Design notes:
// - Protection is applied to the base mapping only. RPCS3 keeps an unprotected alias of guest memory at
//   vm::g_sudo_addr (= g_base_addr + 4 GiB) used by vm::try_access; every latch restore and every preimage read goes
//   through it, so the latch never faults on its own pages. Writers that use the alias (RSX DMA, some HLE) stay
//   uncaptured, as under per-store capture.
// - The fault handler path is: handle_access_violation -> pbd_tick_write_fault -> tick_write_fault, ahead of the RSX
//   handler and the guest page-fault path. Faults on pages the latch did not arm fall through untouched.
// - Ownership. Per-store capture only sees the owner PPU's stores. A fault does not know the storing thread's guest
//   pc, but it knows the thread: the owner's first write is captured; a foreign thread's first write is recorded as a
//   foreign page (the page is unprotected either way) and captured only under PBD_TICK_UNDO_ADMIT=foreign.
// - PAGE_GUARD is not used: it is one-shot and does not distinguish a first write from a first read.
std::atomic<bool> g_pbd_fault_capture_active{false};

std::atomic<bool> g_pbd_replay_timing{false};

// True from a restore until the update entry of the closing live tick, i.e. over the WHOLE of every re-executed
// frame. g_pbd_replay_timing drops when the last re-executed tick's input update completes, which alone would leave the
// rest of that frame (its draw tasks) unskipped: a depth-d correction would skip d - 1 frames and a depth-1 correction none.
std::atomic<bool> g_pbd_replay_frame{false};

std::atomic<s64> g_pbd_pad_getdata_ns[2]{};

std::atomic<s64> g_pbd_pad_poll_ns{};

void pbd::rollback_session::replay_syscall_time(u64 code, u64 ns) noexcept
{
	if (code >= 1024 || !m_storage)
		return;
	m_storage->roundtrip.replay_syscall_ns[code] += ns;
	m_storage->roundtrip.replay_syscall_count[code]++;
}

void pbd_replay_syscall_time(u64 code, u64 ns) noexcept
{
	if (!g_fxo->is_init<pbd::rollback_session>())
		return;
	g_fxo->get<pbd::rollback_session>().replay_syscall_time(code, ns);
}

alignas(64) u64 g_pbd_fault_bits[16384]{};

void pbd::rollback_session::fault_protect(std::vector<u32>& pages, bool arm) noexcept
{
	// pages: page indices (address >> 12), sorted and unique. Contiguous runs with one target protection take one call.
	auto& data = *m_storage;
	const auto begin = session_clock::now();
	const auto target = [&](u32 p) -> utils::protection
	{
		if (arm)
			return utils::protection::ro;
		const u32 address = p << 12;
		return vm::check_addr(address, vm::page_writable) ? utils::protection::rw : vm::check_addr(address, vm::page_readable) ? utils::protection::ro :
		                                                                                                                         utils::protection::no;
	};
	for (usz i = 0; i < pages.size();)
	{
		const auto prot = target(pages[i]);
		usz j = i + 1;
		while (j < pages.size() && pages[j] == pages[j - 1] + 1 && target(pages[j]) == prot)
			j++;
		utils::memory_protect(vm::g_base_addr + (u64{pages[i]} << 12), (j - i) * 4096, prot);
		data.fault.arm_calls++;
		for (usz k = i; k < j; k++)
		{
			const u32 p = pages[k];
			if (arm)
				g_pbd_fault_bits[p >> 6] |= u64{1} << (p & 63);
			else
				g_pbd_fault_bits[p >> 6] &= ~(u64{1} << (p & 63));
		}
		i = j;
	}
	data.fault.arm_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - begin).count();
}

void pbd::rollback_session::fault_arm_all() noexcept
{
	auto& data = *m_storage;
	pbd_fault_guard guard(data.fault.lock);
	const auto mark = [&](u64 page)
	{
		if (page < (u64{1} << 32))
			data.fault.never_arm_bits[page >> 18] |= u64{1} << ((page >> 12) & 63);
	};
	// RSX-facing pages are never armed: the label and DMA control pages, the 1 MiB regions holding the command stream
	// positions and (unless admitted) every IO-mapped main-memory page. Same policy as tick_undo_store, fixed at arm time
	// because the renderer, its IO map and the deny list are invariant for the run.
	if (const auto render = rsx::get_current_renderer())
	{
		mark(render->label_addr & ~u32{4095});
		mark(render->dma_address & ~u32{4095});
		if (render->ctrl)
			for (const u32 offset : {render->ctrl->get.load(), render->ctrl->put.load()})
			{
				const u32 ea = render->iomap_table.get_addr(offset);
				if (ea != umax)
					for (u64 page = ea & ~u32{0xfffff}; page < (ea & ~u64{0xfffff}) + 0x100000; page += 4096)
						mark(page);
			}
		if (!data.undo.admit_rsx_io)
			for (u64 mb = 0; mb < 4096; mb++)
				if (render->iomap_table.io[mb] != umax)
					for (u64 page = mb << 20; page < (mb << 20) + 0x100000; page += 4096)
					{
						bool io_admitted = false;
						for (const auto& [lo, hi] : data.undo.admit_io_spans)
							io_admitted = io_admitted || (page >= lo && page < hi);
						if (!io_admitted)
							mark(page);
					}
	}
	for (const auto& [begin, end] : data.undo.deny)
		for (u64 page = begin & ~u64{4095}; page < end; page += 4096)
			mark(page);
	std::vector<u32> pages;
	pages.reserve(1u << 16);
	for (u64 address = 0x10000; address < 0xE0000000ull; address += 4096)
	{
		if (!pbd_fault_candidate_address(address))
			continue;
		const u32 p = static_cast<u32>(address >> 12);
		if ((data.fault.never_arm_bits[p >> 6] >> (p & 63)) & 1)
			continue;
		if (data.fault.foreign_stack(address))
			continue;
		if (!vm::check_addr(static_cast<u32>(address), vm::page_writable))
			continue;
		pages.push_back(p);
	}
	// Active before the first page is protected: a fault on an armed page must never reach the guest page-fault path.
	g_pbd_fault_capture_active.store(true, std::memory_order_release);
	fault_protect(pages, true);
	data.fault.armed_pages = pages.size();
	data.fault.armed_all = true;
	{
		std::string by_mb;
		for (usz i = 0; i < pages.size();)
		{
			const u32 mb = pages[i] >> 8;
			usz j = i;
			while (j < pages.size() && (pages[j] >> 8) == mb)
				j++;
			by_mb += fmt::format("%s[%u,%u]", by_mb.empty() ? "" : ",", mb << 20, j - i);
			i = j;
		}
		data.fault.log.push_back(fmt::format("{\"type\":\"fault_arm\",\"armed\":%u,\"by_mb\":[%s]}\n", pages.size(), by_mb));
	}
}

void pbd::rollback_session::fault_rearm() noexcept
{
	auto& data = *m_storage;
	pbd_fault_guard guard(data.fault.lock);
	auto& pages = data.fault.disarmed;
	std::sort(pages.begin(), pages.end());
	pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
	usz n = 0;
	for (const u32 p : pages)
		if (!((data.fault.never_arm_bits[p >> 6] >> (p & 63)) & 1) && !data.fault.foreign_stack(u64{p} << 12) && vm::check_addr(p << 12, vm::page_writable))
			pages[n++] = p;
	pages.resize(n);
	fault_protect(pages, true);
	pages.clear();
}

void pbd::rollback_session::fault_disarm_all() noexcept
{
	auto& data = *m_storage;
	pbd_fault_guard guard(data.fault.lock);
	std::vector<u32> pages;
	for (u32 word = 0; word < 16384; word++)
		for (u64 bits = g_pbd_fault_bits[word]; bits; bits &= bits - 1)
			pages.push_back(word * 64 + static_cast<u32>(std::countr_zero(bits)));
	fault_protect(pages, false);
	data.fault.disarmed.clear();
	data.fault.armed_all = false;
	g_pbd_fault_capture_active.store(false, std::memory_order_release);
}

bool pbd::rollback_session::tick_write_fault(u32 p) noexcept
{
	if (!m_storage || !m_storage->fault.capture || (m_storage->checkpoint.ring.empty() && !m_storage->takeover.fault_armed))
		return false;
	return undo_capture{*this, *m_storage}.write_fault(p);
}

void pbd::rollback_session::tick_pages_remapped(u32 addr, u32 size, bool writable) noexcept
{
	if (!m_storage || !m_storage->fault.capture || !m_storage->fault.armed_all)
		return;
	auto& data = *m_storage;
	pbd_fault_guard guard(data.fault.lock);
	std::vector<u32> pages;
	for (u64 page = addr & ~u64{4095}; page < u64{addr} + size; page += 4096)
	{
		const u32 p = static_cast<u32>(page >> 12);
		g_pbd_fault_bits[p >> 6] &= ~(u64{1} << (p & 63)); // The mapping change replaced the host protection.
		if (writable && pbd_fault_candidate_address(page) && !((data.fault.never_arm_bits[p >> 6] >> (p & 63)) & 1) && !data.fault.foreign_stack(page))
			pages.push_back(p);
	}
	data.fault.remaps++;
	fault_protect(pages, true);
	data.fault.log.push_back(fmt::format("{\"type\":\"fault_remap\",\"addr\":%u,\"size\":%u,\"writable\":%s,\"armed\":%u}\n", addr, size, json_bool(writable), pages.size()));
}

bool pbd_tick_write_fault(u32 addr) noexcept
{
	if (!g_fxo->is_init<pbd::rollback_session>())
		return false;
	return g_fxo->get<pbd::rollback_session>().tick_write_fault(addr >> 12);
}

void pbd_tick_pages_remapped(u32 addr, u32 size, bool writable) noexcept
{
	if (!g_pbd_fault_capture_active.load(std::memory_order_relaxed) || !g_fxo->is_init<pbd::rollback_session>())
		return;
	g_fxo->get<pbd::rollback_session>().tick_pages_remapped(addr, size, writable);
}

void pbd_tick_undo_store(ppu_thread& ppu, u64 address, u32 bytes) noexcept
{
	if (ppu.id != pbd::t5dr::main_ppu_id || !g_fxo->is_init<pbd::rollback_session>())
		return;
	g_fxo->get<pbd::rollback_session>().tick_undo_store(ppu, address, bytes);
}

u64 pbd_tick_timebase(ppu_thread& ppu, u32 pc, u64 live) noexcept
{
	if (ppu.id != pbd::t5dr::main_ppu_id || !g_fxo->is_init<pbd::rollback_session>())
		return live;
	return g_fxo->get<pbd::rollback_session>().tick_timebase(ppu, pc, live);
}
