// Parse Program Entry
// -------------------
// Parses one source buffer as a top-level HVM4 program.

fn void parse_def(PState *s);

// Seeds parser state for one source text and parses all top-level definitions.
fn void parse_program(const char *source_path, char *src) {
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
