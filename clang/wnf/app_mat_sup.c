// (λ{#K:h; m} &L{a,b})
// -------------------- APP-MAT-SUP
// ! H &L = h
// ! M &L = m
// &L{(λ{#K:H₀; M₀} a)
//   ,(λ{#K:H₁; M₁} b)}
fn Term wnf_app_mat_sup(Term mat, Term sup) {
  ITRS_INC("APP-MAT-SUP");
  u32  lab = term_ext(sup);
  Copy M   = term_clone(lab, mat);
  u32  loc = term_val(sup);
  Term a   = heap_read(loc + 0);
  Term b   = heap_read(loc + 1);
  heap_free(loc, 2);
  return term_new_sup(lab, term_new_app(M.k0, a), term_new_app(M.k1, b));
}
