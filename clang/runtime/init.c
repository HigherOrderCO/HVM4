// Runtime Session Init
// --------------------
// Initializes process-global state for one program execution.

// Initializes runtime globals and evaluator flags.
fn void runtime_init(u32 threads, int debug, int silent, int steps_enable) {
  thread_set_count(threads);
  wnf_set_tid(0);

  BOOK       = calloc(BOOK_CAP, sizeof(u64));
  HEAP       = calloc(HEAP_CAP, sizeof(Term));
  TABLE.data = calloc(BOOK_CAP, sizeof(char *));

  if (!BOOK || !HEAP || !TABLE.data) {
    sys_error("Memory allocation failed");
  }

  heap_init_slices();
  symbols_init();
  prim_init();

  DEBUG = debug;
  SILENT = silent;
  STEPS_ENABLE = steps_enable;
}
