#if HVM_WINDOWS
  #include <windows.h>
#endif

#if HVM_WINDOWS
fn u64 heap_align_up_words(u64 value, u64 align) {
  if (align <= 1) {
    return value;
  }
  u64 rem = value % align;
  if (rem == 0) {
    return value;
  }
  return value + (align - rem);
}

fn void heap_commit_until(u32 tid, u64 need) {
  u64 idx = (u64)tid * HEAP_STRIDE;
  if (need <= HEAP_COMMIT[idx]) {
    return;
  }

  u64 page_words = HEAP_PAGE_WORDS > 0 ? HEAP_PAGE_WORDS : 1;
  u64 step_words = HEAP_COMMIT_STEP_WORDS > 0 ? HEAP_COMMIT_STEP_WORDS : page_words;
  u64 end_words  = HEAP_END[idx];

  u64 old_commit = HEAP_COMMIT[idx];
  u64 old_page   = (old_commit / page_words) * page_words;
  u64 target     = heap_align_up_words(need, step_words);
  if (target > end_words) {
    target = end_words;
  }
  u64 target_page = heap_align_up_words(target, page_words);
  if (target_page > end_words) {
    target_page = end_words;
  }
  if (target_page <= old_page) {
    return;
  }

  size_t commit_bytes = (size_t)((target_page - old_page) * sizeof(Term));
  void *base = (void *)((char *)HEAP + (old_page * sizeof(Term)));
  void *ok = VirtualAlloc(base, commit_bytes, MEM_COMMIT, PAGE_READWRITE);
  if (!ok) {
    fprintf(stderr,
            "Out of committed heap memory in thread bank %u (need %llu words)\n",
            tid, (unsigned long long)(need - old_commit));
    exit(1);
  }
  HEAP_COMMIT[idx] = target_page;
}
#endif

fn u64 heap_alloc(u64 size) {
  u32 tid  = WNF_TID;
  u64 idx  = (u64)tid * HEAP_STRIDE;
  u64 at   = HEAP_NEXT[idx];
  u64 next = at + size;
  if (__builtin_expect(next <= HEAP_END[idx] && next >= at, 1)) {
#if HVM_WINDOWS
    heap_commit_until(tid, next);
#endif
    HEAP_NEXT[idx] = next;
    return at;
  }
  fprintf(stderr,
          "Out of heap memory in thread bank %u (need %llu words)\n",
          tid, (unsigned long long)size);
  exit(1);
}
