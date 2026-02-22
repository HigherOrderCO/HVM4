// Runtime FFI Loader
// ==================
// Loads requested FFI libraries/directories with optional override warning suppression.

// Forward declarations
// --------------------

fn void prim_set_warn_overrides(int enabled);
fn void ffi_load(const char *path);
fn void ffi_load_dir(const char *path);

// Runtime FFI Load
// ----------------

// Loads a list of FFI targets for the current runtime session.
fn void runtime_load_ffi(const RuntimeFfiLoad *loads, u32 len, int suppress_warnings) {
  if (suppress_warnings) {
    prim_set_warn_overrides(0);
  }

  for (u32 i = 0; i < len; i++) {
    const RuntimeFfiLoad load = loads[i];
    if (load.path == NULL) {
      continue;
    }

    if (load.is_dir) {
      ffi_load_dir(load.path);
    } else {
      ffi_load(load.path);
    }
  }

  if (suppress_warnings) {
    prim_set_warn_overrides(1);
  }
}
