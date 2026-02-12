fn Term cdef_apply(u32 fid, Term *args, u32 argc) {
  if (fid >= CDEF_CAP || BOOK[fid] == 0) {
    return term_new_ref(fid);
  }

  u64 alo_loc = heap_alloc(1);
  heap_set(alo_loc, (u64)BOOK[fid]);

  Term term = term_new(0, ALO, 0, alo_loc);
  for (u32 i = 0; i < argc; i++) {
    term = term_new_app(term, args[i]);
  }

  return term;
}
