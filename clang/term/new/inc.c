// INC: ↑x - priority wrapper for collapse ordering
// fields = [x]
fn Term term_new_inc_at(u32 loc, Term x) {
  heap_set(loc, x);
  return term_new(0, INC, 0, loc);
}

fn Term term_new_inc(Term x) {
  return term_new_inc_at(heap_alloc(1), x);
}
