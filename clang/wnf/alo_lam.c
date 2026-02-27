// @{s} λx.f
// ------------ ALO-LAM
// x' ← fresh
// λx'.@{x',s}f
fn Term wnf_alo_lam(u64 alo_loc, u64 ls_loc, u32 len, Term book) {
  u32 lam_ext  = term_ext(book);
  u64 lam_body = term_val(book);
  u64 bind_loc = heap_alloc_kind(2, AOT_HEAP_KIND_WNF_ALO_LAM);
  u64 loc      = (len > 0) ? alo_loc : heap_alloc_kind(1, AOT_HEAP_KIND_WNF_ALO_LAM);
  Term alo     = term_new_alo_at(loc, bind_loc, len + 1, lam_body);
  heap_set(bind_loc + 0, alo);
  heap_set(bind_loc + 1, term_new(0, NUM, 0, ls_loc));
  return term_new(0, LAM, lam_ext, bind_loc + 0);
}
