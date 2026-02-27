fn Term term_new_app_at(u64 loc, Term fun, Term arg) {
  return term_new_at(loc, APP, 0, 2, (Term[]){fun, arg});
}

fn Term term_new_app(Term fun, Term arg) {
  return term_new_app_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_APP), fun, arg);
}
