// ! (x₀,x₁) = ↑t
// -------------- GET-INC
// x₀ ← ↑t₀
// x₁ ← ↑t₁
fn Term wnf_get_inc(u32 loc, u8 side, Term inc) {
  ITRS_INC("GET-INC");
  u32  inc_loc = term_val(inc);
  Term t       = heap_read(inc_loc);
  u64  base    = heap_alloc(3);
  u32  at      = (u32)base;
  heap_set(at + 0, t);
  Copy T       = term_clone_get_at(at + 0);
  Term r0      = term_new_inc_at(at + 1, T.k0);
  Term r1      = term_new_inc_at(at + 2, T.k1);
  return heap_subst_cop(side, loc, r0, r1);
}
