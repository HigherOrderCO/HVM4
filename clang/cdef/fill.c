fn u32 cdef_fill_aris(void) {
  u32 defs = 0;
  for (u32 fid = 0; fid < CDEF_CAP; fid++) {
    if (BOOK[fid] == 0) {
      continue;
    }
    CDEF_ARIS[fid] = cdef_arity(fid);
    defs++;
  }
  return defs;
}
