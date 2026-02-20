// JIT Module: Function Table Lookup
// ---------------------------------
// Reads compiled function pointers from the global JIT table.

#define HVM4_RUNTIME
#include "runtime.h"
#undef HVM4_RUNTIME

// Returns the compiled JIT function pointer for a definition id.
fn HvmJitFn jit_get(u32 id) {
  if (id >= BOOK_CAP) {
    return NULL;
  }
  return JIT_FNS[id];
}
