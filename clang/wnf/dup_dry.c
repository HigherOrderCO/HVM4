// ! X &L = ^(f x)
// --------------- DUP-DRY
// ! F &L = f
// ! A &L = x
// X₀ ← ^(F₀ A₀)
// X₁ ← ^(F₁ A₁)
fn Term wnf_dup_dry(u32 lab, u32 loc, u8 side, Term dry) {
  ITRS_INC("DUP-DRY");
  u32  d_loc = term_val(dry);
  u64  base  = heap_alloc(6);
  u32  at    = (u32)base;
  heap_set(at + 0, heap_read(d_loc + 0));
  heap_set(at + 1, heap_read(d_loc + 1));
  heap_free(d_loc, 2);
  Copy F     = term_clone_at(at + 0, lab);
  Copy A     = term_clone_at(at + 1, lab);
  Term r0    = term_new_dry_at(at + 2, F.k0, A.k0);
  Term r1    = term_new_dry_at(at + 4, F.k1, A.k1);
  return heap_subst_cop(side, loc, r0, r1);
}
