// data/uset.c - concurrent set for non-zero u32 keys.
//
// Context
// - Used by parallel normalization to track visited heap locations.
// - Optimized for concurrent insert/contains; there is no deletion.
//
// Design
// - Direct-addressed bitmap over heap locations [0, HEAP_CAP).
// - One bit per location; key 0 remains unused by callers.
// - Concurrent insertion uses atomic fetch_or on the owning 64-bit word.
//
// Notes
// - Thread-safe for concurrent reads/writes from multiple threads.
// - On allocation failure, the process exits with an error.
// - Because 0 is reserved by convention, 0 is always reported as "not present".

typedef struct {
  _Atomic u64 *words;
  u64 word_count;
} Uset;

// Number of 64-bit words needed to represent all heap locations.
fn u64 uset_words_for_heap(void) {
  return (HEAP_CAP + 63ull) >> 6;
}

// Initialize the set bitmap covering max_locs heap locations.
fn void uset_init_sized(Uset *set, u64 max_locs) {
  u64 words = (max_locs + 63ull) >> 6;
  if (words == 0) words = 1;
  set->words = (_Atomic u64 *)calloc((size_t)words, sizeof(u64));
  if (!set->words) {
    fprintf(stderr, "uset: allocation failed\n");
    exit(1);
  }
  set->word_count = words;
}

// Initialize the set bitmap covering all HEAP_CAP locations.
fn void uset_init(Uset *set) {
  uset_init_sized(set, HEAP_CAP);
}

// Release the bitmap and reset the set state.
fn void uset_free(Uset *set) {
  if (set->words) {
    free((void *)set->words);
  }
  *set = (Uset){0};
}

// Check whether key is present (0 is never present).
fn u8 uset_has(Uset *set, u32 key) {
  if (key == 0) {
    return 0;
  }
  u64 word_idx = ((u64)key) >> 6;
  u64 bit_mask = 1ull << (key & 63u);
  if (word_idx >= set->word_count) {
    return 0;
  }
  u64 word = atomic_load_explicit(&set->words[word_idx], memory_order_relaxed);
  return (word & bit_mask) != 0;
}

// Insert key if missing; returns 1 if inserted, 0 if already present.
// For out-of-range keys: returns 1 (treat as new) to avoid skipping work.
fn u8 uset_add(Uset *set, u32 key) {
  if (key == 0) {
    return 0;
  }
  u64 word_idx = ((u64)key) >> 6;
  if (__builtin_expect(word_idx >= set->word_count, 0)) {
    return 1;
  }
  u64 bit_mask = 1ull << (key & 63u);
  u64 prev = atomic_fetch_or_explicit(&set->words[word_idx], bit_mask, memory_order_relaxed);
  return (prev & bit_mask) == 0;
}
