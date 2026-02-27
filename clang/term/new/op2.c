// Op2(opr, x, y): binary operation, strict on x first
// Layout: heap_read(loc+0) = x, heap_read(loc+1) = y
// EXT field = operation code (OP_ADD, OP_MUL, etc.)
fn Term term_new_op2_at(u64 loc, u32 opr, Term x, Term y) {
  heap_set(loc + 0, x);
  heap_set(loc + 1, y);
  return term_new(0, OP2, opr, loc);
}

fn Term term_new_op2(u32 opr, Term x, Term y) {
  return term_new_op2_at(heap_alloc_kind(2, AOT_HEAP_KIND_TERM_OP2), opr, x, y);
}