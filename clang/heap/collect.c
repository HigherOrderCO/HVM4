// heap/gc.c - Semi-space copying garbage collector
//
// Heap layout after gc_init:
//   [0..base)         = book/parse allocations (untouched by GC)
//   [base..base+sz)   = from-space (active allocation region)
//   [base+sz..base+2*sz) = to-space (target for copying)
//
// When from-space fills up, wnf() bails via wnf_rebuild. eval_normalize
// calls gc_collect: trace from root_loc, copy live objects to to-space,
// flip spaces. Cost is proportional to the live set only.

#define GC_FWD_TAG 0x7F
#define GC_MARGIN  256

static int GC_ENABLED    = 0;
static int GC_NEEDED     = 0;
static u64 GC_SPACE_SZ   = 0;
static u64 GC_FROM_START = 0;
static u64 GC_TO_START   = 0;
static u64 GC_COUNT      = 0;

fn void gc_init(u64 space_words) {
  u64 base      = HEAP_NEXT_AT(0);
  GC_ENABLED    = 1;
  GC_SPACE_SZ   = space_words;
  GC_FROM_START = base;
  GC_TO_START   = base + space_words;
  GC_COUNT      = 0;
  GC_NEEDED     = 0;
  HEAP_NEXT_AT(0) = GC_FROM_START;
  HEAP_END_AT(0)  = GC_FROM_START + space_words;
}

fn Term gc_evacuate(Term term, u64 *alloc);

fn u64 gc_evacuate_chain(u64 loc, u64 *alloc) {
  u64 from_lo = GC_FROM_START;
  u64 from_hi = GC_FROM_START + GC_SPACE_SZ;
  if (loc == 0 || loc < from_lo || loc >= from_hi) {
    return loc;
  }
  if (term_tag(HEAP[loc]) == GC_FWD_TAG) {
    return term_val(HEAP[loc]);
  }
  u64 dst = *alloc;
  *alloc += 2;
  HEAP[dst]     = HEAP[loc];
  HEAP[dst + 1] = HEAP[loc + 1];
  HEAP[loc]     = term_new(0, GC_FWD_TAG, 0, dst);
  HEAP[dst] = gc_evacuate(HEAP[dst], alloc);
  u64 next_ls = term_val(HEAP[dst + 1]);
  if (next_ls != 0) {
    u64 new_next = gc_evacuate_chain(next_ls, alloc);
    HEAP[dst + 1] = term_new(0, NUM, 0, new_next);
  }
  return dst;
}

fn Term gc_evacuate(Term term, u64 *alloc) {
  u8  tag = term_tag(term);
  u64 loc = term_val(term);
  u64 from_lo = GC_FROM_START;
  u64 from_hi = GC_FROM_START + GC_SPACE_SZ;

  if (tag == NUM || tag == REF || tag == ERA || tag == NAM || tag == ANY ||
      tag == C00 || tag == PRI || tag == GC_FWD_TAG) {
    return term;
  }
  if (loc < from_lo || loc >= from_hi) {
    return term;
  }
  if (term_tag(HEAP[loc]) == GC_FWD_TAG) {
    u64 new_loc = term_val(HEAP[loc]);
    return term_new(term_sub_get(term), tag, term_ext(term), new_loc);
  }

  if (tag == ALO) {
    u32 len = term_ext(term);
    if (len == 0) return term;
    u64 dst = *alloc;
    *alloc += 1;
    HEAP[dst] = HEAP[loc];
    HEAP[loc] = term_new(0, GC_FWD_TAG, 0, dst);
    u64 pair   = HEAP[dst];
    u64 ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
    u64 tm_loc = pair & ALO_TM_MASK;
    if (ls_loc >= from_lo && ls_loc < from_hi) {
      u64 new_ls = gc_evacuate_chain(ls_loc, alloc);
      HEAP[dst] = ((new_ls & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK);
    }
    return term_new(term_sub_get(term), ALO, len, dst);
  }

  if (tag == VAR || tag == DP0 || tag == DP1 ||
      tag == BJV || tag == BJ0 || tag == BJ1) {
    u64 dst = *alloc;
    *alloc += 1;
    HEAP[dst] = HEAP[loc];
    HEAP[loc] = term_new(0, GC_FWD_TAG, 0, dst);
    HEAP[dst] = gc_evacuate(HEAP[dst], alloc);
    return term_new(term_sub_get(term), tag, term_ext(term), dst);
  }

  u32 arity = TERM_ARITY[tag];
  if (arity == 0) return term;

  u64 dst = *alloc;
  *alloc += arity;
  for (u32 i = 0; i < arity; i++) {
    HEAP[dst + i] = HEAP[loc + i];
  }
  HEAP[loc] = term_new(0, GC_FWD_TAG, 0, dst);
  for (u32 i = 0; i < arity; i++) {
    HEAP[dst + i] = gc_evacuate(HEAP[dst + i], alloc);
  }
  return term_new(term_sub_get(term), tag, term_ext(term), dst);
}

fn u64 gc_collect(u64 root_loc) {
  GC_COUNT++;
  u64 alloc = GC_TO_START;
  u64 new_root_loc = alloc++;
  HEAP[new_root_loc] = gc_evacuate(HEAP[root_loc], &alloc);
  u64 old_from  = GC_FROM_START;
  GC_FROM_START = GC_TO_START;
  GC_TO_START   = old_from;
  HEAP_NEXT_AT(0) = alloc;
  HEAP_END_AT(0)  = GC_FROM_START + GC_SPACE_SZ;
  GC_NEEDED = 0;
  return new_root_loc;
}
