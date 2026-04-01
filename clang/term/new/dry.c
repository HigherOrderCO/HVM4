fn Term term_new_dry_at(u64 loc, Term fun, Term arg) {
  heap_set(loc + 0, fun);
  heap_set(loc + 1, arg);
  return term_new(0, DRY, 0, loc);
}

fn Term term_new_dry(Term fun, Term arg) {
  return term_new_dry_at(heap_alloc(2), fun, arg);
}
