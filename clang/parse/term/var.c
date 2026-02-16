fn Term parse_term_var(PState *s, u32 depth) {
  parse_skip(s);
  u32 nam = parse_name(s);
  parse_skip(s);
  int side = parse_match(s, "₀") ? 0 : parse_match(s, "₁") ? 1 : -1;
  parse_skip(s);
  int skipped;
  PBind* bind = parse_bind_lookup(nam, side, &skipped);
  if (bind == NULL) {
    parse_error_var(s, nam, side == -1, skipped);
  }
  if (side == -1 && bind->forked) {
    side = PARSE_FORK_SIDE;
  }
  // Handle dynamic dup binding (lab=PARSE_DYN_LAB marker)
  // For dynamic dup, X₀ and X₁ become BJV references to nested lambdas
  if (bind->lab == PARSE_DYN_LAB) {
    u32 offset = (side == 1) ? 1 : 0;
    return term_new(0, BJV, 0, (u32)bind->lvl + offset);
  }
  u32 val = (u32)bind->lvl;
  u32 lab = bind->lab;
  u8  tag = (side == 0) ? BJ0 : (side == 1) ? BJ1 : BJV;
  return term_new(0, tag, lab, val);
}
