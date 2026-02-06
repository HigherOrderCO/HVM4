#include <metal_stdlib>
using namespace metal;

// ============================================================================
// HVM4 Metal - WNF Evaluator and Normalization Kernel
// ============================================================================
//
// Implements WNF (Weak Normal Form) for HVM4's Interaction Calculus on Metal.
// Only base terms: APP, LAM, SUP, DUP, VAR, DP0, DP1, ERA, NUM.
//
// Architecture:
// - Frontier-based BFS normalization (CPU orchestrates passes)
// - Per-thread bump allocation (no cross-thread alloc contention)
// - Regular device ulong* for heap (aligned 64-bit R/W is atomic on Apple Si)
// - 32-bit bitpacked lock array for DP0/DP1 take mutual exclusion
// - Bounded WNF stack per thread with rebuild on bail-out
// ============================================================================

typedef ulong Term;

// --- Tags ---
constant uchar TAG_APP = 0;
constant uchar TAG_VAR = 1;
constant uchar TAG_LAM = 2;
constant uchar TAG_DP0 = 3;
constant uchar TAG_DP1 = 4;
constant uchar TAG_SUP = 5;
constant uchar TAG_DUP = 6;
constant uchar TAG_ERA = 11;
constant uchar TAG_NUM = 30;

// --- Bit layout: [63:SUB] [62-56:TAG 7b] [55-32:EXT 24b] [31-0:VAL 32b] ---
constant ulong SUB_BIT = 1UL << 63;

// --- Flags ---
constant uint LAM_ERA_MASK = 0x800000;

// --- Breadcrumb tag for pointer-reversal WNF (APP frames stored in heap) ---
constant uchar TAG_BC_APP = 7;
constant uint  BC_SENTINEL = 0xFFFFFFFF;

// --- Limits ---
constant uint DP_STACK_CAP  = 64;         // DP frames only (much fewer than APPs)
constant uint WNF_MAX_ITERS = ~0u; // max uint32 (~4.3G iters per WNF call)

// --- Kernel parameters (set by host each pass) ---
struct Params {
  uint frontier_count;
};

// --- Clone result ---
struct Copy {
  Term k0;
  Term k1;
};

// ============================================================================
// Term Operations
// ============================================================================

inline uchar term_tag(Term t) { return (uchar)((t >> 56) & 0x7F); }
inline uint  term_ext(Term t) { return (uint)((t >> 32) & 0xFFFFFF); }
inline uint  term_val(Term t) { return (uint)(t & 0xFFFFFFFF); }
inline bool  term_sub_get(Term t) { return (t >> 63) != 0; }

inline Term term_sub_set(Term t, bool sub) {
  return (t & ~SUB_BIT) | ((ulong)(sub ? 1 : 0) << 63);
}

inline Term term_new(uint sub, uchar tag, uint ext, uint val) {
  return ((ulong)(sub & 1) << 63)
       | ((ulong)(tag & 0x7F) << 56)
       | ((ulong)(ext & 0xFFFFFF) << 32)
       | ((ulong)(val & 0xFFFFFFFF));
}

inline uint term_arity(uchar tag) {
  switch (tag) {
    case TAG_APP: return 2;
    case TAG_LAM: return 1;
    case TAG_SUP: return 2;
    case TAG_DUP: return 2;
    default:      return 0;
  }
}

// ============================================================================
// Heap Operations
// ============================================================================
//
// Uses regular device ulong* -- aligned 64-bit loads/stores are naturally
// atomic on Apple Silicon GPUs (no tearing). This avoids the need for
// atomic_ulong which lacks load/store/exchange support in Metal.

inline Term heap_read(device ulong* heap, uint loc) {
  return heap[loc];
}

inline void heap_set(device ulong* heap, uint loc, Term val) {
  heap[loc] = val;
}

// ============================================================================
// DP Lock Array (bitpacked, 32-bit atomics)
// ============================================================================
//
// Provides mutual exclusion for DP0/DP1 shared expression slots.
// Each heap word has a 1-bit lock packed into atomic_uint words.
// The host clears only the used portion between passes (up to alloc cursor).

inline bool dp_try_lock(device atomic_uint* locks, uint loc) {
  uint word = loc >> 5;
  uint bit  = 1u << (loc & 31);
  uint old  = atomic_fetch_or_explicit(&locks[word], bit, memory_order_relaxed);
  return (old & bit) == 0;
}

// heap_take: atomically lock the DP slot and read its value.
// Returns 0 on contention (another thread owns the slot).
inline Term heap_take(device ulong* heap, device atomic_uint* locks, uint loc) {
  if (!dp_try_lock(locks, loc)) {
    return 0; // contention -- caller must bail out
  }
  return heap[loc];
}

// ============================================================================
// Per-Thread Slab Allocator
// ============================================================================
//
// Each thread lazily grabs a slab from the global cursor (one atomic), then
// bumps locally within it (zero atomics). Eliminates cross-thread contention.

constant uint ALLOC_SLAB = 4096; // words per slab (32KB)

struct Alloc {
  uint pos;
  uint end;
  device atomic_uint* cursor; // global cursor for slab grabs
};

inline uint heap_alloc(thread Alloc& a, uint size) {
  if (a.pos + size > a.end) {
    uint grab = size > ALLOC_SLAB ? size : ALLOC_SLAB;
    uint base = atomic_fetch_add_explicit(a.cursor, grab, memory_order_relaxed);
    a.pos = base;
    a.end = base + grab;
  }
  uint loc = a.pos;
  a.pos += size;
  return loc;
}

// ============================================================================
// Substitution
// ============================================================================

inline void heap_subst_var(device ulong* heap, uint loc, Term val) {
  heap_set(heap, loc, term_sub_set(val, true));
}

inline Term heap_subst_cop(device ulong* heap, uint side, uint loc,
                           Term r0, Term r1) {
  heap_set(heap, loc, term_sub_set(side == 0 ? r1 : r0, true));
  return side == 0 ? r0 : r1;
}

// ============================================================================
// Term Constructors
// ============================================================================

inline Term term_new_var(uint loc)           { return term_new(0, TAG_VAR, 0, loc); }
inline Term term_new_era()                   { return term_new(0, TAG_ERA, 0, 0); }
inline Term term_new_num(uint n)             { return term_new(0, TAG_NUM, 0, n); }
inline Term term_new_dp0(uint lab, uint loc) { return term_new(0, TAG_DP0, lab, loc); }
inline Term term_new_dp1(uint lab, uint loc) { return term_new(0, TAG_DP1, lab, loc); }

inline Term term_new_app_at(device ulong* heap, uint loc,
                            Term fun, Term arg) {
  heap_set(heap, loc + 0, fun);
  heap_set(heap, loc + 1, arg);
  return term_new(0, TAG_APP, 0, loc);
}

inline Term term_new_app(device ulong* heap, thread Alloc& alloc,
                         Term fun, Term arg) {
  return term_new_app_at(heap, heap_alloc(alloc, 2), fun, arg);
}

inline Term term_new_sup_at(device ulong* heap, uint loc,
                            uint lab, Term tm0, Term tm1) {
  heap_set(heap, loc + 0, tm0);
  heap_set(heap, loc + 1, tm1);
  return term_new(0, TAG_SUP, lab, loc);
}

inline Term term_new_sup(device ulong* heap, thread Alloc& alloc,
                         uint lab, Term tm0, Term tm1) {
  return term_new_sup_at(heap, heap_alloc(alloc, 2), lab, tm0, tm1);
}

// ============================================================================
// Clone (creates DP0/DP1 pair pointing to same location)
// ============================================================================

inline Copy term_clone_at(uint loc, uint lab) {
  return Copy { term_new_dp0(lab, loc), term_new_dp1(lab, loc) };
}

// ============================================================================
// Interactions (base terms only)
// ============================================================================

// (λx.f a) → x←a; f
inline Term wnf_app_lam(device ulong* heap, Term lam, Term arg) {
  uint loc     = term_val(lam);
  uint lam_ext = term_ext(lam);
  Term body    = heap_read(heap, loc);
  if (!(lam_ext & LAM_ERA_MASK)) {
    heap_subst_var(heap, loc, arg);
  }
  return body;
}

// (ERA a) → ERA
inline Term wnf_app_era() {
  return term_new_era();
}

// (&L{f,g} a) → !A&L=a; &L{(f A₀),(g A₁)}
inline Term wnf_app_sup(device ulong* heap, thread Alloc& alloc,
                        Term app, Term sup) {
  uint app_loc = term_val(app);
  uint sup_loc = term_val(sup);
  uint lab     = term_ext(sup);
  Term arg     = heap_read(heap, app_loc + 1);
  Term tm1     = heap_read(heap, sup_loc + 1);
  uint loc     = heap_alloc(alloc, 3);
  heap_set(heap, loc + 2, arg);
  Copy D  = term_clone_at(loc + 2, lab);
  heap_set(heap, sup_loc + 1, D.k0);
  Term ap0 = term_new(0, TAG_APP, 0, sup_loc);
  Term ap1 = term_new_app_at(heap, loc, tm1, D.k1);
  return term_new_sup_at(heap, app_loc, lab, ap0, ap1);
}

// !F&L = λx.f → F₀←λ$x0.G₀, F₁←λ$x1.G₁, x←&L{$x0,$x1}, !G&L=f
inline Term wnf_dup_lam(device ulong* heap, thread Alloc& alloc,
                        uint lab, uint loc, uint side, Term lam) {
  uint lam_loc = term_val(lam);
  uint lam_ext = term_ext(lam);
  Term bod     = heap_read(heap, lam_loc);

  if (lam_ext & LAM_ERA_MASK) {
    uint base = heap_alloc(alloc, 3);
    heap_set(heap, base + 2, bod);
    Copy B = term_clone_at(base + 2, lab);
    heap_set(heap, base + 0, B.k0);
    heap_set(heap, base + 1, B.k1);
    Term l0 = term_new(0, TAG_LAM, lam_ext, base + 0);
    Term l1 = term_new(0, TAG_LAM, lam_ext, base + 1);
    return heap_subst_cop(heap, side, loc, l0, l1);
  }

  uint base = heap_alloc(alloc, 5);
  heap_set(heap, base + 4, bod);
  Copy B = term_clone_at(base + 4, lab);
  heap_set(heap, base + 0, B.k0);
  heap_set(heap, base + 1, B.k1);
  heap_set(heap, base + 2, term_new_var(base + 0));
  heap_set(heap, base + 3, term_new_var(base + 1));
  Term su = term_new(0, TAG_SUP, lab, base + 2);
  Term l0 = term_new(0, TAG_LAM, lam_ext, base + 0);
  Term l1 = term_new(0, TAG_LAM, lam_ext, base + 1);
  heap_subst_var(heap, lam_loc, su);
  return heap_subst_cop(heap, side, loc, l0, l1);
}

// !X&L = &R{a,b}
// if L==R: X₀←a, X₁←b
// else:    !A&L=a, !B&L=b, X₀←&R{A₀,B₀}, X₁←&R{A₁,B₁}
inline Term wnf_dup_sup(device ulong* heap, thread Alloc& alloc,
                        uint lab, uint loc, uint side, Term sup) {
  uint sup_loc = term_val(sup);
  uint sup_lab = term_ext(sup);
  if (lab == sup_lab) {
    Term tm0 = heap_read(heap, sup_loc + 0);
    Term tm1 = heap_read(heap, sup_loc + 1);
    return heap_subst_cop(heap, side, loc, tm0, tm1);
  } else {
    uint base = heap_alloc(alloc, 6);
    heap_set(heap, base + 0, heap_read(heap, sup_loc + 0));
    heap_set(heap, base + 1, heap_read(heap, sup_loc + 1));
    Copy A = term_clone_at(base + 0, lab);
    Copy B = term_clone_at(base + 1, lab);
    Term s0 = term_new_sup_at(heap, base + 2, sup_lab, A.k0, B.k0);
    Term s1 = term_new_sup_at(heap, base + 4, sup_lab, A.k1, B.k1);
    return heap_subst_cop(heap, side, loc, s0, s1);
  }
}

// !X&L = T  (arity-0 terms: ERA, NUM)
inline Term wnf_dup_nod(device ulong* heap, uint loc, uint side, Term term) {
  heap_subst_var(heap, loc, term);
  return term;
}

// ============================================================================
// WNF Evaluator (pointer-reversal for APP frames, small stack for DP frames)
// ============================================================================
//
// APP frames use pointer-reversal: we write a breadcrumb (TAG_BC_APP) into
// the APP node's function slot, forming a linked list through the heap.
// This gives unlimited APP spine depth with zero stack space.
//
// DP frames use a small thread-local stack (typically only ~n frames for P_n).
// Each DP stack entry records the DP term and the current APP breadcrumb head
// (saved_prev) so we can correctly interleave APP and DP unwinds in LIFO order.

Term wnf(device ulong* heap, device atomic_uint* locks,
         Term term, thread Alloc& alloc, thread uint& itrs, thread uint& bailouts) {
  // DP-only stack (small)
  Term  dp_terms[DP_STACK_CAP];
  uint  dp_prevs[DP_STACK_CAP];
  uint  dp_pos = 0;

  // APP breadcrumb chain head (linked list through heap)
  uint  prev = BC_SENTINEL;

  Term  cur  = term;
  bool  entering = true;
  uint  iters = WNF_MAX_ITERS;

  while (iters > 0) {
    iters--;

    // ==== ENTER PHASE ====
    if (entering) {
      uchar tag = term_tag(cur);

      if (tag == TAG_VAR) {
        uint loc  = term_val(cur);
        Term cell = heap_read(heap, loc);
        if (term_sub_get(cell)) {
          cur = term_sub_set(cell, false);
          continue;
        }
        entering = false;
        continue;
      }

      if (tag == TAG_DP0 || tag == TAG_DP1) {
        uint loc = term_val(cur);

        Term cell = heap_read(heap, loc);
        if (term_sub_get(cell)) {
          cur = term_sub_set(cell, false);
          continue;
        }

        Term taken = heap_take(heap, locks, loc);
        if (taken == 0) {
          entering = false;
          continue;
        }

        if (term_sub_get(taken)) {
          cur = term_sub_set(taken, false);
          continue;
        }

        if (dp_pos >= DP_STACK_CAP) {
          heap_set(heap, loc, taken); // restore
          bailouts++;
          break; // bail out: unwind everything after the loop
        }

        dp_terms[dp_pos] = cur;
        dp_prevs[dp_pos] = prev;
        dp_pos++;
        cur = taken;
        continue;
      }

      if (tag == TAG_APP) {
        uint app_loc = term_val(cur);
        Term fun = heap_read(heap, app_loc);
        // Write breadcrumb into function slot (pointer reversal)
        heap_set(heap, app_loc, term_new(0, TAG_BC_APP, 0, prev));
        prev = app_loc;
        cur = fun;
        continue;
      }

      if (tag == TAG_DUP) {
        uint loc = term_val(cur);
        cur = heap_read(heap, loc + 1);
        continue;
      }

      // LAM, SUP, ERA, NUM -> WHNF
      entering = false;
      continue;
    }

    // ==== APPLY PHASE ====
    // Decide next frame: DP stack or APP breadcrumb, whichever was entered last.
    // DP frames record saved_prev; if it matches current prev, DP is more recent.

    if (dp_pos > 0 && dp_prevs[dp_pos - 1] == prev) {
      // --- DP frame is next ---
      dp_pos--;
      Term dp_term = dp_terms[dp_pos];
      uchar dp_tag = term_tag(dp_term);
      uint side = (dp_tag == TAG_DP0) ? 0 : 1;
      uint loc  = term_val(dp_term);
      uint lab  = term_ext(dp_term);
      uchar w_tag = term_tag(cur);

      if (w_tag == TAG_LAM) {
        itrs++;
        cur = wnf_dup_lam(heap, alloc, lab, loc, side, cur);
        continue;
      }
      if (w_tag == TAG_SUP) {
        itrs++;
        cur = wnf_dup_sup(heap, alloc, lab, loc, side, cur);
        entering = true;
        continue;
      }
      if (w_tag == TAG_ERA || w_tag == TAG_NUM) {
        itrs++;
        cur = wnf_dup_nod(heap, loc, side, cur);
        continue;
      }
      // Stuck DP
      uint new_loc = heap_alloc(alloc, 1);
      heap_set(heap, new_loc, cur);
      heap_subst_var(heap, loc,
        term_new(0, side == 0 ? TAG_DP1 : TAG_DP0, lab, new_loc));
      cur = term_new(0, side == 0 ? TAG_DP0 : TAG_DP1, lab, new_loc);
      continue;
    }

    if (prev != BC_SENTINEL) {
      // --- APP breadcrumb is next ---
      Term bc      = heap_read(heap, prev);
      uint parent  = term_val(bc);
      uint app_loc = prev;
      Term arg     = heap_read(heap, app_loc + 1);
      uchar w_tag  = term_tag(cur);

      if (w_tag == TAG_LAM) {
        itrs++;
        cur = wnf_app_lam(heap, cur, arg);
        prev = parent;
        entering = true;
        continue;
      }
      if (w_tag == TAG_SUP) {
        itrs++;
        // wnf_app_sup reads arg from heap[app_loc+1] and writes output
        // to heap[app_loc]. Breadcrumb at heap[app_loc] gets overwritten.
        Term app_term = term_new(0, TAG_APP, 0, app_loc);
        cur = wnf_app_sup(heap, alloc, app_term, cur);
        prev = parent;
        continue;
      }
      if (w_tag == TAG_ERA) {
        itrs++;
        cur = wnf_app_era();
        prev = parent;
        continue;
      }
      // Stuck APP: restore function slot and rebuild
      heap_set(heap, app_loc, cur);
      cur = term_new(0, TAG_APP, 0, app_loc);
      prev = parent;
      continue;
    }

    // No frames left -- done
    return cur;
  }

  // Hit iteration limit or DP stack overflow -- unwind all remaining frames
  bailouts++;
  if (entering) {
    // cur is the term we were about to enter; use it as-is
  }
  while (prev != BC_SENTINEL || dp_pos > 0) {
    if (dp_pos > 0 && dp_prevs[dp_pos - 1] == prev) {
      dp_pos--;
      Term dp_term = dp_terms[dp_pos];
      uint loc = term_val(dp_term);
      heap_set(heap, loc, cur); // restore taken value
      cur = dp_term;
    } else if (prev != BC_SENTINEL) {
      Term bc = heap_read(heap, prev);
      uint parent = term_val(bc);
      Term arg = heap_read(heap, prev + 1);
      cur = term_new_app_at(heap, prev, cur, arg); // restore APP
      prev = parent;
    } else {
      break;
    }
  }
  return cur;
}

// ============================================================================
// wnf_at: reduce term at heap location, write back if changed
// ============================================================================

inline Term wnf_at(device ulong* heap, device atomic_uint* locks,
                   uint loc, thread Alloc& alloc, thread uint& itrs, thread uint& bailouts) {
  Term cur = heap_read(heap, loc);
  uchar tag = term_tag(cur);

  // Fast path: already WHNF
  if (tag == TAG_LAM || tag == TAG_SUP || tag == TAG_ERA || tag == TAG_NUM) {
    return cur;
  }

  Term res = wnf(heap, locks, cur, alloc, itrs, bailouts);
  if (res != cur) {
    heap_set(heap, loc, res);
  }
  return res;
}

// ============================================================================
// Normalize Pass Kernel
// ============================================================================
//
// Each thread processes one frontier entry:
// 1. WNF the term at that heap location
// 2. Enqueue children into next_frontier for the next pass

kernel void normalize_pass(
  device ulong*         heap           [[buffer(0)]],
  device uint*          frontier       [[buffer(1)]],
  device uint*          next_frontier  [[buffer(2)]],
  device atomic_uint*   next_count     [[buffer(3)]],
  constant Params&      params         [[buffer(4)]],
  device atomic_uint*   locks          [[buffer(5)]],
  device atomic_uint*   itr_count      [[buffer(6)]],
  device atomic_uint*   alloc_cursor   [[buffer(7)]],
  uint                  tid            [[thread_position_in_grid]])
{
  if (tid >= params.frontier_count) return;

  uint local_itrs = 0;
  uint local_bailouts = 0;

  // --- Per-thread slab allocator ---
  Alloc alloc = { 0, 0, alloc_cursor };

  // --- WNF at frontier location ---
  uint loc    = frontier[tid];
  Term result = wnf_at(heap, locks, loc, alloc, local_itrs, local_bailouts);

  // --- If result is a DP, retry (another thread may have resolved it) ---
  uchar tag = term_tag(result);
  if (tag == TAG_DP0 || tag == TAG_DP1) {
    // Retry: the winning thread may have written SUB by now
    Term retry = wnf(heap, locks, result, alloc, local_itrs, local_bailouts);
    if (retry != result) {
      result = retry;
      heap_set(heap, loc, result);
      tag = term_tag(result);
    }
  }

  // --- Compute how many frontier slots this thread needs ---
  uint val   = term_val(result);
  uint need  = 0;
  uint slots[2];  // at most 2 entries to enqueue

  if (local_bailouts > 0 && tag != TAG_LAM && tag != TAG_SUP &&
      tag != TAG_ERA && tag != TAG_NUM) {
    slots[0] = loc;
    need = 1;
  } else if (tag == TAG_DP0 || tag == TAG_DP1) {
    slots[0] = val;
    slots[1] = loc;
    need = 2;
  } else {
    uint ari = term_arity(tag);
    need = ari;
    for (uint i = 0; i < ari; i++) slots[i] = val + i;
  }

  // --- SIMD-cooperative frontier enqueue (1 atomic per 32 threads) ---
  uint lane_offset = simd_prefix_exclusive_sum(need);
  uint group_total = simd_sum(need);
  uint group_base  = 0;
  if (simd_is_first()) {
    group_base = atomic_fetch_add_explicit(next_count, group_total, memory_order_relaxed);
  }
  group_base = simd_broadcast_first(group_base);
  uint my_base = group_base + lane_offset;
  for (uint i = 0; i < need; i++) {
    next_frontier[my_base + i] = slots[i];
  }

  // --- SIMD-cooperative interaction + bailout reporting ---
  uint group_itrs = simd_sum(local_itrs);
  if (simd_is_first() && group_itrs > 0) {
    atomic_fetch_add_explicit(itr_count, group_itrs, memory_order_relaxed);
  }
  uint group_bailouts = simd_sum(local_bailouts);
  if (simd_is_first() && group_bailouts > 0) {
    atomic_fetch_add_explicit(itr_count + 1, group_bailouts, memory_order_relaxed);
  }
}
