fn u64 heap_alloc(u64 size) {
  u32 tid  = WNF_TID;
  // Check free list first (sizes 1-8)
  if (__builtin_expect(size <= FREE_LIST_MAX_SIZE, 1)) {
    u64 head = FREE_HEAD_AT(tid, size);
    if (__builtin_expect(head != 0, 0)) {
      FREE_HEAD_AT(tid, size) = HEAP[head];
      return head;
    }
  }
  // Bump allocate
  u64 idx  = (u64)tid * HEAP_STRIDE;
  u64 at   = HEAP_NEXT[idx];
  u64 next = at + size;
  if (__builtin_expect(next <= HEAP_END[idx] && next >= at, 1)) {
    HEAP_NEXT[idx] = next;
    return at;
  }
  fprintf(stderr,
          "Out of heap memory in thread bank %u (need %llu words)\n",
          tid, size);
  exit(1);
}
