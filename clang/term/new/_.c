fn Term term_new_at(u64 loc, u8 tag, u32 ext, u32 ari, Term *args) {
  for (u32 i = 0; i < ari; i++) {
    heap_set(loc + i, args[i]);
  }
  return term_new(0, tag, ext, loc);
}

fn Term term_new_(u8 tag, u32 ext, u32 ari, Term *args) {
  return term_new_at(heap_alloc_kind(ari, AOT_HEAP_KIND_TERM_GENERIC), tag, ext, ari, args);
}
