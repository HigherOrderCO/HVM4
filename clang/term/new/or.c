// Or(a, b): short-circuit OR, strict on a only
// Layout: heap_read(loc+0) = a, heap_read(loc+1) = b
// Returns #1 if a is non-zero, b if a is zero
fn Term term_new_or_at(u64 loc, Term a, Term b) {
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  return term_new(0, OR, 0, loc);
}

fn Term term_new_or(Term a, Term b) {
  return term_new_or_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_OR), a, b);
}
