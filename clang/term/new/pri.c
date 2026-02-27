fn Term term_new_pri_at(u64 loc, u32 prim, u32 ari, Term *args) {
  return term_new_at(loc, PRI, prim, ari, args);
}

fn Term term_new_pri(u32 prim, u32 ari, Term *args) {
  return term_new_pri_at(heap_alloc_kind(ari, AOT_HEAP_KIND_TERM_PRI), prim, ari, args);
}
