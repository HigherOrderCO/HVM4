fn void parse_error_var(PState *s, u32 nam, int is_dup, int skipped) {
  char  nam_fallback[32];
  char *nam_buf = table_get(nam);
  if (nam_buf == NULL) {
    snprintf(nam_fallback, sizeof(nam_fallback), "#%u", nam);
    nam_buf = nam_fallback;
  }
  fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m (%s:%d:%d)\n", s->file, s->line, s->col);
  if (is_dup && skipped) {
    fprintf(stderr, "- dup variable '%s' requires subscript ₀ or ₁\n", nam_buf);
  } else if (!is_dup && skipped) {
    fprintf(stderr, "- non-dup variable '%s' must be used without subscript (₀ or ₁)\n", nam_buf);
  } else {
    fprintf(stderr, "- undefined variable '%s'\n", nam_buf);
  }
  exit(1);
}
