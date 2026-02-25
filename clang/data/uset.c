// data/uset.c - concurrent bitset for non-zero u64 keys.
//
// Context
// - Used by parallel normalization to track visited heap locations.
// - One bit per heap location.

typedef struct {
  _Atomic u64 *words;
  u64 word_count;
#if HVM_WINDOWS
  u64 committed_words;
  u64 page_words;
  u64 step_words;
  SRWLOCK commit_lock;
#endif
} Uset;

#if HVM_WINDOWS
fn u64 uset_align_up_words(u64 value, u64 align) {
  if (align <= 1) {
    return value;
  }
  u64 rem = value % align;
  if (rem == 0) {
    return value;
  }
  return value + (align - rem);
}

fn void uset_commit_until(Uset *set, u64 need_words) {
  if (need_words <= set->committed_words) {
    return;
  }
  AcquireSRWLockExclusive(&set->commit_lock);
  if (need_words > set->committed_words) {
    u64 target = uset_align_up_words(need_words, set->step_words);
    if (target > set->word_count) {
      target = set->word_count;
    }
    target = uset_align_up_words(target, set->page_words);
    if (target > set->word_count) {
      target = set->word_count;
    }
    if (target > set->committed_words) {
      size_t bytes = (size_t)((target - set->committed_words) * sizeof(u64));
      void *base = (void *)((char *)set->words + (set->committed_words * sizeof(u64)));
      void *ok = VirtualAlloc(base, bytes, MEM_COMMIT, PAGE_READWRITE);
      if (ok == NULL) {
        ReleaseSRWLockExclusive(&set->commit_lock);
        fprintf(stderr, "uset: allocation failed\n");
        exit(1);
      }
      set->committed_words = target;
    }
  }
  ReleaseSRWLockExclusive(&set->commit_lock);
}
#endif

// Initialize bitset storage.
fn void uset_init(Uset *set) {
  set->word_count = HEAP_CAP >> 6;
  set->words = NULL;
#if HVM_WINDOWS
  set->committed_words = 0;
  set->page_words = 1;
  set->step_words = 1;
#endif
  if (set->word_count == 0 || set->word_count > ((u64)SIZE_MAX / sizeof(u64))) {
    fprintf(stderr, "uset: allocation failed\n");
    exit(1);
  }
  size_t bytes = (size_t)(set->word_count * sizeof(u64));
  void *map;
#if HVM_WINDOWS
  SYSTEM_INFO sys_info;
  GetSystemInfo(&sys_info);
  u64 page_bytes = (u64)sys_info.dwPageSize;
  if (page_bytes == 0) {
    page_bytes = 4096;
  }
  set->page_words = page_bytes / sizeof(u64);
  if (set->page_words == 0) {
    set->page_words = 1;
  }
  u64 step_bytes = 1ULL << 20; // 1 MiB commit chunks.
  if (step_bytes < page_bytes) {
    step_bytes = page_bytes;
  }
  set->step_words = step_bytes / sizeof(u64);
  if (set->step_words == 0) {
    set->step_words = set->page_words;
  }
  if (set->step_words % set->page_words != 0) {
    set->step_words = ((set->step_words + set->page_words - 1) / set->page_words) * set->page_words;
  }
  InitializeSRWLock(&set->commit_lock);
  map = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
  map = sys_mmap_anon(bytes);
#endif
  if (map == NULL) {
    fprintf(stderr, "uset: allocation failed\n");
    exit(1);
  }
  set->words = (_Atomic u64 *)map;
}

// Release storage and reset state.
fn void uset_free(Uset *set) {
  if (set->words) {
#if HVM_WINDOWS
    VirtualFree((void *)set->words, 0, MEM_RELEASE);
#else
    size_t bytes = (size_t)(set->word_count * sizeof(u64));
    sys_munmap_anon((void *)set->words, bytes);
#endif
  }
  *set = (Uset){0};
}

// Insert key if missing; returns 1 if inserted, 0 if already present.
fn u8 uset_add(Uset *set, u64 key) {
  u64 word_idx = key >> 6;
  if (__builtin_expect(word_idx >= set->word_count, 0)) {
    return 0;
  }
#if HVM_WINDOWS
  uset_commit_until(set, word_idx + 1);
#endif
  u64 bit_mask = 1ull << (key & 63u);
  u64 prev = atomic_fetch_or_explicit(&set->words[word_idx], bit_mask, memory_order_relaxed);
  return (prev & bit_mask) == 0;
}
