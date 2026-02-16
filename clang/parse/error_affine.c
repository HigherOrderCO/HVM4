fn void parse_error_affine(PState *s, u32 nam, u32 side, u32 uses) {
  char  nam_fallback[32];
  char *nam_buf = table_get(nam);
  if (nam_buf == NULL) {
    snprintf(nam_fallback, sizeof(nam_fallback), "#%u", nam);
    nam_buf = nam_fallback;
  }
  fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m (%s:%d:%d)\n", s->file, s->line, s->col);
  fprintf(stderr, "- variable '%s%s' used %d times\n", nam_buf, side == 0 ? "₀" : side == 1 ? "₁" : "", uses);
  fprintf(stderr, "- hint: declare variable as '&%s' to allow multiple uses\n", nam_buf);
  exit(1);
}
