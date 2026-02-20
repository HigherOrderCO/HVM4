// JIT Module: Handle Retention
// ----------------------------
// Stores dlopen handles so JIT-loaded shared libraries remain valid.

fn void sys_error(const char *msg);

// Keeps loaded JIT handles alive for the process lifetime.
fn void jit_handle_push(void *handle) {
  if (JIT_HANDLES_LEN >= JIT_HANDLES_CAP) {
    u32 new_cap = JIT_HANDLES_CAP == 0 ? 8 : JIT_HANDLES_CAP * 2;
    void **new_handles = realloc(JIT_HANDLES, new_cap * sizeof(void *));
    if (new_handles == NULL) {
      sys_error("JIT handle allocation failed");
    }
    JIT_HANDLES     = new_handles;
    JIT_HANDLES_CAP = new_cap;
  }
  JIT_HANDLES[JIT_HANDLES_LEN++] = handle;
}
