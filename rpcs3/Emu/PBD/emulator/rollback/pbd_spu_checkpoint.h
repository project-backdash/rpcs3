#pragma once
#include "util/types.hpp"
class spu_thread;
// Proof of explicit Safe-LLVM full-GPR stores at receive-event/state-check,
// or the native startup/stopped root outside cpu_task. Lifetime ends before
// any generated-code escape. The root requires committed compatible state.
struct pbd_spu_materialized_scope
{
	spu_thread& cpu;
	explicit pbd_spu_materialized_scope(spu_thread&, u32 code);
	~pbd_spu_materialized_scope();
};
// An explicitly materialized LLVM state-check may escape before clearing the
// stock unsavable flag. Retain only this GPR-commit fact until the next JIT entry.
void pbd_spu_invalidate_materialized_exit(spu_thread&) noexcept;
void pbd_spu_note_materialized_exit(spu_thread&) noexcept;
bool pbd_spu_has_materialized_exit(spu_thread&) noexcept;
bool pbd_spu_exit_clears_unsavable(spu_thread&) noexcept;
