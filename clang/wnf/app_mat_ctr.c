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
  u32 mat_loc = term_val(mat);
  if (mat_ext == ctr_ext) {
    ITRS_INC("APP-MAT-CTR-MAT");
    u32 ari = term_tag(ctr) - C00;
    Term res = heap_read(mat_loc);
    if (ari == 0) {
      heap_free(mat_loc, 2);
      return res;
    }
    u32 ctr_loc = term_val(ctr);
    u64 apps = heap_alloc(2 * (u64)ari);
    for (u32 i = 0; i < ari; i++) {
      res = term_new_app_at((u32)(apps + 2 * (u64)i), res, heap_read(ctr_loc + i));
    }
    heap_free(mat_loc, 2);
    heap_free(ctr_loc, ari);
    return res;
  } else {
    ITRS_INC("APP-MAT-CTR-MIS");
    Term g = heap_read(mat_loc + 1);
    return term_new_app_at(mat_loc, g, ctr);
  }
}
