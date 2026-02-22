// Runtime Shared Types
// ====================
// Defines small structs shared by CLI, runtime helpers, and AOT generation.

// Limits
// ------

#define RUNTIME_FFI_MAX 128

// Types
// -----

// One FFI load target: either a single library path or a directory path.
typedef struct {
  int         is_dir;
  const char *path;
} RuntimeFfiLoad;

// Runtime evaluation behavior flags for running one entrypoint.
typedef struct {
  int do_collapse;
  int collapse_limit;
  int stats;
  int silent;
  int step_by_step;
} RuntimeEvalCfg;
