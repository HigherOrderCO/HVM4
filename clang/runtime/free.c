// Runtime Session Free
// --------------------
// Releases process-global state for one program execution.

#include <sys/mman.h>

// Frees runtime-global allocations for the current process run.
fn void runtime_free(void) {
  if (HEAP != NULL) {
    size_t heap_bytes = (size_t)(HEAP_CAP * sizeof(Term));
    munmap(HEAP, heap_bytes);
    HEAP = NULL;
  }
  free(BOOK);
  free(TABLE.data);
  wnf_stack_free();
}
