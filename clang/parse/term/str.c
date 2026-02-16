fn Term parse_term(PState *s, u32 depth);
fn u32  parse_char_esc(PState *s);

fn Term parse_term_str(PState *s, u32 depth) {
  char fence = parse_peek(s);
  parse_advance(s);
  Term t = term_new_ctr(SYM_NIL, 0, 0);
  u32 cs[4096]; u32 n = 0;
  while (parse_peek(s) != fence) {
    u32 c = parse_char_esc(s);
    cs[n++] = c;
  }
  parse_advance(s);
  for (int i = n - 1; i >= 0; i--) {
    Term c = term_new_num(cs[i]);
    Term h = term_new_ctr(SYM_CHR, 1, &c);
    Term a[2] = {h, t};
    t = term_new_ctr(SYM_CON, 2, a);
  }
  return t;
}
