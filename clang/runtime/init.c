// Runtime Session Init
// --------------------
// Initializes process-global state for one program execution.

// Initializes runtime globals and evaluator flags.
fn void runtime_init(u32 threads, int debug, int silent, int steps_enable) {
  thread_set_count(threads);
  wnf_set_tid(0);

  BOOK       = calloc(BOOK_CAP, sizeof(u64));
  HEAP       = NULL;
  TABLE.data = calloc(BOOK_CAP, sizeof(char *));

  if (HEAP_CAP <= ((u64)SIZE_MAX / sizeof(Term))) {
    size_t heap_bytes = (size_t)(HEAP_CAP * sizeof(Term));
#if HVM_WINDOWS
    HEAP = VirtualAlloc(NULL, heap_bytes, MEM_RESERVE, PAGE_READWRITE);
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    u64 page_bytes = (u64)sys_info.dwPageSize;
    if (page_bytes == 0) {
      page_bytes = 4096;
    }
    HEAP_PAGE_WORDS = page_bytes / sizeof(Term);
    if (HEAP_PAGE_WORDS == 0) {
      HEAP_PAGE_WORDS = 1;
    }
    u64 step_bytes = 2ULL << 20; // 2 MiB commit chunks.
    if (step_bytes < page_bytes) {
      step_bytes = page_bytes;
    }
    HEAP_COMMIT_STEP_WORDS = step_bytes / sizeof(Term);
    if (HEAP_COMMIT_STEP_WORDS == 0) {
      HEAP_COMMIT_STEP_WORDS = HEAP_PAGE_WORDS;
    }
    if (HEAP_COMMIT_STEP_WORDS % HEAP_PAGE_WORDS != 0) {
      HEAP_COMMIT_STEP_WORDS =
          ((HEAP_COMMIT_STEP_WORDS + HEAP_PAGE_WORDS - 1) / HEAP_PAGE_WORDS) * HEAP_PAGE_WORDS;
    }
#else
    void *heap_map = sys_mmap_anon(heap_bytes);
    if (heap_map != NULL) {
      HEAP = (Term *)heap_map;
    }
#endif
  }

  if (!BOOK || !HEAP || !TABLE.data) {
    sys_error("Memory allocation failed");
  }

  heap_init_slices();
  symbols_init();
  prim_init();

  DEBUG        = debug;
  SILENT       = silent;
  STEPS_ENABLE = steps_enable;
}
