fn Term term_new_alo(u64 ls_loc, u32 len, u64 tm_loc) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }
  u64 alo_loc = heap_alloc_kind(1, AOT_HEAP_KIND_TERM_ALO);
  heap_set(alo_loc, ((ls_loc & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK));
  return term_new(0, ALO, len, alo_loc);
}

fn Term term_new_alo_at(u64 alo_loc, u64 ls_loc, u32 len, u64 tm_loc) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }
  heap_set(alo_loc, ((ls_loc & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK));
  return term_new(0, ALO, len, alo_loc);
}
