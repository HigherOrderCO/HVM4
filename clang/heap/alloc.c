fn u64 heap_alloc(u64 size) {
  u32 tid  = WNF_TID;
  u64 idx  = (u64)tid * HEAP_STRIDE;
  u64 at   = HEAP_NEXT[idx];
  u64 next = at + size;
  if (__builtin_expect(next <= HEAP_END[idx] && next >= at, 1)) {
    HEAP_NEXT[idx] = next;
    return at;
  }
  fprintf(stderr,
          "Out of heap memory in thread bank %u (need %llu words)\n",
          tid, (unsigned long long)size);
  exit(1);
}
