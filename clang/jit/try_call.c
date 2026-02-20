// JIT Module: Runtime Dispatch Probe
// ----------------------------------
// Attempts to call a compiled symbol for a REF; returns 0 when not available.

#define HVM4_RUNTIME
#include "runtime.h"
#undef HVM4_RUNTIME

fn HvmJitFn jit_get(u32 id);

// Calls a compiled symbol if present and stores the result in out.
fn int jit_try_call(u32 id, Term *stack, u32 *s_pos, u32 base, Term *out) {
  HvmJitFn fun = jit_get(id);
  if (fun == NULL) {
    return 0;
  }
  *out = fun(stack, s_pos, base);
  return 1;
}
