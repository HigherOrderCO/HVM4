fn Term term_new_tup_at(u32 loc, Term tm0, Term tm1) {
  return term_new_at(loc, TUP, 0, 2, (Term[]){tm0, tm1});
}

fn Term term_new_tup(Term tm0, Term tm1) {
  return term_new_tup_at(heap_alloc(2), tm0, tm1);
}
