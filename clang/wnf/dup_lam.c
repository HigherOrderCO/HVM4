// ! F &L = λx.f
// ---------------- DUP-LAM
// F₀ ← λ$x0.G₀
// F₁ ← λ$x1.G₁
// x  ← &L{$x0,$x1}
// ! G &L = f
fn Term wnf_dup_lam(u32 lab, u32 loc, u8 side, Term lam) {
  ITRS_INC("DUP-LAM");
  u32  lam_loc        = term_val(lam);
  u32  lam_ext        = term_ext(lam);
  Term bod            = heap_read(lam_loc);

  if (lam_ext & LAM_ERA_MASK) {
    heap_free(lam_loc, 1);
    u64  a      = heap_alloc(3);
    heap_set(a + 2, bod);
    Copy B      = term_clone_at(a + 2, lab);
    heap_set(a + 0, B.k0);
    heap_set(a + 1, B.k1);
    Term l0     = term_new(0, LAM, lam_ext, a + 0);
    Term l1     = term_new(0, LAM, lam_ext, a + 1);
    return heap_subst_cop(side, loc, l0, l1);
  }

  u64  a       = heap_alloc(5);
  heap_set(a + 4, bod);
  Copy B       = term_clone_at(a + 4, lab);
  heap_set(a + 0, B.k0);
  heap_set(a + 1, B.k1);
  heap_set(a + 2, term_new(0, VAR, 0, a + 0));
  heap_set(a + 3, term_new(0, VAR, 0, a + 1));
  Term su      = term_new(0, SUP, lab, a + 2);
  Term l0      = term_new(0, LAM, lam_ext, a + 0);
  Term l1      = term_new(0, LAM, lam_ext, a + 1);
  heap_subst_var(lam_loc, su);
  return heap_subst_cop(side, loc, l0, l1);
}
