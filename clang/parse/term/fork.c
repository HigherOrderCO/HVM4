fn Term parse_term(PState *s, u32 depth);

// Fork: &Lλx,y,z{A;B} or &(L)λx,y,z{A;B}
// Desugars to: λx&L.λy&L.λz&L.&L{A';B'}
// where A' uses x₀,y₀,z₀ and B' uses x₁,y₁,z₁
fn Term parse_term_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth) {
  if (PARSE_FORK_SIDE != -1) {
    fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m (%s:%d:%d)\n", s->file, s->line, s->col);
    fprintf(stderr, "- nested forks are not allowed\n");
    exit(1);
  }
  u32 names[16];
  u32 n = 0;
  names[n++] = parse_name(s);
  parse_skip(s);
  while (parse_peek(s) != '{') {
    parse_match(s, ",");  // optional comma between names
    parse_skip(s);
    if (parse_peek(s) == '{') break;
    names[n++] = parse_name(s);
    parse_skip(s);
  }
  parse_consume(s, "{");
  u32 d = dyn ? 3 : 2;
  for (u32 i = 0; i < n; i++) {
    parse_bind_push(names[i], depth + i * d + 1, dyn ? PARSE_DYN_LAB : lab, 1, 0);
  }
  u32 body_depth = depth + n * d;
  // Optional &₀: before left branch
  if (parse_match(s, "&₀")) {
    parse_skip(s);
    parse_consume(s, ":");
  }
  PARSE_FORK_SIDE = 0;
  Term left = parse_term(s, body_depth);
  parse_skip(s);
  parse_match(s, ";");  // optional semicolon between branches
  parse_skip(s);
  // Optional &₁: before right branch
  if (parse_match(s, "&₁")) {
    parse_skip(s);
    parse_consume(s, ":");
  }
  PARSE_FORK_SIDE = 1;
  Term right = parse_term(s, body_depth);
  PARSE_FORK_SIDE = -1;
  parse_skip(s);
  parse_match(s, ";");  // optional trailing semicolon
  parse_consume(s, "}");
  for (u32 i = 0; i < n; i++) {
    parse_bind_pop();
  }
  // Check affine for each forked variable
  for (u32 i = 0; i < n; i++) {
    u32 lvl = depth + i * d + 1;
    u32 uses0, uses1;
    if (dyn) {
      uses0 = count_uses(left, lvl, BJV, 0);
      uses1 = count_uses(right, lvl + 1, BJV, 0);
    } else {
      uses0 = count_uses(left, lvl, BJ0, lab);
      uses1 = count_uses(right, lvl, BJ1, lab);
    }
    if (uses0 > 1) {
      parse_error_affine(s, names[i], 0, uses0);
    }
    if (uses1 > 1) {
      parse_error_affine(s, names[i], 1, uses1);
    }
  }
  // Build body: DSU or SUP
  Term body;
  if (dyn) {
    body = term_new_dsu(lab_term, left, right);
  } else {
    body = term_new_sup(lab, left, right);
  }
  // Wrap with λx&L or λx&(L) for each arg (reverse order)
  for (int i = n - 1; i >= 0; i--) {
    u32 dd = depth + i * d;
    if (dyn) {
      u64 loc1 = heap_alloc(1);
      HEAP[loc1] = body;
      u64 loc0 = heap_alloc(1);
      HEAP[loc0] = term_new(0, LAM, dd + 3, loc1);
      Term ddu = term_new_ddu(lab_term, term_new(0, BJV, 0, dd + 1), term_new(0, LAM, dd + 2, loc0));
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = ddu;
      body = term_new(0, LAM, dd + 1, lam_loc);
    } else {
      Term dup = term_new_dup(lab, term_new(0, BJV, 0, dd + 1), body);
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = dup;
      body = term_new(0, LAM, dd + 1, lam_loc);
    }
  }
  return body;
}
