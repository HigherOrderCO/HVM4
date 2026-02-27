// INC: ↑x - priority wrapper for collapse ordering
// fields = [x]
fn Term term_new_inc(Term x) {
  u64 loc = heap_alloc_kind(1, AOT_HEAP_KIND_TERM_INC);
  heap_set(loc, x);
  return term_new(0, INC, 0, loc);
}
