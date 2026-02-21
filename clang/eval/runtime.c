// Runtime Driver Helpers
// ----------------------
// Shared boot/parse helpers used by the CLI and generated AOT programs.

fn void parse_def(PState *s);

// Initializes runtime globals and evaluator flags.
fn void eval_runtime_init(u32 threads, int debug, int silent, int steps_enable) {
  thread_set_count(threads);
  wnf_set_tid(0);

  BOOK       = calloc(BOOK_CAP, sizeof(u64));
  HEAP       = calloc(HEAP_CAP, sizeof(Term));
  TABLE.data = calloc(BOOK_CAP, sizeof(char*));

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

// Parses source text as if it came from `source_path`.
fn void eval_parse_source(const char *source_path, char *src) {
  const char *path = source_path != NULL ? source_path : "<source>";
  char *file = strdup(path);
  if (file == NULL) {
    sys_error("Source path allocation failed");
  }

  if (PARSE_SEEN_FILES_LEN >= 1024) {
    free(file);
    sys_error("MAX_INCLUDES");
  }
  PARSE_SEEN_FILES[PARSE_SEEN_FILES_LEN++] = file;

  PState s = {
    .file = file,
    .src  = src,
    .pos  = 0,
    .len  = strlen(src),
    .line = 1,
    .col  = 1
  };
  parse_def(&s);
}

// Checks that static locations fit the 24-bit ALO term field.
fn int eval_check_alo_space(void) {
  if (HEAP_NEXT_AT(0) <= (ALO_TM_MASK + 1)) {
    return 1;
  }

  fprintf(stderr, "Error: static book exceeds 24-bit location space (%llu words used)\n", HEAP_NEXT_AT(0));
  return 0;
}

// Resolves @main id; returns 1 when defined, 0 otherwise.
fn int eval_get_main_id(u32 *out_main_id) {
  u32 main_id = table_find("main", 4);
  if (BOOK[main_id] == 0) {
    return 0;
  }

  *out_main_id = main_id;
  return 1;
}

// Frees runtime-global allocations for the current process run.
fn void eval_runtime_free(void) {
  free(HEAP);
  free(BOOK);
  free(TABLE.data);
  wnf_stack_free();
}
