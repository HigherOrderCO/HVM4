fn Term term_new_get_at(u32 loc, Term val, Term bod) {
  return term_new_at(loc, GET, 0, 2, (Term[]){val, bod});
}

fn Term term_new_get(Term val, Term bod) {
  return term_new_get_at(heap_alloc(2), val, bod);
}
