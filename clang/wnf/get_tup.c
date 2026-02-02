// ! (x₀,x₁) = (a,b)
// ------------------ GET-TUP
// x₀ ← a
// x₁ ← b
fn Term wnf_get_tup(u32 loc, u8 side, Term tup) {
  ITRS_INC("GET-TUP");
  u32 tup_loc = term_val(tup);
  Term tm0 = heap_read(tup_loc + 0);
  Term tm1 = heap_read(tup_loc + 1);
  return heap_subst_cop(side, loc, tm0, tm1);
}
