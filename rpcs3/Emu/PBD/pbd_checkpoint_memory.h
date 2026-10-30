#pragma once

#include "pbd_checkpoint_write_plan.h"

namespace pbd::session_runtime
{
	inline bool read_pages(const std::vector<u32>& pages, std::vector<u8>& out) noexcept
	{
		try
		{
			out.resize(pages.size() * 4096ull);
		}
		catch (...)
		{
			return false;
		}
		for (usz i = 0; i < pages.size(); i++)
			if (!vm::try_access(pages[i], out.data() + i * 4096, 4096, false))
				return false;
		return true;
	}

	class checkpoint_memory_writer
	{
		pbd::checkpoint_memory::write_plan policy;
		static bool write_guest(u32 address, std::span<const u8> bytes) noexcept
		{
			return vm::try_access(address, const_cast<u8*>(bytes.data()), static_cast<u32>(bytes.size()), true);
		}

	public:
		void reset(const std::vector<u32>& pages)
		{
			policy.reset(pages);
		}
		void exclude(u64 begin, u64 end, const std::vector<u32>& pages)
		{
			policy.exclude(begin, end, pages);
		}
		void prepare(const std::vector<u32>& pages)
		{
			policy.prepare(pages);
		}
		bool write_pages_except(const std::vector<u32>& pages, std::vector<u8>& image, u64& written) noexcept
		{
			return policy.write_pages_except(pages, image, written, write_guest);
		}
		bool write_page_except_ranges(u32 page, const u8* image, u64& excluded) noexcept
		{
			return policy.write_page_except_ranges(page, image, excluded, write_guest);
		}

		// The older identity/roundtrip diagnostics also write the voice page; use the same lease as live correction.
		bool write_pages_voice_guarded(ppu_thread& ppu, const std::vector<u32>& pages, std::vector<u8>& image, u64& written) noexcept
		{
			if (!pbd::fn_hook::voice_restore_begin(ppu))
				return false;
			struct release
			{
				~release()
				{
					pbd::fn_hook::voice_restore_end();
				}
			} lease;
			return write_pages_except(pages, image, written);
		}

		// Non-excluded differing bytes; first 4,096 spans as [address,length].
		std::string byte_differences(const std::vector<u32>& pages, const std::vector<u8>& a, const std::vector<u8>& b, u64& count, u64& excluded_count)
		{
			std::string spans;
			u32 listed = 0;
			for (usz i = 0; i < pages.size(); i++)
			{
				for (u32 o = 0; o < 4096; o++)
				{
					if (a[i * 4096 + o] == b[i * 4096 + o])
						continue;
					if (policy.excluded_at(i, o, pages[i] + o))
					{
						excluded_count++;
						continue;
					}
					u32 s = o;
					while (o + 1 < 4096 && a[i * 4096 + o + 1] != b[i * 4096 + o + 1] && !policy.excluded_at(i, o + 1, pages[i] + o + 1))
						o++;
					count += o + 1 - s;
					if (listed++ < 4096)
						spans += fmt::format("%s[%u,%u]", spans.empty() ? "" : ",", pages[i] + s, o + 1 - s);
				}
			}
			return spans;
		}
	};

	inline bool roundtrip_locks_free() noexcept
	{
		for (u32 address : sampled_lwmutexes)
		{
			std::array<be_t<u32>, 2> w{};
			if (!vm::try_access(address, w.data(), sizeof(w), false) || w[0] != 0xffffffffu || w[1] != 0u)
				return false;
		}
		auto* mutex = idm::check_unlocked<lv2_obj, lv2_mutex>(sampled_mutex_id);
		if (!mutex)
			return false;
		std::shared_lock object_lock(mutex->mutex, std::try_to_lock);
		if (!object_lock.owns_lock())
			return false;
		const auto control = mutex->control.load();
		return !control.owner && !control.sq;
	}

	// Diagnostic: which of the guarded locks is not free, as JSON items [address, owner, waiter] (the lv2 mutex as
	// [id, owner, 1 if it has sleepers]). The lwmutex control blocks are not restored, so their words
	// are the truth, and a lock that stays busy through the whole wait names a thread that really holds it that long.
	inline std::string roundtrip_locks_busy_text() noexcept
	{
		std::string text;
		for (u32 address : sampled_lwmutexes)
		{
			std::array<be_t<u32>, 2> w{};
			const bool read = vm::try_access(address, w.data(), sizeof(w), false);
			if (!read || w[0] != 0xffffffffu || w[1] != 0u)
				text += fmt::format("%s[%u,%u,%u]", text.empty() ? "" : ",", address, read ? u32{w[0]} : 0u, read ? u32{w[1]} : 0u);
		}
		if (auto* mutex = idm::check_unlocked<lv2_obj, lv2_mutex>(sampled_mutex_id))
		{
			const auto control = mutex->control.load();
			if (control.owner || control.sq)
				text += fmt::format("%s[%u,%u,%u]", text.empty() ? "" : ",", sampled_mutex_id, control.owner, control.sq ? 1u : 0u);
		}
		return text;
	}

} // namespace pbd::session_runtime
