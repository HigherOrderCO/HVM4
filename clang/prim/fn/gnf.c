// %gnf(term) — GPU Normalize
// Copies term to Metal heap, runs SNF normalization on GPU, copies result back.

#include "../../../metal/host.h"

// Metal runtime state
static int     gnf_metal_ready = 0;
static u64    *gnf_mheap       = NULL;
static u32     gnf_mcursor     = 1; // Metal heap alloc cursor (0 is reserved)

// ============================================================================
// Metal heap allocator (bump allocator on the shared Metal buffer)
// ============================================================================

fn u32 gnf_metal_alloc(u32 size) {
  u32 at = gnf_mcursor;
  gnf_mcursor += size;
  if (gnf_mcursor >= METAL_HEAP_WORDS) {
    fprintf(stderr, "gnf: out of Metal heap memory\n");
    exit(1);
  }
  return at;
}

// ============================================================================
// Simple hash map for location remapping (open addressing, power-of-2)
// ============================================================================

#define GNF_MAP_CAP (1u << 20) // 1M entries
#define GNF_MAP_MASK (GNF_MAP_CAP - 1)
#define GNF_MAP_EMPTY 0xFFFFFFFF

typedef struct {
  u32 *keys;
  u32 *vals;
} GnfMap;

fn void gnf_map_init(GnfMap *m) {
  m->keys = malloc(GNF_MAP_CAP * sizeof(u32));
  m->vals = malloc(GNF_MAP_CAP * sizeof(u32));
  memset(m->keys, 0xFF, GNF_MAP_CAP * sizeof(u32)); // fill with GNF_MAP_EMPTY
}

fn void gnf_map_free(GnfMap *m) {
  free(m->keys);
  free(m->vals);
}

fn u32 gnf_map_get(GnfMap *m, u32 key) {
  u32 h = (key * 2654435761u) & GNF_MAP_MASK;
  for (;;) {
    if (m->keys[h] == key) return m->vals[h];
    if (m->keys[h] == GNF_MAP_EMPTY) return GNF_MAP_EMPTY;
    h = (h + 1) & GNF_MAP_MASK;
  }
}

fn void gnf_map_set(GnfMap *m, u32 key, u32 val) {
  u32 h = (key * 2654435761u) & GNF_MAP_MASK;
  for (;;) {
    if (m->keys[h] == GNF_MAP_EMPTY || m->keys[h] == key) {
      m->keys[h] = key;
      m->vals[h] = val;
      return;
    }
    h = (h + 1) & GNF_MAP_MASK;
  }
}

// ============================================================================
// gnf_instantiate: instantiate a static (book) term onto Metal heap
// ============================================================================

// Bind stack: tracks Metal heap locations for lambda/dup scopes.
// bind_stack[i] is the Metal heap location for de Bruijn level (i+1).
#define GNF_BIND_CAP 4096

fn Term gnf_instantiate(u32 book_loc, u32 *bind_stack, u32 bind_len, u64 *mheap) {
  Term book = heap_read(book_loc);
  u8  tag = term_tag(book);
  u32 ext = term_ext(book);
  u32 val = term_val(book);

  switch (tag) {
    case VAR:
    case BJV: {
      u32 lvl = val;
      if (lvl == 0 || lvl > bind_len) {
        fprintf(stderr, "gnf: unbound VAR/BJV (lvl=%u, bind_len=%u)\n", lvl, bind_len);
        exit(1);
      }
      u32 metal_loc = bind_stack[lvl - 1];
      return term_new(0, VAR, 0, metal_loc);
    }
    case DP0:
    case DP1:
    case BJ0:
    case BJ1: {
      u32 lvl = val;
      if (lvl == 0 || lvl > bind_len) {
        fprintf(stderr, "gnf: unbound DP/BJ (lvl=%u, bind_len=%u)\n", lvl, bind_len);
        exit(1);
      }
      u32 metal_loc = bind_stack[lvl - 1];
      u8 out_tag = (tag == BJ0 || tag == DP0) ? DP0 : DP1;
      return term_new(0, out_tag, ext, metal_loc);
    }
    case LAM: {
      u32 body_loc = val;
      u32 mloc = gnf_metal_alloc(1);
      bind_stack[bind_len] = mloc;
      Term body = gnf_instantiate(body_loc, bind_stack, bind_len + 1, mheap);
      mheap[mloc] = body;
      return term_new(0, LAM, ext, mloc);
    }
    case DUP: {
      u32 book_val_loc  = val;
      u32 book_body_loc = val + 1;
      u32 mloc = gnf_metal_alloc(2);
      Term mval = gnf_instantiate(book_val_loc, bind_stack, bind_len, mheap);
      mheap[mloc] = mval;
      bind_stack[bind_len] = mloc;
      Term mbody = gnf_instantiate(book_body_loc, bind_stack, bind_len + 1, mheap);
      mheap[mloc + 1] = mbody;
      return term_new(0, DUP, ext, mloc);
    }
    case APP: {
      u32 mloc = gnf_metal_alloc(2);
      Term mfun = gnf_instantiate(val, bind_stack, bind_len, mheap);
      Term marg = gnf_instantiate(val + 1, bind_stack, bind_len, mheap);
      mheap[mloc]     = mfun;
      mheap[mloc + 1] = marg;
      return term_new(0, APP, ext, mloc);
    }
    case SUP: {
      u32 mloc = gnf_metal_alloc(2);
      Term mt0 = gnf_instantiate(val, bind_stack, bind_len, mheap);
      Term mt1 = gnf_instantiate(val + 1, bind_stack, bind_len, mheap);
      mheap[mloc]     = mt0;
      mheap[mloc + 1] = mt1;
      return term_new(0, SUP, ext, mloc);
    }
    case ERA: {
      return term_new(0, ERA, 0, 0);
    }
    case NUM: {
      return term_new(0, NUM, 0, val);
    }
    case REF: {
      // REF definitions are closed terms with their own de Bruijn scope,
      // so we instantiate with an empty bind stack.
      u32 nam = ext;
      if (BOOK[nam] == 0) {
        fprintf(stderr, "gnf: undefined REF during instantiation\n");
        exit(1);
      }
      u32 ref_bind_stack[GNF_BIND_CAP];
      return gnf_instantiate(BOOK[nam], ref_bind_stack, 0, mheap);
    }
    default: {
      fprintf(stderr, "gnf: unsupported tag %u in book term during instantiation\n", tag);
      exit(1);
    }
  }
}

// ============================================================================
// gnf_copy_to_metal: deep-copy a dynamic (linked) term to Metal heap
// ============================================================================

fn Term gnf_copy_to_metal(Term t, GnfMap *remap, u64 *mheap) {
  u8  tag = term_tag(t);
  u32 ext = term_ext(t);
  u32 val = term_val(t);

  // Follow substitutions for variable-like tags
  if (tag == VAR || tag == DP0 || tag == DP1) {
    u32 loc = val;
    Term cell = HEAP[loc];
    if (term_sub_get(cell)) {
      return gnf_copy_to_metal(term_sub_set(cell, 0), remap, mheap);
    }
    u32 existing = gnf_map_get(remap, loc);
    if (existing != GNF_MAP_EMPTY) {
      return term_new(0, tag, ext, existing);
    }
    u32 mloc = gnf_metal_alloc(1);
    gnf_map_set(remap, loc, mloc);
    mheap[mloc] = gnf_copy_to_metal(cell, remap, mheap);
    return term_new(0, tag, ext, mloc);
  }

  // Expand REF: look up book definition, instantiate directly
  if (tag == REF) {
    u32 nam = ext;
    if (BOOK[nam] == 0) {
      fprintf(stderr, "gnf: undefined REF @%u\n", nam);
      exit(1);
    }
    u32 bind_stack[GNF_BIND_CAP];
    return gnf_instantiate(BOOK[nam], bind_stack, 0, mheap);
  }

  // Expand ALO: resolve the substitution chain, instantiate book term
  if (tag == ALO) {
    u32 alo_loc = val;
    u64 pair    = heap_read(alo_loc);
    u32 tm_loc  = (u32)(pair & 0xFFFFFFFF);
    u32 ls_loc  = (u32)(pair >> 32);
    u32 len     = ext;

    // Build bind stack from the linked list of bindings.
    // The list is newest→oldest (innermost first). We reverse it into
    // oldest-first ordering so gnf_instantiate resolves BJV(lvl) via
    // bind_stack[lvl - 1].
    u32 bind_stack[GNF_BIND_CAP];
    u32 bind_count = 0;

    // First pass: collect C heap bind_locs in linked-list order (newest first)
    u32 temp_locs[GNF_BIND_CAP];
    u32 it = ls_loc;
    while (it != 0 && bind_count < len) {
      u64 entry = heap_read(it);
      u32 bind_loc = (u32)(entry >> 32);
      u32 next     = (u32)(entry & 0xFFFFFFFF);
      temp_locs[bind_count] = bind_loc;
      bind_count++;
      it = next;
    }

    // Second pass: reverse and remap to Metal heap
    for (u32 i = 0; i < bind_count; i++) {
      u32 bind_loc = temp_locs[bind_count - 1 - i];
      u32 existing = gnf_map_get(remap, bind_loc);
      if (existing != GNF_MAP_EMPTY) {
        bind_stack[i] = existing;
      } else {
        u32 mloc = gnf_metal_alloc(1);
        gnf_map_set(remap, bind_loc, mloc);
        Term cell = HEAP[bind_loc];
        if (term_sub_get(cell)) {
          // Substituted binding: copy the value with SUB bit so GPU follows it
          Term copied = gnf_copy_to_metal(term_sub_set(cell, 0), remap, mheap);
          mheap[mloc] = term_sub_set(copied, 1);
        } else {
          mheap[mloc] = gnf_copy_to_metal(cell, remap, mheap);
        }
        bind_stack[i] = mloc;
      }
    }

    return gnf_instantiate(tm_loc, bind_stack, bind_count, mheap);
  }

  // Unsupported tags
  if (tag != APP && tag != LAM && tag != SUP && tag != DUP &&
      tag != ERA && tag != NUM) {
    fprintf(stderr, "gnf: unsupported tag %u in dynamic term\n", tag);
    exit(1);
  }

  // ERA, NUM: immediate (no heap children)
  if (tag == ERA) return term_new(0, ERA, 0, 0);
  if (tag == NUM) return term_new(0, NUM, 0, val);

  // APP(2), LAM(1), SUP(2), DUP(2): allocate on Metal heap, recurse
  u32 loc = val;
  u32 existing = gnf_map_get(remap, loc);
  if (existing != GNF_MAP_EMPTY) {
    return term_new(0, tag, ext, existing);
  }

  u32 ari = TERM_ARITY[tag];
  u32 mloc = gnf_metal_alloc(ari);
  gnf_map_set(remap, loc, mloc);

  for (u32 i = 0; i < ari; i++) {
    Term child = HEAP[loc + i];
    mheap[mloc + i] = gnf_copy_to_metal(child, remap, mheap);
  }

  return term_new(0, tag, ext, mloc);
}

// ============================================================================
// gnf_copy_from_metal: deep-copy Metal heap term back to C heap
// ============================================================================

fn Term gnf_copy_from_metal(Term t, GnfMap *remap, u64 *mheap) {
  u8  tag = term_tag(t);
  u32 ext = term_ext(t);
  u32 val = term_val(t);

  // Follow substitutions on Metal heap (with cycle detection)
  if (tag == VAR || tag == DP0 || tag == DP1) {
    u32 mloc = val;
    // Follow SUB chain, but stop if we see the same location twice (cycle)
    u32 sub_steps = 0;
    while (sub_steps < 256) {
      Term cell = mheap[mloc];
      if (!term_sub_get(cell)) break;
      Term unsub = term_sub_set(cell, 0);
      u8 utag = term_tag(unsub);
      if (utag == VAR || utag == DP0 || utag == DP1) {
        mloc = term_val(unsub);
        tag = utag;
        ext = term_ext(unsub);
        sub_steps++;
      } else {
        return gnf_copy_from_metal(unsub, remap, mheap);
      }
    }
    // Either no SUB or we've resolved to an unsubstituted location
    u32 existing = gnf_map_get(remap, mloc);
    if (existing != GNF_MAP_EMPTY) {
      return term_new(0, tag, ext, existing);
    }
    u32 cloc = heap_alloc(1);
    gnf_map_set(remap, mloc, cloc);
    Term cell = mheap[mloc];
    Term child = gnf_copy_from_metal(cell, remap, mheap);
    heap_set(cloc, child);
    return term_new(0, tag, ext, cloc);
  }

  // ERA, NUM: immediate
  if (tag == ERA) return term_new(0, ERA, 0, 0);
  if (tag == NUM) return term_new(0, NUM, 0, val);

  // APP(2), LAM(1), SUP(2), DUP(2)
  u32 mloc = val;
  u32 existing = gnf_map_get(remap, mloc);
  if (existing != GNF_MAP_EMPTY) {
    return term_new(0, tag, ext, existing);
  }

  u32 ari = TERM_ARITY[tag];
  u32 cloc = heap_alloc(ari);
  gnf_map_set(remap, mloc, cloc);

  for (u32 i = 0; i < ari; i++) {
    Term child = gnf_copy_from_metal(mheap[mloc + i], remap, mheap);
    heap_set(cloc + i, child);
  }

  return term_new(0, tag, ext, cloc);
}

// ============================================================================
// prim_fn_gnf: entry point for %gnf(term)
// ============================================================================

fn Term prim_fn_gnf(Term *args) {
  // 1. Lazy-init Metal runtime
  if (!gnf_metal_ready) {
    if (metal_init() != 0) {
      fprintf(stderr, "gnf: failed to initialize Metal runtime\n");
      exit(1);
    }
    gnf_mheap = metal_heap_ptr();
    gnf_mcursor = 1; // reserve slot 0
    gnf_metal_ready = 1;
  }

  // 2. WNF the argument
  Term term = wnf(args[0]);

  // 3. Copy term to Metal heap
  GnfMap remap;
  gnf_map_init(&remap);

  u32 mroot_loc = gnf_metal_alloc(1);
  gnf_mheap[mroot_loc] = gnf_copy_to_metal(term, &remap, gnf_mheap);

  gnf_map_free(&remap);

  // 4. Set alloc cursor and run GPU normalization
  metal_set_alloc_cursor(gnf_mcursor);
  u64 itrs = metal_normalize(mroot_loc);

  // 5. Copy result back to C heap
  Term mresult = gnf_mheap[mroot_loc];
  GnfMap remap_back;
  gnf_map_init(&remap_back);

  Term result = gnf_copy_from_metal(mresult, &remap_back, gnf_mheap);

  gnf_map_free(&remap_back);

  // 6. Return the result
  return result;
}

// ============================================================================
// Registration
// ============================================================================

fn void prim_gnf_init(void) {
  prim_register("gnf", 3, 1, prim_fn_gnf);
}
