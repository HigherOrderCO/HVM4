// ! X &L = &R{a,b}
// ---------------- DUP-SUP
// if L == R:
//   X₀ ← a
//   X₁ ← b
// else:
//   ! A &L = a
//   ! B &L = b
//   X₀ ← &R{A₀,B₀}
//   X₁ ← &R{A₁,B₁}
fn Term wnf_dup_sup(u32 lab, u64 loc, u8 side, Term sup) {
  ITRS_INC("DUP-SUP");
  u64 sup_loc = term_val(sup);
  u32 sup_lab = term_ext(sup);
  if (lab == sup_lab) {
    Term tm0 = heap_read(sup_loc + 0);
    Term tm1 = heap_read(sup_loc + 1);
    return heap_subst_cop(side, loc, tm0, tm1);
  } else {
    u64 base = heap_alloc_kind(4, AOT_HEAP_KIND_WNF_DUP_SUP);
    u64 at   = base;
    Copy A  = term_clone_at(sup_loc + 0, lab);
    Copy B  = term_clone_at(sup_loc + 1, lab);
    Term s0 = term_new_sup_at(at + 0, sup_lab, A.k0, B.k0);
    Term s1 = term_new_sup_at(at + 2, sup_lab, A.k1, B.k1);
    return heap_subst_cop(side, loc, s0, s1);
  }
}
