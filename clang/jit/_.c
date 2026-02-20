// HVM4 JIT Runtime State
// ======================
//
// This file owns global state shared by all JIT modules.
//
// Design summary:
// - Trigger: when CLI runs with `--jit`, HVM4 compiles every top-level definition.
// - Artifact: each definition emits `./.hvm/fn_<name>.c`.
// - Build: each emitted file is compiled with `clang -O2` into a shared library.
// - Load: each shared object is loaded with `dlopen`, and `hvm4_jit_fn` is cached.
// - Dispatch: during WNF, when a REF is encountered, runtime first tries that JIT fn.
//
// Compiled function model:
// - It is a partial evaluation of WNF for one statically known root location.
// - It only fast-paths APP-LAM and APP-MAT (CTR/NUM).
// - APP-LAM: pop APP frame, capture argument into a local env array, continue.
// - APP-MAT: inspect APP arg kind/value, choose hit/miss branch, continue.
// - It keeps executing while the static head remains one of supported forms.
//
// Fallback model:
// - Any unsupported shape (stack too short, non-APP frame, wrong arg kind, etc.)
//   falls back to `jit_alo`, rebuilding `{subterm | env}` exactly as interpreter
//   would do from the current point.
// - Unsupported interactions are not compiled and immediately fallback to ALO.
//
// Invariants:
// - Generated C files are intentionally kept in `./.hvm/` for debugging.
// - Loaded library handles are kept alive for process lifetime.
// - JIT function table is indexed by definition id (`BOOK` id space).
//
#define HVM4_RUNTIME
#include "runtime.h"
#undef HVM4_RUNTIME

// Per-definition JIT entrypoint cache (id -> function pointer).
static HvmJitFn JIT_FNS[BOOK_CAP] = {0};

// Process-lifetime list of dlopen handles to avoid unloading JIT libraries.
static void **JIT_HANDLES    = NULL;
static u32    JIT_HANDLES_LEN = 0;
static u32    JIT_HANDLES_CAP = 0;
