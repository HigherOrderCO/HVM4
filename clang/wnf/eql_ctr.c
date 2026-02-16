// (#K{a0,a1...} === #K{b0,b1...})  (same tag)
// --------------------------------------- EQL-CTR-MAT
// For SUC (1n+): ↑(pred === pred)
// For CON (<>): ↑((head === head) & ↑(tail === tail))
// Others: (a0 === b0) & (a1 === b1) & ...
//
// (#K{...} === #L{...})  (different tag)
// ------------------------------------- EQL-CTR-MIS
// #0
fn Term wnf_eql_ctr(Term a, Term b) {
  ITRS_INC("EQL-CTR-MIS");
  u32 a_tag = term_tag(a);
  u32 b_tag = term_tag(b);
  u32 a_ext = term_ext(a);
  u32 b_ext = term_ext(b);

  // Different constructor tags or names -> #0
  if (a_tag != b_tag || a_ext != b_ext) {
    return term_new_num(0);
  }

  u32 arity = a_tag - C00;

  // Arity 0: equal
  if (arity == 0) {
    return term_new_num(1);
  }

  u32  a_loc = term_val(a);
  u32  b_loc = term_val(b);

  // SUC (1n+): recursive natural - wrap in INC for priority
  if (a_ext == SYM_SUC && arity == 1) {
    Term eq = term_new_eql(heap_read(a_loc), heap_read(b_loc));
    return term_new_inc(eq);
  }

  // CON (<>): recursive list - wrap tail and whole in INC
  if (a_ext == SYM_CON && arity == 2) {
    Term eq_h = term_new_eql(heap_read(a_loc), heap_read(b_loc));
    Term eq_t = term_new_inc(term_new_eql(heap_read(a_loc + 1), heap_read(b_loc + 1)));
    return term_new_inc(term_new_and(eq_h, eq_t));
  }

  // Other constructors: no INC, just AND chain
  Term result = term_new_eql(heap_read(a_loc), heap_read(b_loc));
  for (u32 i = 1; i < arity; i++) {
    Term eq_i = term_new_eql(heap_read(a_loc + i), heap_read(b_loc + i));
    result = term_new_and(result, eq_i);
  }
  return result;
}
