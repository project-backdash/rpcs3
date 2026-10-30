#include "pbd_test.h"

#include "Emu/Cell/pbd_ppu_object_name.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using pbd::test::check;
namespace object_name = pbd::ppu_object_name;

namespace
{
	struct function
	{
		std::uint32_t addr;
		std::uint32_t size;
	};

	using sites = std::vector<std::uint32_t>;

	object_name::object_hooks hooks(const std::vector<function>& functions, const sites& observed_sites, const sites& skippable,
		const object_name::switches& run = {}, const sites& excluded = {})
	{
		return object_name::hooks_of(functions, excluded, {observed_sites, skippable}, run);
	}

	std::string tags(const std::vector<function>& functions, const sites& observed_sites, const sites& skippable,
		const object_name::switches& run = {})
	{
		return object_name::object_tags(hooks(functions, observed_sites, skippable, run));
	}
} // namespace

PBD_TEST(PbdPpuObjectName, HookTag)
{
	check(object_name::hook_tag({}, {}).empty());
	const sites observed_sites{0x159f0, 0x264f30};
	const auto tag = object_name::hook_tag(observed_sites, {});
	check(tag.size() == 11 && tag.starts_with("fn") && tag.ends_with("-"));
	check(object_name::hook_tag(observed_sites, {}) == tag);
	// A skippable entry emits an early return as well, so the same address as a probe is another tag.
	check(object_name::hook_tag(sites{0x159f0}, sites{0x264f30}) != tag);
	check(object_name::hook_tag(sites{0x159f0}, {}) != tag);
}

PBD_TEST(PbdPpuObjectName, GpuSampleReloadRejectsOlderObjects)
{
	const sites gpu{pbd::t5dr::gpu_load_probe};
	check(object_name::hook_tag(gpu, {}) != "fn554c2fa6-");
	check(object_name::hook_tag({}, gpu) != "fnfcd4a8d0-");
	check(object_name::hook_tag(sites{0x159f0, 0x264f30}, {}) == "fn7335e49d-");
	const std::vector<function> sampler{{0x259dc0, 0xd0}}, other{{0x264000, 0x2000}};
	check(tags(sampler, gpu, {}) != tags(sampler, {}, {}));
	check(tags(other, gpu, {}) == tags(other, {}, {}));
}

PBD_TEST(PbdPpuObjectName, ObjectNamesOnlyItsOwnSites)
{
	const std::vector<function> object{{0x10000, 0x100}, {0x10200, 0x40}, {0x10300, 0}};
	const auto found = hooks(object, {0xfffc, 0x10010, 0x10100, 0x10204, 0x10300, 0x20000}, {0x100f0});
	check(found.observed_sites == sites{0x10010, 0x10204});
	check(found.skippable == sites{0x100f0});
	check(!found.copy16);
	check(found.lists(0x10204) && found.lists(0x100f0) && !found.lists(0x20000));
	// An excluded function is not built, so its sites are not the object's.
	check(hooks(object, {0x10010, 0x10204}, {}, {}, {0x10200}).observed_sites == sites{0x10010});
	check(tags(object, {}, {}).empty());
}

PBD_TEST(PbdPpuObjectName, RunsShareObjectsTheirDifferingHooksMiss)
{
	// The pick hook every run emits, then lobby-only, Practice-only and match-only sites elsewhere in the game.
	const sites lobby_skippable{0x59000, 0x5aa18, 0x2739f4};
	const sites lobby_hook_sites{0x159f0, 0x53ee4, 0x5a554, 0x264f30};
	const sites practice_hook_sites{0x16d9c, 0xf0600, 0xf26f4, 0x264f30};
	const sites practice_skippable{0x108248, 0x139d44};
	const std::vector<function> untouched{{0x300000, 0x8000}};
	const std::vector<function> pick{{0x264000, 0x2000}};
	const std::vector<function> lobby_gate{{0x273000, 0x1000}};
	check(tags(untouched, lobby_hook_sites, lobby_skippable).empty());
	check(tags(untouched, lobby_hook_sites, lobby_skippable) == tags(untouched, practice_hook_sites, practice_skippable));
	check(!tags(pick, lobby_hook_sites, lobby_skippable).empty());
	check(tags(pick, lobby_hook_sites, lobby_skippable) == tags(pick, practice_hook_sites, practice_skippable));
	check(tags(lobby_gate, lobby_hook_sites, lobby_skippable) != tags(lobby_gate, practice_hook_sites, practice_skippable));
}

PBD_TEST(PbdPpuObjectName, CopyLoops)
{
	check(object_name::copy16_end(0x355c80) == 0x355c9c && object_name::copy16_end(0x355b4c) == 0x355b68);
	check(object_name::copy16_end(0x355c84) == 0);
	object_name::switches copy16;
	copy16.copy16 = true;
	// The function ends inside the loop's window: the translator still checks the window's words for hooks.
	const std::vector<function> loop{{0x355c60, 0x24}};
	const auto rewritten = hooks(loop, {0x355c98}, {}, copy16);
	check(rewritten.copy16 && rewritten.observed_sites == sites{0x355c98});
	check(tags(loop, {}, {}, copy16) == "cp16a-");
	check(tags(loop, {}, {}).empty());
	check(hooks(loop, {0x355c98}, {}).observed_sites.empty());
	const std::vector<function> elsewhere{{0x300000, 0x100}};
	check(tags(elsewhere, {}, {}, copy16).empty());
}

PBD_TEST(PbdPpuObjectName, InlinedLeaves)
{
	const std::vector<function> caller{{0x10000, 0x100}};
	const sites in_window{0x368040};
	const sites elsewhere{0x200000};
	object_name::switches window;
	window.leaf = 1;
	object_name::switches every;
	every.leaf = 2;
	check(hooks(caller, in_window, {}).observed_sites.empty());
	check(hooks(caller, in_window, {}, window).observed_sites == in_window);
	check(hooks(caller, elsewhere, {}, window).observed_sites.empty());
	check(hooks(caller, elsewhere, {}, every).observed_sites == elsewhere);
	check(hooks(caller, {0x36c000}, {}, window).observed_sites.empty());
}

PBD_TEST(PbdPpuObjectName, SameTagsMeanSameSitesInTheObject)
{
	// Whatever two runs' lists are, objects named alike have the same sites in every instruction the translator tests.
	std::uint32_t seed = 0x2545f491;
	const auto next = [&seed](std::uint32_t bound)
	{
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) % bound;
	};
	const auto draw = [&](std::uint32_t count)
	{
		sites out;
		for (std::uint32_t i = 0; i < count; i++)
			out.push_back(0x10000 + 4 * next(0x400));
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	};
	for (int round = 0; round < 2000; round++)
	{
		const function first{0x10000 + 4 * next(0x100), 4 * (1 + next(0x40))};
		const function second{0x10800 + 4 * next(0x40), 4 * next(0x20)};
		const std::vector<function> object{first, second};
		const sites a = draw(next(6)), b = draw(next(6));
		if (tags(object, a, {}) != tags(object, b, {}))
			continue;
		for (const auto& f : object)
			for (std::uint32_t at = f.addr; at < f.addr + f.size; at += 4)
				check(std::binary_search(a.begin(), a.end(), at) == std::binary_search(b.begin(), b.end(), at));
	}
}

PBD_TEST(PbdPpuObjectName, Identity)
{
	const sites observed_sites{0x159f0, 0x264f30};
	const sites skippable{0x59000};
	object_name::switches run;
	run.input = true;
	run.leaf = 1;
	run.scalar = 1;
	const auto base = object_name::identity("v8-x-", run, {observed_sites, skippable});
	check(base.starts_with("v8-x-split" + std::to_string(object_name::split_revision) + "-input1-fn"));
	check(base.ends_with("-leaf3-scalar1-"));
	check(object_name::identity("v8-x-", run, {observed_sites, skippable}) == base);
	check(object_name::identity("v8-y-", run, {observed_sites, skippable}) != base);
	check(object_name::identity("v8-x-", run, {observed_sites, {}}) != base);
	auto copy16 = run;
	copy16.copy16 = true;
	check(object_name::identity("v8-x-", copy16, {observed_sites, skippable}) != base);
	auto stock = run;
	stock.scalar = 0;
	check(object_name::identity("v8-x-", stock, {observed_sites, skippable}) != base);
	check(object_name::run_tags_before(run) == "input1-" && object_name::run_tags_after(run) == "leaf3-scalar1-");
}

PBD_TEST(PbdPpuObjectName, ObjectWrittenOnlyWithEveryHitNamed)
{
	using object_name::hit_kind;
	using hits = std::vector<object_name::hook_hit>;
	const std::vector<function> object{{0x10000, 0x100}};
	const auto own = hooks(object, {0x10010, 0x20000}, {0x100f0});
	check(!object_name::first_unnamed(hits{}, own, false));
	check(!object_name::first_unnamed(hits{{0x10010, hit_kind::observe}, {0x100f0, hit_kind::skippable}}, own, false));
	// A probe outside what the namer covers, a probe found as a skippable entry, an input hook without the input tag and a
	// rewritten copy loop without "cp16a-" are each refused.
	const auto outside = object_name::first_unnamed(hits{{0x10010, hit_kind::observe}, {0x20000, hit_kind::observe}}, own, false);
	check(outside && outside->site == 0x20000 && outside->kind == hit_kind::observe);
	check(object_name::first_unnamed(hits{{0x10010, hit_kind::skippable}}, own, false));
	check(object_name::first_unnamed(hits{{0x10040, hit_kind::input}}, own, false));
	check(!object_name::first_unnamed(hits{{0x10040, hit_kind::input}}, own, true));
	check(object_name::first_unnamed(hits{{0x355c80, hit_kind::copy_loop}}, own, false));
	object_name::switches copy16;
	copy16.copy16 = true;
	const std::vector<function> loop{{0x355c60, 0x24}};
	const auto rewritten = hooks(loop, {}, {}, copy16);
	check(!object_name::first_unnamed(hits{{0x355c80, hit_kind::copy_loop}}, rewritten, false));
	check(object_name::first_unnamed(hits{{0x355c84, hit_kind::copy_loop}}, rewritten, false));
}
