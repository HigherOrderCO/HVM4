// heap/free.c — Stub for removed free-list allocator
//
// The per-thread free-list was replaced by the epoch nursery system.
// heap_free_reset() is called by epoch_reset() for backward compatibility.

fn void heap_free_reset(void) {
  // No-op: free lists no longer exist
}
