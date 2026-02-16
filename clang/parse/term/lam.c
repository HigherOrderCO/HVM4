fn Term parse_term(PState *s, u32 depth);
fn u32  parse_char_lit(PState *s);

fn Term parse_term_lam(PState *s, u32 depth) {
  parse_skip(s);
  // Era λ{}, Use λ{f} or Lambda-match λ{A:f; B:g; ...; i}
  if (parse_peek(s) == '{') {
    parse_consume(s, "{");
    parse_skip(s);
    if (parse_peek(s) == '}') {
      parse_consume(s, "}");
      return term_new_era();
    }
    Term  term = term_new_num(0);
    Term *tip  = &term;
    while (1) {
      parse_skip(s);
      u8  tag = 0;
      u32 ext = 0;
      PState save = *s;
      // Select the match case:
      if (parse_peek(s) == '\'') {
        u32 code = parse_char_lit(s);
        parse_skip(s);
        tag = SWI;
        ext = code;
      }
      else if (isdigit(parse_peek(s))) {
        while (isdigit(parse_peek(s))) {
          ext = ext * 10 + (parse_peek(s) - '0');
          parse_advance(s);
        }
        parse_skip(s);
        if (parse_peek(s) == ':') {
          tag = SWI;
        } else if (parse_peek(s) == 'n') {
          if (ext == 0 && parse_peek_at(s, 1) != '+') {
            parse_advance(s);
            tag = MAT;
            ext = SYM_ZER;
          } else if (ext == 1 && parse_peek_at(s, 1) == '+') {
            parse_advance(s);
            parse_advance(s);
            tag = MAT;
            ext = SYM_SUC;
          }
        }
      }
      else if (parse_peek(s) == '#') {
        parse_advance(s);
        tag = MAT;
        ext = parse_name(s);
      }
      else if (parse_peek(s) == '[' && parse_peek_at(s, 1) == ']') {
        parse_advance(s);
        parse_advance(s);
        tag = MAT;
        ext = SYM_NIL;
      }
      else if (parse_peek(s) == '<' && parse_peek_at(s, 1) == '>') {
        parse_advance(s);
        parse_advance(s);
        tag = MAT;
        ext = SYM_CON;
      }
      if (tag) {
        parse_skip(s);
        parse_consume(s, ":");
        Term val = parse_term(s, depth);
        parse_skip(s);
        parse_match(s, ";");
        u64 loc = heap_alloc(2);
        HEAP[loc + 0] = val;
        HEAP[loc + 1] = term_new_num(0);
        *tip = term_new(0, tag, ext, loc);
        tip  = &HEAP[loc + 1];
        continue;
      }
      // Not a match case, backtrack
      *s = save;

      // Use: λ{f}
      if (term == term_new_num(0)) {
        Term f = parse_term(s, depth);
        parse_skip(s);
        parse_consume(s, "}");
        return term_new_use(f);
      }
      // Match ending with no default
      if (parse_peek(s) == '}') {
        parse_consume(s, "}");
        return term;
      }
      // Match ending with default
      // Optional "_:" before default case
      if (parse_peek(s) == '_') {
        parse_advance(s);
        parse_skip(s);
        parse_consume(s, ":");
      }
      *tip = parse_term(s, depth);
      parse_skip(s);
      parse_consume(s, "}");
      return term;
    }
  }

  // Unscoped lambda: λ$x. body
  if (parse_peek(s) == '$') {
    parse_advance(s);  // consume '$'
    u32 nam = parse_name(s);
    parse_skip(s);

    // Bind unscoped var at depth+1 (reserve depth+1 for hidden f binder)
    parse_bind_push(nam, depth + 1, 0, 0, 0);
    Term body;
    if (parse_match(s, ",")) {
      body = parse_term_lam(s, depth + 2);
    } else {
      parse_consume(s, ".");
      body = parse_term(s, depth + 2);
    }
    parse_bind_pop();

    // Affine check for unscoped var
    u32 uses = count_uses(body, depth + 2, BJV, 0);
    if (uses > 1) {
      parse_error_affine(s, nam, -1, uses);
    }

    // Build: !${f,x}; f(body) with fresh f
    Term f_ref = term_new(0, BJV, 0, depth + 1);
    Term app   = term_new_app(f_ref, body);
    u64 loc_x  = heap_alloc(1);
    HEAP[loc_x] = app;
    u32 lam_x_ext = depth + 2;
    if (uses == 0) {
      lam_x_ext |= LAM_ERA_MASK;
    }
    Term lam_x = term_new(0, LAM, lam_x_ext, loc_x);
    u64 loc_f = heap_alloc(1);
    HEAP[loc_f] = lam_x;
    Term lam_f = term_new(0, LAM, depth + 1, loc_f);
    return term_new_uns(lam_f);
  }

  // Parse argument: [&]name[&[label|(label)]]
  u32 cloned = parse_match(s, "&");
  u32 nam    = parse_name(s);
  parse_skip(s);

  // Inline dup: λx&L or λx&(L) or λx&
  if (parse_peek(s) == '&') {
    parse_advance(s);
    parse_skip(s);
    int  dyn      = parse_peek(s) == '(';
    Term lab_term = 0;
    u32  lab      = 0;
    if (dyn) {
      parse_consume(s, "(");
      lab_term = parse_term(s, depth + 1);  // +1 because we're inside the outer lambda
      parse_consume(s, ")");
    } else {
      char c = parse_peek(s);
      if (c == ',' || c == '.') {
        lab = PARSE_FRESH_LAB++;
      } else {
        lab = parse_name_num(s);
      }
    }
    parse_skip(s);
    u32 d = dyn ? 3 : 2;
    PBind* bind = parse_bind_push(nam, depth + 1, dyn ? PARSE_DYN_LAB : lab, 0, cloned);
    Term body;
    if (parse_match(s, ",")) {
      body = parse_term_lam(s, depth + d);
    } else {
      parse_consume(s, ".");
      body = parse_term(s, depth + d);
    }
    parse_bind_pop();
    if (dyn) {
      //λx&(L) -> λx.!x&(L) = x;
      u32 uses0 = count_uses(body, depth + 2, BJV, 0);
      u32 uses1 = count_uses(body, depth + 3, BJV, 0);
      if (cloned) {
        body = parse_auto_dup(body, depth + 2, depth + 3, BJV, 0, uses0);
        body = parse_auto_dup(body, depth + 3, depth + 3, BJV, 0, uses1);
      }
      if (!cloned && uses0 > 1) {
        parse_error_affine(s, nam, 0, uses0);
      }
      if (!cloned && uses1 > 1) {
        parse_error_affine(s, nam, 1, uses1);
      }
      u64 loc1 = heap_alloc(1);
      HEAP[loc1] = body;
      u64 loc0 = heap_alloc(1);
      HEAP[loc0] = term_new(0, LAM, depth + 3, loc1);
      Term ddu = term_new_ddu(lab_term, term_new(0, BJV, 0, depth + 1), term_new(0, LAM, depth + 2, loc0));
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = ddu;
      u32 lam_ext = depth + 1;
      if (uses0 == 0 && uses1 == 0) {
        lam_ext |= LAM_ERA_MASK;
      }
      return term_new(0, LAM, lam_ext, lam_loc);
    } else {
      //λx&L
      u32 uses0 = count_uses(body, depth + 2, BJ0, lab);
      u32 uses1 = count_uses(body, depth + 2, BJ1, lab);
      if (cloned) {
        body = parse_auto_dup(body, depth + 2, depth + 2, BJ1, lab, uses1);
        body = parse_auto_dup(body, depth + 2, depth + 2, BJ0, lab, uses0);
      }
      if (!cloned && uses0 > 1) {
        parse_error_affine(s, nam, 0, uses0);
      }
      if (!cloned && uses1 > 1) {
        parse_error_affine(s, nam, 1, uses1);
      }
      u64 dup_term_loc = heap_alloc(2);
      HEAP[dup_term_loc + 0] = term_new(0, BJV, 0, depth + 1);
      HEAP[dup_term_loc + 1] = body;
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = term_new(0, DUP, lab, dup_term_loc);
      u32 lam_ext = depth + 1;
      if (uses0 == 0 && uses1 == 0) {
        lam_ext |= LAM_ERA_MASK;
      }
      return term_new(0, LAM, lam_ext, lam_loc);
    }
  }

  // Simple single arg (with comma recursion for cloned/complex args)
  PBind* bind = parse_bind_push(nam, depth, 0, 0, cloned);
  Term body;
  if (parse_match(s, ",")) {
    body = parse_term_lam(s, depth + 1);
  } else {
    parse_consume(s, ".");
    body = parse_term(s, depth + 1);
  }
  parse_bind_pop();
  u32 uses = count_uses(body, depth + 1, BJV, 0);
  if (cloned) {
    body = parse_auto_dup(body, depth + 1, depth + 1, BJV, 0, uses);
  }
  if (!cloned && uses > 1) {
    parse_error_affine(s, nam, -1, uses);
  }
  u32 lam_ext = depth + 1;
  if (uses == 0) {
    lam_ext |= LAM_ERA_MASK;
  }
  u64 loc = heap_alloc(1);
  HEAP[loc] = body;
  return term_new(0, LAM, lam_ext, loc);
}
