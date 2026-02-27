// UNS: ! ${f, v}; body - unscoped binding
// fields = [body]
fn Term term_new_uns(Term bod) {
  u64 loc = heap_alloc_kind(1, AOT_HEAP_KIND_TERM_UNS);
  heap_set(loc, bod);
  return term_new(0, UNS, 0, loc);
}
