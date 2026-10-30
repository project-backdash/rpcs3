#pragma once

struct pbd::rollback_session::undo_capture
{
	rollback_session& session;
	storage& data;

	void store(ppu_thread& ppu, u64 address, u32 bytes) noexcept
	{
		if (!data.undo.ready || !bytes)
			return;
		if (data.fault.capture)
		{
			// Write-fault capture. Reservation stores (ppu_stwcx/ppu_stdcx) write through the sudo alias and interpreter stores
			// report before writing: an armed page in the reported span is captured here exactly as a fault would capture it
			// (owner thread, preimage through the alias, unprotect, re-arm at the next boundary).
			for (u64 page = address & ~u64{4095}; page < address + bytes; page += 4096)
			{
				const u32 p = static_cast<u32>(page >> 12);
				if ((g_pbd_fault_bits[p >> 6] >> (p & 63)) & 1)
					session.tick_write_fault(p);
			}
			return;
		}
		if (address + bytes > (u64{1} << 32))
			return; // The interpreter faults on this store itself.
		// RSX-facing pages. Always refused: the IO-mapped megabytes holding the RSX's live command stream positions (DMA
		// control get and put, mapped through the RSX IO map), the DMA control page and the label page. Undoing them rewinds
		// commands the RSX already consumed (dead FIFO). The cellGcmSys HLE config cannot locate the
		// command buffer after a savestate load (ioAddress 0, command_size default), so the RSX thread state is used.
		// Other RSX IO-mapped main memory (the game heap 0x40100000..0x45c00000) is refused unless PBD_TICK_UNDO_ADMIT has rsx_io.

		auto& entry = data.checkpoint.ring[data.undo.tick % data.checkpoint.ring.size()];
		for (u64 page = address & ~u64{4095}; page < address + bytes; page += 4096)
		{
			const u32 p = static_cast<u32>(page >> 12);
			if (((data.checkpoint.static_page_bits[p >> 6] | data.undo.page_bits[p >> 6]) >> (p & 63)) & 1)
				continue;
			// Classify each page once. The renderer, its IO map and the deny list are invariant for the run, so
			// repeating this per tick would only burn cycles. The library test depends on the STORING pc, not the page, so it
			// stays out of the cache and is evaluated per store below.
			bool rsx_page, denied;
			classify_page(page, p, rsx_page, denied);
			if (denied || rsx_page || (!data.undo.admit_library && (ppu.cia < 0x10000 || ppu.cia >= 0x435000)))
			{
				// Outside the EBOOT text segment (loader: 0x10000-0x434fa8): library sync/kernel-backed state such as lwmutex
				// fields; also declared deny spans and RSX IO-mapped pages. Refused pages are marked for the tick and never saved.
				if (entry.skip_pages.size() >= data.undo.max_pages)
				{
					if (!entry.undo_overflow)
						data.undo.overflows++;
					entry.undo_overflow = true;
					return;
				}
				entry.skip_pages.push_back(static_cast<u32>(page));
				entry.skip_pcs.push_back(static_cast<u32>(ppu.cia) | (rsx_page ? 1u : 0u) | (denied ? 2u : 0u)); // Bit 0: RSX-facing, bit 1: denied.
				data.undo.page_bits[p >> 6] |= u64{1} << (p & 63);
				g_pbd_undo_skip_bits[p >> 6] |= u64{1} << (p & 63);
				data.undo.marked.push_back(p);
				continue;
			}
			if (entry.undo_pages.size() >= data.undo.max_pages)
			{
				if (!entry.undo_overflow)
					data.undo.overflows++;
				entry.undo_overflow = true;
				return;
			}
			const usz offset = entry.undo_bytes.size();
			entry.undo_bytes.resize(offset + 4096);
			if (!vm::try_access(static_cast<u32>(page), entry.undo_bytes.data() + offset, 4096, false))
			{
				entry.undo_bytes.resize(offset); // Unmapped: the store faults in the interpreter.
				continue;
			}
			entry.undo_pages.push_back(static_cast<u32>(page));
			entry.undo_pcs.push_back(static_cast<u32>(ppu.cia));
			entry.undo_lrs.push_back(static_cast<u32>(ppu.lr));
			data.undo.page_bits[p >> 6] |= u64{1} << (p & 63);
			g_pbd_undo_skip_bits[p >> 6] |= u64{1} << (p & 63);
			data.undo.marked.push_back(p);
			data.undo.saves++;
		}
	}

	bool write_fault(u32 p) noexcept
	{
		pbd_fault_guard guard(data.fault.lock);
		if (!((g_pbd_fault_bits[p >> 6] >> (p & 63)) & 1))
			return true; // Another thread captured and unprotected this page meanwhile: retry the store.
		const auto begin = session_clock::now();
		const u32 page = p << 12;
		ring_entry* entry = data.checkpoint.ring.empty() ? nullptr : &data.checkpoint.ring[data.undo.tick % data.checkpoint.ring.size()];
		const auto cpu = get_current_cpu_thread();
		const bool owner = cpu && cpu->id == pbd::t5dr::main_ppu_id;
		const bool first_foreign = !((data.fault.foreign_page_bits[p >> 6] >> (p & 63)) & 1);
		capture_ring_page(entry, owner, cpu, page);
		// A takeover journal keeps the first owner-written preimage after T. Re-arming every boundary can fault the same page again;
		// the map deliberately retains the earliest copy so restoring it equals applying a long undo ring newest-to-oldest.
		capture_takeover_page(owner, page);
		// Replay rewind: the newest checkpoint's journal keeps the first owner write of each page since that checkpoint. Only the
		// owner thread writes this list (here and at its boundary), so it needs no lock beyond this handler's.
		capture_rewind_page(owner, page);
		if (!owner)
		{
			if (entry && entry->foreign_pages.size() < data.undo.max_pages)
				entry->foreign_pages.push_back(page);
			if (first_foreign)
			{
				data.fault.foreign_page_bits[p >> 6] |= u64{1} << (p & 63);
				data.fault.foreign_pages++;
			}
			data.fault.foreign++;
		}
		utils::memory_protect(vm::g_base_addr + page, 4096, utils::protection::rw);
		g_pbd_fault_bits[p >> 6] &= ~(u64{1} << (p & 63));
		data.fault.disarmed.push_back(p);
		data.fault.events++;
		data.fault.capture_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - begin).count();
		return true;
	}

	static bool rsx_command_page(u64 page)
	{
		const auto render = rsx::get_current_renderer();
		if (!render)
			return false;
		if ((render->label_addr & ~u32{4095}) == page || (render->dma_address & ~u32{4095}) == page)
			return true;
		if (!render->ctrl)
			return false;
		for (const u32 offset : {render->ctrl->get.load(), render->ctrl->put.load()})
		{
			const u32 ea = render->iomap_table.get_addr(offset);
			if (ea != umax && (page >> 20) == (ea >> 20))
				return true;
		}
		return false;
	}

	static bool rsx_io_page(u64 page)
	{
		const auto render = rsx::get_current_renderer();
		return render && render->iomap_table.io[page >> 20] != umax;
	}

	void classify_page(u64 page, u32 p, bool& rsx_page, bool& denied)
	{
		if ((data.undo.classified_page_bits[p >> 6] >> (p & 63)) & 1)
		{
			rsx_page = ((data.undo.refused_page_bits[p >> 6] >> (p & 63)) & 1) != 0;
			denied = ((data.undo.denied_page_bits[p >> 6] >> (p & 63)) & 1) != 0;
		}
		else
		{
			// The two verdicts stay SEPARATE: skip_pcs bit 0 means RSX-facing and bit 1 means a declared deny span,
			// and the undo-policy evidence depends on telling them apart.
			bool io_admitted = false;
			for (const auto& [lo, hi] : data.undo.admit_io_spans)
				io_admitted = io_admitted || (page >= lo && page < hi);
			rsx_page = rsx_command_page(page) || (!data.undo.admit_rsx_io && !io_admitted && rsx_io_page(page));
			denied = false;
			for (const auto& [begin, end] : data.undo.deny)
				denied = denied || (page < end && page + 4096 > begin);
			data.undo.classified_page_bits[p >> 6] |= u64{1} << (p & 63);
			if (rsx_page)
				data.undo.refused_page_bits[p >> 6] |= u64{1} << (p & 63);
			if (denied)
				data.undo.denied_page_bits[p >> 6] |= u64{1} << (p & 63);
		}
	}

	void capture_ring_page(ring_entry* entry, bool owner, auto* cpu, u32 page)
	{
		if (entry && (owner || data.fault.capture_foreign))
		{
			if (entry->undo_pages.size() >= data.undo.max_pages)
			{
				if (!entry->undo_overflow)
					data.undo.overflows++;
				entry->undo_overflow = true;
			}
			else
			{
				const usz offset = entry->undo_bytes.size();
				entry->undo_bytes.resize(offset + 4096);
				std::memcpy(entry->undo_bytes.data() + offset, vm::g_sudo_addr + page, 4096);
				entry->undo_pages.push_back(page);
				entry->undo_pcs.push_back(owner ? static_cast<u32>(static_cast<ppu_thread*>(cpu)->cia) : 0xffffffffu);
				entry->undo_lrs.push_back(owner ? static_cast<u32>(static_cast<ppu_thread*>(cpu)->lr) : cpu ? cpu->id :
																											  0u);
				data.undo.saves++;
				data.fault.captures++;
			}
		}
	}

	void capture_takeover_page(bool owner, u32 page)
	{
		if (owner && data.replay.takeover_fast && data.takeover.keyframe.valid)
		{
			auto& a = data.takeover.keyframe;
			if (!a.journal_index.count(page))
			{
				if (a.journal_pages.size() >= 4096)
					a.overflow = true;
				else
				{
					const usz slot = a.journal_pages.size();
					a.journal_index.emplace(page, slot);
					a.journal_pages.push_back(page);
					a.journal_bytes.resize((slot + 1) * 4096ull);
					std::memcpy(a.journal_bytes.data() + slot * 4096ull, vm::g_sudo_addr + page, 4096);
					data.fault.captures++;
				}
			}
		}
	}

	void capture_rewind_page(bool owner, u32 page)
	{
		if (owner && data.rewind.enabled && !data.rewind.points.empty())
		{
			auto& a = data.rewind.points.back()->anchor;
			if (!a.overflow && !a.journal_index.count(page))
			{
				try
				{
					if (a.journal_pages.size() >= 4096)
						a.overflow = true;
					else
					{
						const usz slot = a.journal_pages.size();
						a.journal_bytes.resize((slot + 1) * 4096ull);
						std::memcpy(a.journal_bytes.data() + slot * 4096ull, vm::g_sudo_addr + page, 4096);
						a.journal_pages.push_back(page);
						a.journal_index.emplace(page, slot);
					}
				}
				catch (...)
				{
					a.overflow = true;
				}
			}
		}
	}
};
