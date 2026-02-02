// ((a,b) === (c,d))
// ----------------- EQL-TUP
// (a === c) & (b === d)
fn Term wnf_eql_tup(Term a, Term b) {
  ITRS_INC("EQL-TUP");
  u32 a_loc = term_val(a);
  u32 b_loc = term_val(b);
  Term eq0 = term_new_eql(heap_read(a_loc + 0), heap_read(b_loc + 0));
  Term eq1 = term_new_eql(heap_read(a_loc + 1), heap_read(b_loc + 1));
  return term_new_and(eq0, eq1);
}
