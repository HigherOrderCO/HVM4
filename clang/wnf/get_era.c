// ! (x₀,x₁) = &{}
// ------------- GET-ERA
// &{}
fn Term wnf_get_era(u32 loc) {
  ITRS_INC("GET-ERA");
  Term era = term_new_era();
  heap_subst_var(loc, era);
  return era;
}
