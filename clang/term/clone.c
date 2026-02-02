fn Copy term_clone_at(u32 loc, u32 lab) {
  return (Copy){ term_new_dp0(lab, loc), term_new_dp1(lab, loc) };
}

fn Copy term_clone(u32 lab, Term val) {
  u64 loc   = heap_alloc(1);
  heap_set(loc, val);
  return term_clone_at(loc, lab);
}

fn void term_clone2(u32 lab, Term a, Term b, Copy *A, Copy *B) {
  u64 loc = heap_alloc(2);
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  *A = term_clone_at(loc + 0, lab);
  *B = term_clone_at(loc + 1, lab);
}

fn void term_clone3(u32 lab, Term a, Term b, Term c, Copy *A, Copy *B, Copy *C) {
  u64 loc = heap_alloc(3);
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  heap_set(loc + 2, c);
  *A = term_clone_at(loc + 0, lab);
  *B = term_clone_at(loc + 1, lab);
  *C = term_clone_at(loc + 2, lab);
}

fn void term_clone_many(u32 lab, Term *src, u32 n, Term *dst0, Term *dst1) {
  for (u32 i = 0; i < n; i++) {
    Copy c  = term_clone(lab, src[i]);
    dst0[i] = c.k0;
    dst1[i] = c.k1;
  }
}

// GET clones (no labels): creates GT0/GT1 that share a node.
fn Copy term_clone_get_at(u32 loc) {
  return (Copy){ term_new_gt0(loc), term_new_gt1(loc) };
}

fn Copy term_clone_get(Term val) {
  u64 loc = heap_alloc(1);
  heap_set(loc, val);
  return term_clone_get_at(loc);
}
