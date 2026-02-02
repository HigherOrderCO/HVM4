// ! X &L = (a,b)
// --------------- DUP-TUP
// ! A &L = a
// ! B &L = b
// X₀ ← (A₀,B₀)
// X₁ ← (A₁,B₁)
fn Term wnf_dup_tup(u32 lab, u32 loc, u8 side, Term tup) {
  ITRS_INC("DUP-TUP");
  u32 t_loc = term_val(tup);
  Term a = heap_read(t_loc + 0);
  Term b = heap_read(t_loc + 1);
  Copy A = term_clone(lab, a);
  Copy B = term_clone(lab, b);
  u64 base = heap_alloc(4);
  u32 at = (u32)base;
  Term r0 = term_new_tup_at(at + 0, A.k0, B.k0);
  Term r1 = term_new_tup_at(at + 2, A.k1, B.k1);
  return heap_subst_cop(side, loc, r0, r1);
}
