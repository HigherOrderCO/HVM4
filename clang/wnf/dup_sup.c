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
fn Term wnf_dup_sup(u32 lab, u32 loc, u8 side, Term sup) {
  ITRS_INC("DUP-SUP");
  u32 sup_loc = term_val(sup);
  u32 sup_lab = term_ext(sup);
  if (lab == sup_lab) {
    Term tm0 = heap_read(sup_loc + 0);
    Term tm1 = heap_read(sup_loc + 1);
    heap_free(sup_loc, 2);
    return heap_subst_cop(side, loc, tm0, tm1);
  } else {
    u64 base = heap_alloc(6);
    u32 at   = (u32)base;
    heap_set(at + 0, heap_read(sup_loc + 0));
    heap_set(at + 1, heap_read(sup_loc + 1));
    heap_free(sup_loc, 2);
    Copy A  = term_clone_at(at + 0, lab);
    Copy B  = term_clone_at(at + 1, lab);
    Term s0 = term_new_sup_at(at + 2, sup_lab, A.k0, B.k0);
    Term s1 = term_new_sup_at(at + 4, sup_lab, A.k1, B.k1);
    return heap_subst_cop(side, loc, s0, s1);
  }
}
