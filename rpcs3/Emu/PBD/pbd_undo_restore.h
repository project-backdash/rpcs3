#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pbd::undo_restore
{
	// Refusals before the first possible write leave the current boundary usable. Once restoration starts, any
	// failure is terminal: no input capture or restore bridge may resume from a partially restored guest/host pair.
	template <typename Steps>
	bool restore_anchor(Steps& steps)
	{
		if (!steps.admit_anchor() || !steps.plan_journal() || !steps.wait_restore_ready())
			return false;
		if (!steps.restore_memory())
		{
			if (steps.restored.started)
				steps.stop_failed_restore();
			return false;
		}
		steps.audit_restore();
		if (!steps.restore_host_state() || !steps.rewind_record_writer() || !steps.discard_future_history())
		{
			steps.stop_failed_restore();
			return false;
		}
		steps.resume_input_capture();
		steps.start_restore_bridge();
		return true;
	}

	struct source
	{
		std::uint32_t journal = 0, address = 0;
		std::size_t slot = 0;
	};

	// Append journals oldest first, then write the selected sources in reverse to preserve the final-write order
	// of a full newest-to-oldest rewind. The caller's page writer applies exclusions.
	class plan
	{
		std::array<std::uint64_t, 16384> seen{};
		std::vector<source> selected;
		std::size_t available = 0;

	public:
		void clear() noexcept
		{
			for (const auto& copy : selected)
				seen[copy.address >> 18] &= ~(std::uint64_t{1} << ((copy.address >> 12) & 63));
			selected.clear();
			available = 0;
		}

		bool append(std::uint32_t journal, std::span<const std::uint32_t> pages, bool unique = true)
		{
			for (const auto page : pages)
				if (page & 4095)
					return false;
			available += pages.size();
			for (std::size_t slot = 0; slot < pages.size(); ++slot)
			{
				const auto page = pages[slot];
				auto& word = seen[page >> 18];
				const auto bit = std::uint64_t{1} << ((page >> 12) & 63);
				if (!unique || !(word & bit))
				{
					selected.push_back({journal, page, slot});
					word |= bit;
				}
			}
			return true;
		}

		std::span<const source> copies() const noexcept
		{
			return selected;
		}
		std::size_t candidates() const noexcept
		{
			return available;
		}
	};
} // namespace pbd::undo_restore
