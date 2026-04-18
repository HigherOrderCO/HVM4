fn Term parse_term(PState *s, u32 depth);

// Sup-fork: &L[x,y,&z]{A; B}  or  &(L)[x,y,&z]{A; B}
// Desugars to: !x &L = x; !y &L = y; !&z &L = z; &L{A'; B'}
// where A' uses x₀,y₀,z₀ and B' uses x₁,y₁,z₁
// Variables prefixed with & are cloned (can be used multiple times per branch).
fn Term parse_term_sup_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth) {
  // Save outer fork state (sup-forks can nest)
  int saved_fork_side = PARSE_FORK_SIDE;

  // Parse variable names: [x, y, &z]
  // Do this BEFORE resetting PARSE_FORK_SIDE so that variables from an
  // outer fork resolve to the correct projection (BJ0/BJ1).
  u32 names[16];
  u32 old_depths[16];
  u32 old_tags[16];
  u32 old_labs[16];
  u32 cloned[16];
  u32 n = 0;

  parse_skip(s);
  while (parse_peek(s) != ']') {
    parse_skip(s);
    if (n >= 16) {
      parse_error(s, "at most 16 sup-fork binders", parse_peek(s));
    }
    // Check for & prefix (cloned)
    cloned[n] = 0;
    if (parse_peek(s) == '&') {
      parse_advance(s);
      parse_skip(s);
      cloned[n] = 1;
    }
    names[n] = parse_name(s);
    parse_skip(s);

    // Look up the variable in the current scope.
    int skipped;
    PBind* bind = parse_bind_lookup(names[n], -1, &skipped);
    if (bind == NULL) {
      parse_error_var(s, names[n], 1, skipped);
    }
    old_depths[n] = bind->lvl;
    if (bind->forked && saved_fork_side >= 0) {
      old_tags[n] = (saved_fork_side == 0) ? BJ0 : BJ1;
      old_labs[n] = bind->lab;
    } else {
      old_tags[n] = BJV;
      old_labs[n] = 0;
    }
    n++;

    parse_skip(s);
    parse_match(s, ",");
  }
  parse_consume(s, "]");
  parse_skip(s);
  parse_consume(s, "{");
  parse_skip(s);

  // Now reset PARSE_FORK_SIDE for parsing the branches of THIS fork
  PARSE_FORK_SIDE = -1;

  // Push forked bindings for each variable.
  u32 d = dyn ? 2 : 1;
  for (u32 i = 0; i < n; i++) {
    parse_bind_push(names[i], depth + i * d, dyn ? PARSE_DYN_LAB : lab, 1, cloned[i]);
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
  parse_match(s, ";");
  parse_skip(s);

  // Optional &₁: before right branch
  if (parse_match(s, "&₁")) {
    parse_skip(s);
    parse_consume(s, ":");
  }
  PARSE_FORK_SIDE = 1;
  Term right = parse_term(s, body_depth);
  PARSE_FORK_SIDE = saved_fork_side;
  parse_skip(s);
  parse_match(s, ";");
  parse_consume(s, "}");

  // Pop forked bindings
  for (u32 i = 0; i < n; i++) {
    parse_bind_pop();
  }

  // Affine checks + auto-dup for each forked variable
  for (u32 i = 0; i < n; i++) {
    u32 lvl = depth + i * d + 1;
    u32 uses0, uses1;
    if (dyn) {
      uses0 = count_uses(left,  lvl,     BJV, 0);
      uses1 = count_uses(right, lvl + 1, BJV, 0);
    } else {
      uses0 = count_uses(left,  lvl, BJ0, lab);
      uses1 = count_uses(right, lvl, BJ1, lab);
    }
    if (cloned[i]) {
      // Auto-dup for cloned variables.
      // Use body_depth as base so that refs to other forked variables
      // (at levels between depth+1 and body_depth) don't get shifted.
      if (dyn) {
        left  = parse_auto_dup(left,  lvl,     body_depth, BJV, 0, uses0);
        right = parse_auto_dup(right, lvl + 1, body_depth, BJV, 0, uses1);
      } else {
        left  = parse_auto_dup(left,  lvl, body_depth, BJ0, lab, uses0);
        right = parse_auto_dup(right, lvl, body_depth, BJ1, lab, uses1);
      }
    } else {
      if (uses0 > 1) {
        parse_error_affine(s, names[i], 0, uses0);
      }
      if (uses1 > 1) {
        parse_error_affine(s, names[i], 1, uses1);
      }
    }
  }

  // Build body: SUP
  Term body;
  if (dyn) {
    body = term_new_dsu(lab_term, left, right);
  } else {
    body = term_new_sup(lab, left, right);
  }

  // Wrap with DUP chain (reverse order)
  for (int i = n - 1; i >= 0; i--) {
    Term var_ref = term_new(0, old_tags[i], old_labs[i], old_depths[i]);
    if (dyn) {
      u64 loc1 = heap_alloc(1);
      HEAP[loc1] = body;
      Term lam1 = term_new(0, LAM, depth + i * d + 2, loc1);
      u64 loc0 = heap_alloc(1);
      HEAP[loc0] = lam1;
      Term lam0 = term_new(0, LAM, depth + i * d + 1, loc0);
      body = term_new_ddu(lab_term, var_ref, lam0);
    } else {
      body = term_new_dup(lab, var_ref, body);
    }
  }

  return body;
}
