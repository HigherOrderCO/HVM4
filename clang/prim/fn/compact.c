// @compact(term): Normalize term, then deep-copy the result tree to fresh heap
// positions. In epoch mode, copies to the stable region and resets the nursery,
// reclaiming all evaluation intermediates in O(1).
//
// Without epoch mode, the old tree and evaluation intermediates remain in place
// (some freed to the free list during normalization, the rest as unreferenced
// garbage). This is safe but wastes memory over many rounds.
//
// With epoch mode, the lifecycle is:
//   1. Allocations during normalization go to the nursery (same heap_alloc)
//   2. compact_deep_copy copies the live result to the stable region
//   3. epoch_reset() reclaims the entire nursery in O(threads) pointer rewinds
//
// IC's cycle-freedom guarantees that after SNF, all nursery data except the
// result tree is dead. No tracing or ref-counting needed.

// Forward declarations (defined later in include order)
fn Term eval_normalize(Term term);

// Maximum recursion depth for compact_deep_copy. Prevents stack overflow on
// deeply nested SNF trees (e.g., long lists). 64K depth at ~64 bytes/frame
// uses ~4MB stack, well within typical 8MB stack limit.
#define COMPACT_MAX_DEPTH 65536

// Deep-copy a fully-normalized term tree to fresh heap positions.
// After eval_normalize, the tree should be pure SNF: constructors, numbers,
// lambdas, SUPs, ERAs, and REFs. No unresolved DP0/DP1 or VARs.
// Handles DP0/DP1/VAR defensively by following resolved substitutions.
//
// In epoch mode: allocates in the stable region via heap_alloc_stable().
// Without epoch mode: allocates in the caller's nursery via heap_alloc().
static Term compact_deep_copy_go(Term term, u32 depth) {
  if (__builtin_expect(depth > COMPACT_MAX_DEPTH, 0)) {
    fprintf(stderr, "compact: tree depth exceeds %u — returning term as-is\n",
      COMPACT_MAX_DEPTH);
    return term;
  }

  u8 tag = term_tag(term);

  // Follow resolved DP0/DP1 and VAR substitutions
  while (tag == DP0 || tag == DP1 || tag == VAR) {
    u32 loc = term_val(term);
    if (loc == 0) break;  // Null sentinel — can't read HEAP[0] (spin-waits)
    Term cell = heap_read(loc);
    if (term_sub_get(cell)) {
      term = term_sub_set(cell, 0);
      tag = term_tag(term);
    } else {
      break; // unresolved — copy the cell as-is
    }
  }

  // Determine number of heap children
  u32 nch;
  switch (tag) {
    case NUM: case ERA: case NAM: case ANY:
    case C00: case BJV: case BJ0: case BJ1:
    case REF: case F_OP2_NUM:
      return term; // no heap children — return as-is

    case DP0: case DP1: case VAR:
      nch = 1; break;

    // F_EQL_R stores a heap loc in ext field which we can't relocate here.
    // It should not appear in SNF. If it does, copy val-children but note
    // the ext-field reference is NOT updated (potential stale pointer in epoch).
    case F_EQL_R:
      nch = 2; break;

    case ALO:
      nch = 0; break; // ALO should not appear in SNF, skip

    case PRI:
      nch = prim_arity(term_ext(term)); break;

    default:
      nch = TERM_ARITY[tag]; break;
  }

  if (nch == 0) return term;

  // In epoch mode, skip copying data that's already in the stable region
  u32 old_loc = term_val(term);
  if (EPOCH_ENABLED && epoch_is_stable(old_loc)) {
    return term;
  }

  // Allocate in stable (epoch mode) or nursery (classic mode)
  u32 new_loc = (u32)(EPOCH_ENABLED ? heap_alloc_stable(nch) : heap_alloc(nch));
  for (u32 i = 0; i < nch; i++) {
    heap_set(new_loc + i, compact_deep_copy_go(heap_read(old_loc + i), depth + 1));
  }
  return term_new(term_sub_get(term), tag, term_ext(term), new_loc);
}

static Term compact_deep_copy(Term term) {
  return compact_deep_copy_go(term, 0);
}

fn Term prim_fn_compact(Term *args) {
  if (EPOCH_ENABLED) {
    epoch_begin();
  }

  // 1. Normalize the argument to SNF
  Term root = eval_normalize(args[0]);

  // 2. Deep-copy the normalized tree to fresh/stable heap positions
  Term copy = compact_deep_copy(root);

  if (EPOCH_ENABLED) {
    // 3. Reclaim nursery: O(threads) pointer rewinds
    epoch_reset();
  }

  return copy;
}

fn void prim_compact_init(void) {
  prim_register("compact", 7, 1, prim_fn_compact);
}
