// %gnf(term) — GPU Normalize
// Copies term to Metal heap, runs SNF normalization on GPU, copies result back.

#include "../../../metal/host.h"

// Metal runtime state
static int     gnf_metal_ready = 0;
static u64    *gnf_mheap       = NULL;
static u32     gnf_mcursor     = 1; // Metal heap alloc cursor (0 is reserved)
static int     gnf_book_ready  = 0;

fn double gnf_time_diff_sec(struct timespec a, struct timespec b) {
  return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

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
// Paged direct map for location remapping (u32 key -> u32 val)
// ============================================================================

#define GNF_MAP_EMPTY 0xFFFFFFFF
#define GNF_MAP_PAGE_BITS 14
#define GNF_MAP_PAGE_SIZE (1u << GNF_MAP_PAGE_BITS)
#define GNF_MAP_PAGE_MASK (GNF_MAP_PAGE_SIZE - 1)
#define GNF_MAP_TOP_SIZE  (1u << (32 - GNF_MAP_PAGE_BITS))

typedef struct {
  u32 **pages;
  u32  *used_pages;
  u32   used_len;
  u32   used_cap;
} GnfMap;

fn void gnf_map_init(GnfMap *m) {
  m->pages = calloc(GNF_MAP_TOP_SIZE, sizeof(u32 *));
  m->used_cap = 1024;
  m->used_len = 0;
  m->used_pages = malloc((size_t)m->used_cap * sizeof(u32));
  if (!m->pages || !m->used_pages) {
    fprintf(stderr, "gnf: remap allocation failed\n");
    exit(1);
  }
}

fn void gnf_map_free(GnfMap *m) {
  for (u32 i = 0; i < m->used_len; i++) {
    free(m->pages[m->used_pages[i]]);
  }
  free(m->pages);
  free(m->used_pages);
  m->pages = NULL;
  m->used_pages = NULL;
  m->used_len = 0;
  m->used_cap = 0;
}

fn u32 gnf_map_get(GnfMap *m, u32 key) {
  u32 page = key >> GNF_MAP_PAGE_BITS;
  u32 slot = key & GNF_MAP_PAGE_MASK;
  u32 *pg = m->pages[page];
  return pg ? pg[slot] : GNF_MAP_EMPTY;
}

fn void gnf_map_set(GnfMap *m, u32 key, u32 val) {
  u32 page = key >> GNF_MAP_PAGE_BITS;
  u32 slot = key & GNF_MAP_PAGE_MASK;
  u32 *pg = m->pages[page];
  if (pg == NULL) {
    pg = malloc((size_t)GNF_MAP_PAGE_SIZE * sizeof(u32));
    if (!pg) {
      fprintf(stderr, "gnf: remap page allocation failed\n");
      exit(1);
    }
    memset(pg, 0xFF, (size_t)GNF_MAP_PAGE_SIZE * sizeof(u32));
    m->pages[page] = pg;
    if (m->used_len >= m->used_cap) {
      u32 new_cap = m->used_cap << 1;
      u32 *new_used = realloc(m->used_pages, (size_t)new_cap * sizeof(u32));
      if (!new_used) {
        fprintf(stderr, "gnf: remap page list allocation failed\n");
        exit(1);
      }
      m->used_pages = new_used;
      m->used_cap = new_cap;
    }
    m->used_pages[m->used_len++] = page;
  }
  pg[slot] = val;
}

// ============================================================================
// Static book upload (for REF/ALO resolution on GPU)
// ============================================================================

fn u32 gnf_book_max_name(void) {
  for (u32 i = BOOK_CAP; i > 0; i--) {
    if (BOOK[i - 1] != 0) return i - 1;
  }
  return 0;
}

fn void gnf_book_mark_loc(GnfMap *seen, u32 loc) {
  gnf_map_set(seen, loc, 1);
}

fn int gnf_book_loc_seen(GnfMap *seen, u32 loc) {
  return gnf_map_get(seen, loc) != GNF_MAP_EMPTY;
}

fn u32 gnf_book_max_loc_rec(u32 loc, GnfMap *seen) {
  if (loc == 0 || gnf_book_loc_seen(seen, loc)) {
    return 0;
  }
  gnf_book_mark_loc(seen, loc);

  Term t = heap_read(loc);
  u8  tag = term_tag(t);
  u32 val = term_val(t);
  u32 ext = term_ext(t);
  u32 max_loc = loc;
  u32 ari = term_arity(t);

  if (ari > 0 && val + ari - 1 > max_loc) {
    max_loc = val + ari - 1;
  }

  if (tag == LAM) {
    u32 m = gnf_book_max_loc_rec(val, seen);
    if (m > max_loc) max_loc = m;
  } else if (ari > 0) {
    for (u32 i = 0; i < ari; i++) {
      u32 m = gnf_book_max_loc_rec(val + i, seen);
      if (m > max_loc) max_loc = m;
    }
  }

  if (tag == REF && BOOK[ext] != 0) {
    u32 m = gnf_book_max_loc_rec(BOOK[ext], seen);
    if (m > max_loc) max_loc = m;
  }

  return max_loc;
}

fn void gnf_upload_book_once(void) {
  if (gnf_book_ready) return;

  u32 max_name = gnf_book_max_name();
  u32 book_count = max_name + 1;
  if (book_count == 0) book_count = 1;

  GnfMap seen;
  gnf_map_init(&seen);
  u32 max_loc = 0;
  for (u32 i = 0; i < book_count; i++) {
    if (BOOK[i] == 0) continue;
    u32 m = gnf_book_max_loc_rec(BOOK[i], &seen);
    if (m > max_loc) max_loc = m;
  }
  gnf_map_free(&seen);

  u32 heap_words = max_loc + 1;
  if (heap_words == 0) heap_words = 1;

  if (metal_book_upload(BOOK, book_count, HEAP, heap_words) != 0) {
    fprintf(stderr, "gnf: failed to upload definition book to Metal\n");
    exit(1);
  }
  gnf_book_ready = 1;
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
    case OP2: {
      u32 mloc = gnf_metal_alloc(2);
      Term mx = gnf_instantiate(val, bind_stack, bind_len, mheap);
      Term my = gnf_instantiate(val + 1, bind_stack, bind_len, mheap);
      mheap[mloc]     = mx;
      mheap[mloc + 1] = my;
      return term_new(0, OP2, ext, mloc);
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

fn Term gnf_copy_to_metal(Term t, GnfMap *remap, u64 *mheap);

fn u32 gnf_copy_bind_list_to_metal(u32 ls_loc, GnfMap *remap, u64 *mheap) {
  if (ls_loc == 0) return 0;
  u32 existing = gnf_map_get(remap, ls_loc);
  if (existing != GNF_MAP_EMPTY) return existing;

  u32 m_ls = gnf_metal_alloc(1);
  gnf_map_set(remap, ls_loc, m_ls);

  u64 entry = HEAP[ls_loc];
  u32 bind_loc = (u32)(entry >> 32);
  u32 next_loc = (u32)(entry & 0xFFFFFFFF);

  u32 m_bind = 0;
  if (bind_loc != 0) {
    u32 bind_existing = gnf_map_get(remap, bind_loc);
    if (bind_existing != GNF_MAP_EMPTY) {
      m_bind = bind_existing;
    } else {
      m_bind = gnf_metal_alloc(1);
      gnf_map_set(remap, bind_loc, m_bind);
      Term cell = HEAP[bind_loc];
      if (term_sub_get(cell)) {
        Term copied = gnf_copy_to_metal(term_sub_set(cell, 0), remap, mheap);
        mheap[m_bind] = term_sub_set(copied, 1);
      } else {
        mheap[m_bind] = gnf_copy_to_metal(cell, remap, mheap);
      }
    }
  }

  u32 m_next = gnf_copy_bind_list_to_metal(next_loc, remap, mheap);
  mheap[m_ls] = ((u64)m_bind << 32) | m_next;
  return m_ls;
}

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

  // Keep REF as-is. GPU resolves name -> book location.
  if (tag == REF) {
    return term_new_ref(ext);
  }

  // Keep ALO as-is. Copy ALO node + bind list; book term location stays static.
  if (tag == ALO) {
    u32 alo_loc = val;
    u32 existing = gnf_map_get(remap, alo_loc);
    if (existing != GNF_MAP_EMPTY) {
      return term_new(0, ALO, ext, existing);
    }
    u32 m_alo = gnf_metal_alloc(1);
    gnf_map_set(remap, alo_loc, m_alo);

    u64 pair    = HEAP[alo_loc];
    u32 tm_loc  = (u32)(pair & 0xFFFFFFFF);
    u32 ls_loc  = (u32)(pair >> 32);
    u32 m_ls = gnf_copy_bind_list_to_metal(ls_loc, remap, mheap);
    mheap[m_alo] = ((u64)m_ls << 32) | tm_loc;
    return term_new(0, ALO, ext, m_alo);
  }

  // Unsupported tags
  if (tag != APP && tag != LAM && tag != SUP && tag != DUP &&
      tag != OP2 &&
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

typedef struct {
  u8  out_tag;
  u32 out_ext;
  u32 cloc;
  u32 mloc;
  u32 ari;
  u32 idx;
} GnfCopyFrame;

fn Term gnf_copy_from_metal(Term t, GnfMap *remap, u64 *mheap, u64 *copied_words) {
  u64 heap_idx  = (u64)WNF_TID * HEAP_STRIDE;
  u64 heap_next = HEAP_NEXT[heap_idx];
  u64 heap_end  = HEAP_END[heap_idx];

  u32 cap = 1024;
  u32 len = 0;
  GnfCopyFrame *stk = malloc((size_t)cap * sizeof(GnfCopyFrame));
  if (!stk) {
    fprintf(stderr, "gnf: copy stack allocation failed\n");
    exit(1);
  }

  Term cur = t;
  Term out = 0;

  for (;;) {
    u8  tag = term_tag(cur);
    u32 ext = term_ext(cur);
    u32 val = term_val(cur);

    // Follow substitutions on Metal heap (with cycle detection).
    if (tag == VAR || tag == DP0 || tag == DP1) {
      u32 mloc = val;
      u32 sub_steps = 0;
      while (sub_steps < 256) {
        Term cell = mheap[mloc];
        if (!term_sub_get(cell)) break;
        Term unsub = term_sub_set(cell, 0);
        u8 utag = term_tag(unsub);
        if (utag == VAR || utag == DP0 || utag == DP1) {
          mloc = term_val(unsub);
          tag  = utag;
          ext  = term_ext(unsub);
          sub_steps++;
        } else {
          cur = unsub;
          goto next_node;
        }
      }

      u32 existing = gnf_map_get(remap, mloc);
      if (existing != GNF_MAP_EMPTY) {
        out = term_new(0, tag, ext, existing);
        goto produced;
      }

      u64 cloc64 = heap_next;
      heap_next += 1;
      if (__builtin_expect(heap_next > heap_end || heap_next < cloc64, 0)) {
        fprintf(stderr, "gnf: out of C heap memory during copy-back\n");
        free(stk);
        exit(1);
      }
      u32 cloc = (u32)cloc64;
      *copied_words += 1;
      gnf_map_set(remap, mloc, cloc);

      if (len == cap) {
        cap <<= 1;
        GnfCopyFrame *new_stk = realloc(stk, (size_t)cap * sizeof(GnfCopyFrame));
        if (!new_stk) {
          fprintf(stderr, "gnf: copy stack reallocation failed\n");
          exit(1);
        }
        stk = new_stk;
      }
      stk[len++] = (GnfCopyFrame){
        .out_tag = tag, .out_ext = ext, .cloc = cloc, .mloc = mloc, .ari = 1, .idx = 0
      };
      cur = mheap[mloc];
      goto next_node;
    }

    // ERA, NUM: immediate.
    if (tag == REF) {
      out = term_new_ref(ext);
      goto produced;
    }
    if (tag == ERA) {
      out = term_new(0, ERA, 0, 0);
      goto produced;
    }
    if (tag == NUM) {
      out = term_new(0, NUM, 0, val);
      goto produced;
    }

    // APP(2), LAM(1), SUP(2), DUP(2).
    u32 mloc = val;
    u32 existing = gnf_map_get(remap, mloc);
    if (existing != GNF_MAP_EMPTY) {
      out = term_new(0, tag, ext, existing);
      goto produced;
    }

    u32 ari = TERM_ARITY[tag];
    if (ari == 0) {
      if (tag == ALO) {
        fprintf(stderr, "gnf: copy-back of residual ALO is not supported yet\n");
      }
      fprintf(stderr, "gnf: unsupported tag %u in metal term\n", tag);
      free(stk);
      exit(1);
    }

    u64 cloc64 = heap_next;
    heap_next += ari;
    if (__builtin_expect(heap_next > heap_end || heap_next < cloc64, 0)) {
      fprintf(stderr, "gnf: out of C heap memory during copy-back\n");
      free(stk);
      exit(1);
    }
    u32 cloc = (u32)cloc64;
    *copied_words += ari;
    gnf_map_set(remap, mloc, cloc);

    if (len == cap) {
      cap <<= 1;
      GnfCopyFrame *new_stk = realloc(stk, (size_t)cap * sizeof(GnfCopyFrame));
      if (!new_stk) {
        fprintf(stderr, "gnf: copy stack reallocation failed\n");
        exit(1);
      }
      stk = new_stk;
    }
    stk[len++] = (GnfCopyFrame){
      .out_tag = tag, .out_ext = ext, .cloc = cloc, .mloc = mloc, .ari = ari, .idx = 0
    };
    cur = mheap[mloc];
    goto next_node;

produced:
    while (len > 0) {
      GnfCopyFrame *fr = &stk[len - 1];
      HEAP[fr->cloc + fr->idx] = out;
      fr->idx++;
      if (fr->idx < fr->ari) {
        cur = mheap[fr->mloc + fr->idx];
        goto next_node;
      }
      out = term_new(0, fr->out_tag, fr->out_ext, fr->cloc);
      len--;
    }
    HEAP_NEXT[heap_idx] = heap_next;
    free(stk);
    return out;

next_node:
    continue;
  }
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
  gnf_upload_book_once();

  // 2. WNF the argument
  Term term = wnf(args[0]);

  // 3. Copy term to Metal heap
  GnfMap remap;
  gnf_map_init(&remap);

  u32 mroot_loc = gnf_metal_alloc(1);
  gnf_mheap[mroot_loc] = gnf_copy_to_metal(term, &remap, gnf_mheap);

  gnf_map_free(&remap);

  // 4. Set alloc cursor and run GPU normalization
  struct timespec gpu_t0, gpu_t1, copy_t0, copy_t1;
  metal_set_alloc_cursor(gnf_mcursor);
  clock_gettime(CLOCK_MONOTONIC, &gpu_t0);
  u64 itrs = metal_normalize(mroot_loc);
  clock_gettime(CLOCK_MONOTONIC, &gpu_t1);
  ITRS += itrs;
  if (metal_last_error()) {
    fprintf(stderr, "gnf: metal normalize failed (runtime error)\n");
    return term;
  }

  // 5. Copy result back to C heap
  clock_gettime(CLOCK_MONOTONIC, &copy_t0);
  Term mresult = gnf_mheap[mroot_loc];
  GnfMap remap_back;
  gnf_map_init(&remap_back);
  u64 copied_words = 0;

  Term result = gnf_copy_from_metal(mresult, &remap_back, gnf_mheap, &copied_words);

  gnf_map_free(&remap_back);
  clock_gettime(CLOCK_MONOTONIC, &copy_t1);

  if (SHOW_STATS) {
    double gpu_dt  = gnf_time_diff_sec(gpu_t0, gpu_t1);
    double copy_dt = gnf_time_diff_sec(copy_t0, copy_t1);
    double gpu_mips = gpu_dt > 0.0 ? ((double)itrs / gpu_dt) / 1e6 : 0.0;
    double copied_mb = ((double)copied_words * sizeof(u64)) / (1024.0 * 1024.0);
    fprintf(stderr,
            "gnf: itrs=%llu gpu=%.3fms perf=%.2f MIPS copy_back=%.3fms copied_slots=%llu copied=%.2fMB\n",
            (unsigned long long)itrs, gpu_dt * 1e3, gpu_mips, copy_dt * 1e3,
            (unsigned long long)copied_words, copied_mb);
  }

  // 6. Return the result
  return result;
}

// ============================================================================
// Registration
// ============================================================================

fn void prim_gnf_init(void) {
  prim_register("gnf", 3, 1, prim_fn_gnf);
}
