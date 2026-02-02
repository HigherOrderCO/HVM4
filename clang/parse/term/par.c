fn Term parse_term(PState *s, u32 depth);

fn Term parse_term_par(PState *s, u32 depth) {
  Term term = parse_term(s, depth);
  parse_skip(s);
  if (parse_match(s, ",")) {
    Term term1 = parse_term(s, depth);
    parse_skip(s);
    parse_consume(s, ")");
    return term_new_tup(term, term1);
  }
  parse_consume(s, ")");
  return term;
}
