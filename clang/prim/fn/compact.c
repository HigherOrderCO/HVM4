// @compact(term): Normalize term, then deep-copy the result tree to fresh heap
// positions. The old tree and evaluation intermediates remain in place (some
// freed to the free list during normalization, the rest as unreferenced garbage).
//
// This approach is safe to call from ANY position in the evaluation — inside
// WNF, inside eval_normalize, nested in expressions, etc. — because it never
// modifies or resets existing heap data. It only allocates new heap space.
//
// For the Bellman-Ford use case: each round produces ~O(tree_size) live data.
// The evaluation intermediates are mostly freed to the free list and reused.
// The deep copy adds ~tree_size new words. Over R rounds with tree size T,
// total extra heap usage is O(R * T), which is modest for practical graphs.
//
// TODO: For very large problems (1000+ nodes, 100+ rounds), implement a proper
// mark-compact GC with full root set discovery (including eval_normalize state
// and WNF work queues) to reclaim dead space.

// Forward declarations (defined later in include order)
fn Term eval_normalize(Term term);

// Deep-copy a fully-normalized term tree to fresh heap positions.
// After eval_normalize, the tree should be pure SNF: constructors, numbers,
// lambdas, SUPs, ERAs, and REFs. No unresolved DP0/DP1 or VARs.
// Handles DP0/DP1/VAR defensively by following resolved substitutions.
static Term compact_deep_copy(Term term) {
  u8 tag = term_tag(term);

  // Follow resolved DP0/DP1 and VAR substitutions
  while (tag == DP0 || tag == DP1 || tag == VAR) {
    u32 loc = term_val(term);
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

  // Recursively copy children to fresh heap locations
  u32 old_loc = term_val(term);
  u32 new_loc = (u32)heap_alloc(nch);
  for (u32 i = 0; i < nch; i++) {
    heap_set(new_loc + i, compact_deep_copy(heap_read(old_loc + i)));
  }
  return term_new(term_sub_get(term), tag, term_ext(term), new_loc);
}

fn Term prim_fn_compact(Term *args) {
  // 1. Normalize the argument to SNF
  Term root = eval_normalize(args[0]);

  // 2. Deep-copy the normalized tree to fresh heap positions
  Term copy = compact_deep_copy(root);

  return copy;
}

fn void prim_compact_init(void) {
  prim_register("compact", 7, 1, prim_fn_compact);
}
