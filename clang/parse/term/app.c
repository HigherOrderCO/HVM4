fn Term parse_term(PState *s, u32 depth);
fn Term parse_term_atom(PState *s, u32 depth);
fn int parse_term_opr_peek(PState *s);
fn void parse_term_opr_consume(PState *s, int op);
fn int parse_term_opr_prec(int op);

fn Term parse_term_app_prec(Term f, PState *s, u32 depth, int min_prec);

fn Term parse_term_app(Term f, PState *s, u32 depth) {
  return parse_term_app_prec(f, s, depth, 0);
}

fn Term parse_term_app_prec(Term f, PState *s, u32 depth, int min_prec) {
  parse_skip(s);
  if (parse_match(s, "<>")) {
    Term t = parse_term(s, depth);
    Term a[2] = {f, t};
    return parse_term_app_prec(term_new_ctr(SYM_CON, 2, a), s, depth, min_prec);
  }
  // Structural equality: === (must check before == for numeric)
  if (parse_match(s, "===")) {
    Term rhs = parse_term_atom(s, depth);
    rhs = parse_term_app_prec(rhs, s, depth, 4);  // same precedence as ==
    return parse_term_app_prec(term_new_eql(f, rhs), s, depth, min_prec);
  }
  // Short-circuit AND: .&.
  if (parse_match(s, ".&.")) {
    Term rhs = parse_term_atom(s, depth);
    rhs = parse_term_app_prec(rhs, s, depth, 2);  // same precedence as &&
    return parse_term_app_prec(term_new_and(f, rhs), s, depth, min_prec);
  }
  // Short-circuit OR: .|.
  if (parse_match(s, ".|.")) {
    Term rhs = parse_term_atom(s, depth);
    rhs = parse_term_app_prec(rhs, s, depth, 1);  // same precedence as ||
    return parse_term_app_prec(term_new_or(f, rhs), s, depth, min_prec);
  }
  // Precedence climbing for infix operators
  int op = parse_term_opr_peek(s);
  if (op >= 0 && parse_term_opr_prec(op) >= min_prec) {
    parse_term_opr_consume(s, op);
    Term rhs = parse_term_atom(s, depth);
    // Parse higher-precedence ops on the right first
    rhs = parse_term_app_prec(rhs, s, depth, parse_term_opr_prec(op) + 1);
    f = term_new_op2(op, f, rhs);
    // Continue at same precedence level (left-associative)
    return parse_term_app_prec(f, s, depth, min_prec);
  }
  if (parse_peek(s) != '(') {
    return f;
  }
  parse_consume(s, "(");
  if (parse_peek(s) == ')') {
    parse_consume(s, ")");
    return parse_term_app_prec(f, s, depth, min_prec);
  }
  while (1) {
    Term arg = parse_term(s, depth);
    f = term_new_app(f, arg);
    parse_skip(s);
    parse_match(s, ",");  // optional comma
    parse_skip(s);
    if (parse_peek(s) == ')') {
      parse_consume(s, ")");
      break;
    }
  }
  return parse_term_app_prec(f, s, depth, min_prec);
}
