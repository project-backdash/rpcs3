#pragma once

#include "pbd_rematch.h"
#include <optional>

class ppu_thread;

namespace pbd::rematch
{
	enum class progress
	{
		waiting,
		loading,
		complete,
		refused
	};
	struct native_result
	{
		progress state = progress::waiting;
		const char* reason = "";
	};
	bool capture_options(ppu_thread& owner, options& out, bool initial_select = false) noexcept;
	class native_load
	{
		struct target
		{
			selection selected;
			options rules;
			std::optional<u32> repick;
			bool initial_select = false;
		};
		std::optional<target> pending;
		u64 generation = 0;
		u32 first_boundary = 0;
		bool requested = false;

	public:
		bool active() const
		{
			return pending.has_value();
		}
		// A load without a repick replaces the select with the stock resource request and goes straight to VS, behind the loading
		// card. A repick runs the stock select in the open: this only follows it to the battle and checks what it applied there.
		bool straight_to_vs() const
		{
			return pending && !pending->repick;
		}
		bool arm(const load& value, const sealed_state& previous, bool draw, u64 runtime, u32 boundary, bool next_set = false);
		bool arm_start(const selection& selected, const options& rules, u64 runtime, u32 boundary);
		native_result advance(ppu_thread& owner, u32 boundary) noexcept;
	};
} // namespace pbd::rematch
