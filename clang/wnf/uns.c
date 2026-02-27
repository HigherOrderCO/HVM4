// ! ${f, v}; t
// ------------- WNF UNS
// t(λy.λ$x.y, $x)
fn Term wnf_uns(Term uns) {
  ITRS_INC("WNF-UNS");
  u64  uns_loc = term_val(uns);
  Term bod     = heap_read(uns_loc + 0);
  u64  loc     = heap_alloc_kind(2, AOT_HEAP_KIND_WNF_UNS);
  Term x_var   = term_new_var(loc + 0);
  Term y_var   = term_new_var(loc + 1);
  Term x_lam   = term_new_lam_at(loc + 0, y_var);
  Term y_lam   = term_new_lam_at(loc + 1, x_lam);
  return term_new_app(term_new_app(bod, y_lam), x_var);
}
