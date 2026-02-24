// data/uset.c - concurrent bitset for non-zero u64 keys.
//
// Context
// - Used by parallel normalization to track visited heap locations.
// - One bit per heap location.

#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
  #define MAP_ANONYMOUS MAP_ANON
#endif

typedef struct {
  _Atomic u64 *words;
  u64 word_count;
} Uset;

// Initialize bitset storage.
fn void uset_init(Uset *set) {
  set->word_count = HEAP_CAP >> 6;
  set->words = NULL;
  if (set->word_count == 0 || set->word_count > ((u64)SIZE_MAX / sizeof(u64))) {
    fprintf(stderr, "uset: allocation failed\n");
    exit(1);
  }
  size_t bytes = (size_t)(set->word_count * sizeof(u64));
  int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
  mmap_flags |= MAP_NORESERVE;
#endif
  void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
  if (map == MAP_FAILED) {
    fprintf(stderr, "uset: allocation failed\n");
    exit(1);
  }
  set->words = (_Atomic u64 *)map;
}

// Release storage and reset state.
fn void uset_free(Uset *set) {
  if (set->words) {
    size_t bytes = (size_t)(set->word_count * sizeof(u64));
    munmap((void *)set->words, bytes);
  }
  *set = (Uset){0};
}

// Insert key if missing; returns 1 if inserted, 0 if already present.
fn u8 uset_add(Uset *set, u64 key) {
  u64 word_idx = key >> 6;
  if (__builtin_expect(word_idx >= set->word_count, 0)) {
    return 0;
  }
  u64 bit_mask = 1ull << (key & 63u);
  u64 prev = atomic_fetch_or_explicit(&set->words[word_idx], bit_mask, memory_order_relaxed);
  return (prev & bit_mask) == 0;
}
