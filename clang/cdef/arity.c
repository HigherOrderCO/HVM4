fn u32 cdef_arity(u32 fid) {
  if (fid >= CDEF_CAP || BOOK[fid] == 0) {
    return 0;
  }

  u32  arity = 0;
  u32  loc   = BOOK[fid];
  Term term  = heap_read(loc);
  while (term_tag(term) == LAM) {
    arity++;
    loc = term_val(term);
    term = heap_read(loc + 0);
  }
  return arity;
}
