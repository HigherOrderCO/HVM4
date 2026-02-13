// ! X &L = T{a,b,...}
// ------------------- DUP-NOD
// ! A &L = a
// ! B &L = b
// ...
// X₀ ← T{A₀,B₀,...}
// X₁ ← T{A₁,B₁,...}
fn Term wnf_dup_nod(u32 lab, u32 loc, u8 side, Term term) {
  ITRS_INC("DUP-NOD");
  u32 ari = term_arity(term);
  if (ari == 0) {
    heap_subst_var_dup(loc, term);
    return term;
  }
  u32  t_loc = term_val(term);
  u32  t_ext = term_ext(term);
  u8   t_tag = term_tag(term);
  // Split into 3 separate allocations so each can reuse freed blocks
  u64  vals_blk  = heap_alloc(ari);
  u64  r0_blk    = heap_alloc(ari);
  u64  r1_blk    = heap_alloc(ari);
  u32  vals      = (u32)vals_blk;
  u32  r0_loc    = (u32)r0_blk;
  u32  r1_loc    = (u32)r1_blk;
  for (u32 i = 0; i < ari; i++) {
    heap_set(vals + i, heap_read(t_loc + i));
    Copy A = term_clone_at(vals + i, lab);
    heap_set(r0_loc + i, A.k0);
    heap_set(r1_loc + i, A.k1);
  }
  heap_free(t_loc, ari);
  Term r0 = term_new(0, t_tag, t_ext, r0_loc);
  Term r1 = term_new(0, t_tag, t_ext, r1_loc);
  return heap_subst_cop(side, loc, r0, r1);
}
