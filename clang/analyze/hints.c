// analyze/hints.c - Post-parse program analysis for buffer sizing.
//
// Context
// - Runs after parse_def() completes, before evaluation begins.
// - Scans the static BOOK/HEAP to compute program size hints.
// - Hints are used to right-size runtime buffers (queues, uset, wspq).
//
// Design
// - Linear scan of static heap for tag statistics: O(N) where N = static terms.
// - Tree walk per BOOK entry for max depth measurement.
// - All data is read-only after parsing; no synchronization needed.

typedef struct {
  u64 node_count;      // Total term words in static heap
  u32 def_count;       // Number of @-definitions (TABLE_LEN)
  u32 max_arity;       // Largest constructor arity seen (C00-C16)
  u32 dup_count;       // DUP nodes found
  u32 sup_count;       // SUP nodes found
  u32 max_depth;       // Deepest term tree across all definitions
  u64 static_heap;     // Heap words used by static definitions
  u8  has_sup;         // 1 if any SUP exists
  u8  has_pri;         // 1 if any PRI exists
} HvmHints;

// Compute the smallest power-of-two exponent >= val, with min/max bounds.
fn u32 hints_cap_pow2(u64 val, u32 min_pow2, u32 max_pow2) {
  u32 p = min_pow2;
  while ((1ULL << p) < val && p < max_pow2) p++;
  return p;
}

fn HvmHints hvm_analyze(void) {
  HvmHints h = {0};
  h.def_count    = TABLE_LEN;
  h.static_heap  = HEAP_NEXT_AT(0);
  h.node_count   = h.static_heap > 1 ? h.static_heap - 1 : 0;

  // Linear scan of static heap for tag statistics.
  for (u64 i = 1; i < h.static_heap; i++) {
    Term t  = HEAP[i];
    u8  tag = term_tag(t);
    if (tag == DUP) h.dup_count++;
    if (tag == SUP) { h.sup_count++; h.has_sup = 1; }
    if (tag == PRI) h.has_pri = 1;
    if (tag >= C00 && tag <= C16) {
      u32 ari = tag - C00;
      if (ari > h.max_arity) h.max_arity = ari;
    }
  }

  // Tree walk per definition for max depth.
  // Stack-based iterative DFS to avoid deep recursion.
  #define HINTS_WALK_STACK 4096
  u32 walk_loc[HINTS_WALK_STACK];
  u32 walk_dep[HINTS_WALK_STACK];

  for (u32 id = 0; id < TABLE_LEN; id++) {
    if (BOOK[id] == 0) continue;
    u32 sp = 0;
    walk_loc[sp] = BOOK[id];
    walk_dep[sp] = 0;
    sp++;

    while (sp > 0) {
      sp--;
      u32 loc   = walk_loc[sp];
      u32 depth = walk_dep[sp];
      if (depth > h.max_depth) h.max_depth = depth;

      if (loc == 0 || loc >= h.static_heap) continue;

      Term t   = HEAP[loc];
      u8   tag = term_tag(t);
      u32  val = term_val(t);

      // Only recurse into children of compound nodes.
      // DP0/DP1/VAR/ALO/REF/NUM/ERA etc. have arity 0 → no children.
      u32 ari = TERM_ARITY[tag];
      if (tag == PRI) ari = 0; // can't determine statically

      for (u32 i = 0; i < ari && sp < HINTS_WALK_STACK; i++) {
        walk_loc[sp] = val + i;
        walk_dep[sp] = depth + 1;
        sp++;
      }
    }
  }
  #undef HINTS_WALK_STACK

  return h;
}

// Print hints summary to stderr (used by -v flag).
fn void hvm_hints_print(HvmHints *h) {
  fprintf(stderr, "[hints] defs=%u nodes=%llu max_arity=%u dups=%u sups=%u depth=%u static_heap=%llu\n",
    h->def_count,
    (unsigned long long)h->node_count,
    h->max_arity,
    h->dup_count,
    h->sup_count,
    h->max_depth,
    (unsigned long long)h->static_heap);

  // Compute and display buffer sizing decisions.
  u32 norm_pow2 = hints_cap_pow2(h->node_count / 4, 8, 24);
  u64 uset_locs = h->static_heap * 64;
  if (uset_locs < 4096) uset_locs = 4096;
  if (uset_locs > HEAP_CAP) uset_locs = HEAP_CAP;
  u64 uset_kb = ((uset_locs + 63) >> 6) * 8 / 1024;

  u32 wspq_brackets = h->max_depth + 4;
  if (wspq_brackets > WSPQ_BRACKETS) wspq_brackets = WSPQ_BRACKETS;
  if (wspq_brackets < 4) wspq_brackets = 4;

  fprintf(stderr, "[hints] normalize_queue=2^%u uset=%lluKB", norm_pow2, (unsigned long long)uset_kb);
  if (!h->has_sup) {
    fprintf(stderr, " collapse=minimal(no SUPs)");
  } else {
    u32 col_pow2 = hints_cap_pow2(h->sup_count * 4, 8, 24);
    fprintf(stderr, " collapse_queue=2^%u brackets=%u", col_pow2, wspq_brackets);
  }
  fprintf(stderr, "\n");
}
