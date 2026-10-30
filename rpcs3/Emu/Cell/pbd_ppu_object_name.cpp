#include "stdafx.h"
#include "pbd_ppu_object_name.h"
#include "PPUAnalyser.h"
#include "PPUThread.h"
#include "pbd_fn_hook.h"
#include "pbd_rollback_session.h"
#include "lv2/sys_sync.h"

#ifdef LLVM_AVAILABLE
#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
#endif

namespace pbd::ppu_object_name
{
	namespace
	{
		switches run_switches()
		{
			return {pbd::rollback_session::requested(), pbd_llvm_undo_requested(), pbd_jit_copy16_enabled(),
				pbd_jit_inline_leaf_mode(), pbd_jit_scalar_mode()};
		}

		hook_lists run_hook_lists()
		{
			return {pbd::fn_hook::emitted_observers(), pbd::fn_hook::emitted_skippable()};
		}

		object_hooks part_hooks(const ppu_module<lv2_obj>& part)
		{
			if (part.is_relocatable)
				return {};
			return hooks_of(part.get_funcs(), part.excluded_funcs, run_hook_lists(), run_switches());
		}
	} // namespace

	std::string build_prefix()
	{
		// The ABI revision (abiN) must change whenever emitted code changes without changing a field offset, a setting or a
		// tag, or objects cached under the older revision are reused (docs/design/ppu-object-names.md). A firmware library's
		// name is this prefix and its hashes alone, so a new revision makes every player compile all of the firmware again on
		// the next boot. A change that reaches only game code is fenced with a tag in part_tags instead, which firmware lacks.
		return fmt::format("v8-dr2026-abi7-l%x-%x-%x-%x-", sizeof(ppu_thread), ::offset32(&ppu_thread::state),
			::offset32(&ppu_thread::gpr), ::offset32(&ppu_thread::hv_ctx));
	}

	std::string part_tags(const ppu_module<lv2_obj>& part)
	{
		if (part.is_relocatable)
			return {};
		const switches run = run_switches();
		return run_tags_before(run) + object_tags(part_hooks(part)) + run_tags_after(run);
	}

	std::optional<hook_hit> unnamed_hit(const ppu_module<lv2_obj>& part, std::span<const hook_hit> hits)
	{
		if (hits.empty())
			return std::nullopt;
		// A relocatable part names no hook and no switch, so any hit in one is unnamed.
		return first_unnamed(hits, part_hooks(part), !part.is_relocatable && run_switches().input);
	}

#ifdef LLVM_AVAILABLE
	bool calls_only_named_hooks(const llvm::Module& module, const ppu_module<lv2_obj>& part)
	{
		const auto observe = module.getFunction("__dr2026_fn_probe");
		if (!observe)
			return true;
		const object_hooks named = part_hooks(part);
		for (const llvm::User* user : observe->users())
		{
			const auto call = llvm::dyn_cast<llvm::CallInst>(user);
			if (!call || call->getCalledFunction() != observe)
				return false;
			const auto site = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
			if (!site || !named.lists(static_cast<address>(site->getZExtValue())))
				return false;
		}
		return true;
	}
#endif

	std::string run_identity()
	{
		return identity(build_prefix(), run_switches(), run_hook_lists());
	}
} // namespace pbd::ppu_object_name
