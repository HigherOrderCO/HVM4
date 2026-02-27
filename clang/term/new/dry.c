fn Term term_new_dry_at(u64 loc, Term fun, Term arg) {
  return term_new_at(loc, DRY, 0, 2, (Term[]){fun, arg});
}

fn Term term_new_dry(Term fun, Term arg) {
  return term_new_dry_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_DRY), fun, arg);
}
