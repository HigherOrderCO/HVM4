// Eql(a, b): structural equality, strict on a first then b
// Layout: heap_read(loc+0) = a, heap_read(loc+1) = b
// Returns #1 if equal, #0 if not
fn Term term_new_eql_at(u64 loc, Term a, Term b) {
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  return term_new(0, EQL, 0, loc);
}

fn Term term_new_eql(Term a, Term b) {
  return term_new_eql_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_EQL), a, b);
}
