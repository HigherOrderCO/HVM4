// Runtime Session Free
// --------------------
// Releases process-global state for one program execution.

// Frees runtime-global allocations for the current process run.
fn void runtime_free(void) {
  free(HEAP);
  free(BOOK);
  free(TABLE.data);
  wnf_stack_free();
}
