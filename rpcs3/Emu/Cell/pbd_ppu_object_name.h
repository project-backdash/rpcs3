#pragma once

#include "../PBD/pbd_t5dr_memory.h"

// The hook part of a game object's name, shared by the namer (PPUThread.cpp) and the translator.
// Why an object names only its own hooks: docs/design/ppu-object-names.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lv2_obj;
template <typename Type>
struct ppu_module;
namespace llvm
{
	class Module;
}

namespace pbd::ppu_object_name
{
	using address = std::uint32_t;

	// JIT copy16 rewrites these guest copy loops (PPUTranslator.cpp). `end` closes the seven words the translator checks for
	// hooks before it rewrites a loop, and is where the rewritten loop continues.
	struct copy_loop
	{
		address head;
		address end;
	};
	inline constexpr copy_loop copy16_loops[] = {{0x355c80, 0x355c9c}, {0x355b4c, 0x355b68}};

	// Vector-leaf inlining in mode 1 decodes callees only inside this window, into whichever object calls them. A hook in a
	// callee keeps the ordinary call, so every object's code depends on the hooks inside the window.
	inline constexpr address leaf_window_begin = 0x368000;
	inline constexpr address leaf_window_end = 0x36c000;

	// Where the copy loop starting at `head` continues once rewritten; 0 when no loop starts there.
	constexpr address copy16_end(address head) noexcept
	{
		for (const auto& loop : copy16_loops)
			if (loop.head == head)
				return loop.end;
		return 0;
	}

	// The run's switches that shape every game object's code.
	struct switches
	{
		bool input = false;       // the input hooks (pbd::rollback_session::requested)
		bool undo = false;        // compiled write capture
		bool copy16 = false;      // JIT copy16
		std::uint32_t leaf = 0;   // vector-leaf inlining mode
		std::uint32_t scalar = 0; // scalar IR passes
	};

	// The function hooks a run emits (pbd_fn_hook.cpp): each list sorted, the two disjoint.
	struct hook_lists
	{
		std::span<const address> observed_sites;
		std::span<const address> skippable;
	};

	// The function hooks and copy loops one object's code depends on.
	struct object_hooks
	{
		std::vector<address> observed_sites;
		std::vector<address> skippable;
		bool copy16 = false;

		bool lists(address site) const noexcept
		{
			return std::binary_search(observed_sites.begin(), observed_sites.end(), site) ||
			       std::binary_search(skippable.begin(), skippable.end(), site);
		}
	};

	// What the translator found while it built an object (PPUTranslator.cpp): a hook it tested for and found at `site`, or a
	// copy loop it rewrote there. ppu_initialize2 writes the object only when its name lists every one.
	enum class hit_kind : std::uint8_t
	{
		observe,   // pbd::fn_hook::probe_at, named by the object's hook tag
		skippable, // pbd::fn_hook::skippable_at, likewise
		input,     // pbd::rollback_session::slot_for, named by the run's input tag
		copy_loop, // a rewritten copy loop head, named by "cp16a-"
	};

	struct hook_hit
	{
		address site;
		hit_kind kind;
	};

	// "fn<hash>-" for a hook set, empty for none. The two leading words name the emitted probe shape: a tri-state answer
	// and a fatal branch before the instruction.
	inline std::string hook_tag(std::span<const address> observed_sites, std::span<const address> skippable)
	{
		if (observed_sites.empty() && skippable.empty())
			return {};
		std::uint64_t h = 0xcbf29ce484222325ull;
		const auto mix = [&h](address value)
		{
			for (int i = 0; i < 4; i++)
			{
				h ^= (value >> (8 * i)) & 0xff;
				h *= 0x100000001b3ull;
			}
		};
		mix(0x50524203u);
		mix(0x50524200u);
		// Only this probe reloads its r3 result after the callback. Other game and firmware objects keep their names.
		if (std::binary_search(observed_sites.begin(), observed_sites.end(), pbd::t5dr::gpu_load_probe) ||
			std::binary_search(skippable.begin(), skippable.end(), pbd::t5dr::gpu_load_probe))
			mix(0x47505501u);
		for (address site : observed_sites)
			mix(site);
		mix(0x534b5000u);
		for (address site : skippable)
			mix(site);
		char text[16];
		std::snprintf(text, sizeof(text), "fn%08x-", static_cast<std::uint32_t>(h ^ (h >> 32)));
		return text;
	}

	namespace detail
	{
		using range = std::pair<std::uint64_t, std::uint64_t>;

		inline void collect(std::span<const address> sorted, const range& where, std::vector<address>& out)
		{
			for (auto it = std::lower_bound(sorted.begin(), sorted.end(), where.first); it != sorted.end() && *it < where.second; ++it)
				out.push_back(*it);
		}

		inline void tidy(std::vector<address>& list)
		{
			std::sort(list.begin(), list.end());
			list.erase(std::unique(list.begin(), list.end()), list.end());
		}
	} // namespace detail

	// Every hook the translator can test while it builds `functions` (each with addr and size; an empty or excluded one is not
	// built): the instructions of each function, the hook window of a copy loop it rewrites and the inlinable callees.
	template <typename Functions>
	object_hooks hooks_of(const Functions& functions, std::span<const address> excluded, const hook_lists& lists, const switches& run)
	{
		object_hooks out;
		std::vector<detail::range> ranges;
		for (const auto& function : functions)
		{
			if (!function.size || std::find(excluded.begin(), excluded.end(), function.addr) != excluded.end())
				continue;
			const detail::range body{function.addr, std::uint64_t{function.addr} + function.size};
			ranges.push_back(body);
			for (const auto& loop : copy16_loops)
			{
				if (run.copy16 && loop.head >= body.first && loop.head < body.second)
				{
					out.copy16 = true;
					ranges.emplace_back(loop.head, loop.end);
				}
			}
		}
		if (run.leaf == 1)
			ranges.emplace_back(leaf_window_begin, leaf_window_end);
		else if (run.leaf != 0)
			ranges.emplace_back(0, std::uint64_t{1} << 32);
		for (const auto& where : ranges)
		{
			detail::collect(lists.observed_sites, where, out.observed_sites);
			detail::collect(lists.skippable, where, out.skippable);
		}
		detail::tidy(out.observed_sites);
		detail::tidy(out.skippable);
		return out;
	}

	// The tags every game object of a run carries before its own hook tags.
	inline std::string run_tags_before(const switches& run)
	{
		return std::string(run.input ? "input1-" : "") + (run.undo ? "undo1-" : "");
	}

	// The tags every game object of a run carries after its own hook tags.
	inline std::string run_tags_after(const switches& run)
	{
		static constexpr const char* leaf[] = {"", "leaf3-", "leaf4-"};
		static constexpr const char* scalar[] = {"", "scalar1-", "scalar2-", "scalar3-"};
		const char* leaf_tag = run.leaf < std::size(leaf) ? leaf[run.leaf] : "";
		const char* scalar_tag = run.scalar < std::size(scalar) ? scalar[run.scalar] : "";
		return std::string(leaf_tag) + scalar_tag;
	}

	// An object's own tags: the hooks it reaches, and "cp16a-" when it holds a rewritten copy loop.
	inline std::string object_tags(const object_hooks& hooks)
	{
		return hook_tag(hooks.observed_sites, hooks.skippable) + (hooks.copy16 ? "cp16a-" : "");
	}

	// The first of `hits` an object's name does not list, or none. `hooks` are the object's own (hooks_of), `input` whether its
	// name carries the input tag. A translator that tests for a hook outside what hooks_of covers is caught here, before two
	// objects with different code can share one name.
	inline std::optional<hook_hit> first_unnamed(std::span<const hook_hit> hits, const object_hooks& hooks, bool input)
	{
		for (const hook_hit& hit : hits)
		{
			bool named = false;
			switch (hit.kind)
			{
			case hit_kind::observe: named = std::binary_search(hooks.observed_sites.begin(), hooks.observed_sites.end(), hit.site); break;
			case hit_kind::skippable: named = std::binary_search(hooks.skippable.begin(), hooks.skippable.end(), hit.site); break;
			case hit_kind::input: named = input; break;
			case hit_kind::copy_loop: named = hooks.copy16 && copy16_end(hit.site) != 0; break;
			}
			if (!named)
				return hit;
		}
		return std::nullopt;
	}

	// The revision of how the game executable is analysed into functions (PPUAnalyser.cpp) and grouped into objects (the
	// split in ppu_initialize). Object names follow such a change by themselves, through the guest code hash, but the identity
	// below does not see the code: bump this with any change to either, so a launcher comparing identities prepares again.
	inline constexpr unsigned split_revision = 1;

	// Everything a game object's name takes from the build and the run, apart from the guest code, the settings and the CPU,
	// plus the split revision: two runs with the same identity name every object of the same game alike, so a launcher that
	// compares identities knows whether a run would find its objects compiled. The complete hook lists stand for each object's
	// share of them.
	inline std::string identity(std::string_view prefix, const switches& run, const hook_lists& lists)
	{
		const std::string split = "split" + std::to_string(split_revision) + "-";
		const std::string hooks = hook_tag(lists.observed_sites, lists.skippable) + (run.copy16 ? "cp16a-" : "");
		return std::string(prefix) + split + run_tags_before(run) + hooks + run_tags_after(run);
	}

	// The start of every object name: the name and ABI revisions and the ppu_thread layout the generated code bakes in.
	std::string build_prefix();
	// What follows the prefix: the run's switch tags around the object's own hook tags. None for a relocatable module (a
	// firmware library), which no PBD code reaches.
	std::string part_tags(const ppu_module<lv2_obj>& part);
	// The first hit of the translator that built `part` its name does not list (first_unnamed); such an object is not written.
	std::optional<hook_hit> unnamed_hit(const ppu_module<lv2_obj>& part, std::span<const hook_hit> hits);
	// False when `module` calls a function hook at a site the name of `part` does not list: such an object is not written.
	bool calls_only_named_hooks(const llvm::Module& module, const ppu_module<lv2_obj>& part);
	// The identity of a run with this process's environment, printed by --pbd-ppu-codegen.
	std::string run_identity();
} // namespace pbd::ppu_object_name
