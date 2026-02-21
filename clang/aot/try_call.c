// AOT Module: Runtime Dispatch Probe
// ----------------------------------
// Tries to execute a compiled function for a REF; returns 0 when absent.

// Calls one compiled symbol if present and stores its output.
fn int aot_try_call(u32 id, Term *stack, u32 *s_pos, u32 base, Term *out) {
  if (id >= BOOK_CAP) {
    return 0;
  }

  HvmAotFn fun = AOT_FNS[id];
  if (fun == NULL) {
    return 0;
  }

  *out = fun(stack, s_pos, base);
  return 1;
}
