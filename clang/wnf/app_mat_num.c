// (λ{#a:h; m} #a)
// --------------- APP-MAT-NUM-MAT
// h
//
// (λ{#a:h; m} #b)
// --------------- APP-MAT-NUM-MIS
// (m #b)
fn Term wnf_app_mat_num(Term mat, Term num) {
  u32 mat_loc = term_val(mat);
  u32 mat_ext = term_ext(mat);
  u32 num_val = term_val(num);
  if (mat_ext == num_val) {
    ITRS_INC("APP-MAT-NUM-MAT");
    Term res = heap_read(mat_loc + 0);
    heap_free(mat_loc, 2);
    return res;
  } else {
    ITRS_INC("APP-MAT-NUM-MIS");
    Term g = heap_read(mat_loc + 1);
    return term_new_app_at(mat_loc, g, num);
  }
}
