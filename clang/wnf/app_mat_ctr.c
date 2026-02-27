// (λ{#K:h; m} #K{a,b})
// -------------------- APP-MAT-CTR-MAT
// (h a b)
//
// (λ{#K:h; m} #L{a,b})
// -------------------- APP-MAT-CTR-MIS
// (m #L{a,b})
fn Term wnf_app_mat_ctr(Term mat, Term ctr) {
  u32 mat_ext = term_ext(mat);
  u32 ctr_ext = term_ext(ctr);
  u64 mat_loc = term_val(mat);
  if (mat_ext == ctr_ext) {
    ITRS_INC("APP-MAT-CTR-MAT");
    u32 ari = term_tag(ctr) - C00;
    Term res = heap_read(mat_loc);
    if (ari == 0) {
      return res;
    }
    u64 ctr_loc = term_val(ctr);
    // Reuse MAT node storage for the first APP in the chain.
    Term arg0 = heap_read(ctr_loc + 0);
    res = term_new_app_at(mat_loc, res, arg0);
    if (ari == 1) {
      return res;
    }
    // Reuse CTR node storage for the second APP.
    Term arg1 = heap_read(ctr_loc + 1);
    res = term_new_app_at(ctr_loc, res, arg1);
    if (ari == 2) {
      return res;
    }
    u64 apps = heap_alloc_kind(2 * (u64)(ari - 2), AOT_HEAP_KIND_WNF_APP_MAT);
    for (u32 i = 2; i < ari; i++) {
      res = term_new_app_at(apps + 2 * (u64)(i - 2), res, heap_read(ctr_loc + i));
    }
    return res;
  } else {
    ITRS_INC("APP-MAT-CTR-MIS");
    Term g = heap_read(mat_loc + 1);
    return term_new_app_at(mat_loc, g, ctr);
  }
}
