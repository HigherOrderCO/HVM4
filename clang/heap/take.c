fn Term heap_take(u64 loc) {
  if (__builtin_expect(THREAD_COUNT == 1, 1)) {
    Term term = HEAP[loc];
    if (__builtin_expect(term != 0, 1)) {
      return term;
    }
    fprintf(stderr, "ERROR: heap_take saw zero at %llu in single-threaded mode\n", (unsigned long long)loc);
    abort();
  }
  for (;;) {
    Term prev = __atomic_exchange_n(&HEAP[loc], 0, __ATOMIC_ACQUIRE);
    if (__builtin_expect(prev != 0, 1)) {
      return prev;
    }
    cpu_relax();
  }
}
