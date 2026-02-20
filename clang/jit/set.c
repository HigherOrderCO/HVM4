// JIT Module: Function Table Store
// --------------------------------
// Stores compiled function pointers in the global JIT table.

#define HVM4_RUNTIME
#include "runtime.h"
#undef HVM4_RUNTIME

// Stores a compiled JIT function pointer for a definition id.
fn void jit_set(u32 id, HvmJitFn fun) {
  if (id >= BOOK_CAP) {
    return;
  }
  JIT_FNS[id] = fun;
}
