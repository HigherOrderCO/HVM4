// ! (x₀,x₁) = &L{a,b}
// ------------------- GET-SUP
// ! (A₀,A₁) = a
// ! (B₀,B₁) = b
// x₀ ← &L{A₀,B₀}
// x₁ ← &L{A₁,B₁}
fn Term wnf_get_sup(u32 loc, u8 side, Term sup) {
  ITRS_INC("GET-SUP");
  u32 sup_loc = term_val(sup);
  u32 lab     = term_ext(sup);
  Term a      = heap_read(sup_loc + 0);
  Term b      = heap_read(sup_loc + 1);
  u64  base   = heap_alloc(6);
  u32  at     = (u32)base;
  heap_set(at + 0, a);
  heap_set(at + 1, b);
  Copy A      = term_clone_get_at(at + 0);
  Copy B      = term_clone_get_at(at + 1);
  Term r0     = term_new_sup_at(at + 2, lab, A.k0, B.k0);
  Term r1     = term_new_sup_at(at + 4, lab, A.k1, B.k1);
  return heap_subst_cop(side, loc, r0, r1);
}
