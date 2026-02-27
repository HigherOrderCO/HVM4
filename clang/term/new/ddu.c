// DynDup(lab, val, bod): dynamic DUP binder, strict on lab
// Layout: heap_read(loc+0) = lab, heap_read(loc+1) = val, heap_read(loc+2) = bod
fn Term term_new_ddu(Term lab, Term val, Term bod) {
  u64 loc = heap_alloc_kind(3, AOT_HEAP_KIND_TERM_DDU);
  heap_set(loc + 0, lab);
  heap_set(loc + 1, val);
  heap_set(loc + 2, bod);
  return term_new(0, DDU, 0, loc);
}
