fn Term parse_term(PState *s, u32 depth);

fn Term parse_term_nat(PState *s, u32 depth) {
  u32 sav = s->pos;
  u32 num = 0;
  while (isdigit(parse_peek(s))) {
    num = num * 10 + (parse_peek(s) - '0');
    parse_advance(s);
  }
  if (parse_peek(s) != 'n') { s->pos = sav; return 0; }
  parse_advance(s);
  Term t = parse_match(s, "+") ? parse_term(s, depth) : term_new_ctr(SYM_ZER, 0, 0);
  for (u32 i = 0; i < num; i++) t = term_new_ctr(SYM_SUC, 1, &t);
  return t;
}
