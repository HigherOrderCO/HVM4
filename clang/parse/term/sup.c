fn Term parse_term(PState *s, u32 depth);
fn Term parse_term_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth);

fn Term parse_term_sup(PState *s, u32 depth) {
  parse_skip(s);
  if (parse_peek(s) == '{') {
    parse_consume(s, "{");
    parse_consume(s, "}");
    return term_new_era();
  }
  int  dyn      = parse_peek(s) == '(';
  Term lab_term = 0;
  u32  lab      = 0;
  if (dyn) {
    parse_consume(s, "(");
    lab_term = parse_term(s, depth);
    parse_consume(s, ")");
  } else {
    lab = parse_name_num(s);
  }
  parse_skip(s);
  // Fork: &Lλx,y,z{A;B} or &(L)λx,y,z{A;B}
  if (parse_match(s, "λ")) {
    return parse_term_fork(s, dyn, lab_term, lab, depth);
  }
  // Regular sup: &L{A,B} or &(L){A,B} or &L{A B}
  parse_consume(s, "{");
  Term tm0 = parse_term(s, depth);
  parse_skip(s);
  parse_match(s, ",");  // optional comma
  Term tm1 = parse_term(s, depth);
  parse_skip(s);
  parse_match(s, ",");  // optional trailing comma
  parse_consume(s, "}");
  return dyn ? term_new_dsu(lab_term, tm0, tm1) : term_new_sup(lab, tm0, tm1);
}
