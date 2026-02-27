// USE: λ{f} - reduce arg and apply
// fields = [f]
fn Term term_new_use_at(u64 loc, Term f) {
  heap_set(loc, f);
  return term_new(0, USE, 0, loc);
}

fn Term term_new_use(Term f) {
  return term_new_use_at(heap_alloc_kind(1, AOT_HEAP_KIND_TERM_USE), f);
}
