#include <metal_stdlib>
using namespace metal;

// ============================================================================
// HVM4 Metal - WNF Evaluator and Normalization Kernel
// ============================================================================
//
// Implements WNF (Weak Normal Form) for HVM4's Interaction Calculus on Metal.
// Base runtime terms plus REF/ALO support for static definition books.
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
constant uchar TAG_ALO = 7;
constant uchar TAG_REF = 8;
constant uchar TAG_DRY = 10;
constant uchar TAG_ERA = 11;
constant uchar TAG_MAT = 15;
constant uchar TAG_NUM = 30;
constant uchar TAG_SWI = 31;
constant uchar TAG_USE = 32;
constant uchar TAG_OP2 = 33;
constant uchar TAG_DSU = 34;
constant uchar TAG_DDU = 35;
constant uchar TAG_EQL = 36;
constant uchar TAG_AND = 37;
constant uchar TAG_OR  = 38;
constant uchar TAG_INC = 41;
constant uchar TAG_BJV = 42;
constant uchar TAG_BJ0 = 43;
constant uchar TAG_BJ1 = 44;

// --- OP2 operation codes (EXT field of OP2) ---
constant uint OP_ADD = 0;
constant uint OP_SUB = 1;
constant uint OP_MUL = 2;
constant uint OP_DIV = 3;
constant uint OP_MOD = 4;
constant uint OP_AND = 5;
constant uint OP_OR  = 6;
constant uint OP_XOR = 7;
constant uint OP_LSH = 8;
constant uint OP_RSH = 9;
constant uint OP_NOT = 10; // unary: Op2(OP_NOT, 0, x)
constant uint OP_EQ  = 11;
constant uint OP_NE  = 12;
constant uint OP_LT  = 13;
constant uint OP_LE  = 14;
constant uint OP_GT  = 15;
constant uint OP_GE  = 16;

// --- Bit layout: [63:SUB] [62-56:TAG 7b] [55-32:EXT 24b] [31-0:VAL 32b] ---
constant ulong SUB_BIT = 1UL << 63;

// --- Flags ---
constant uint LAM_ERA_MASK = 0x800000;

// --- Breadcrumb tags for pointer-reversal WNF (frames stored in heap) ---
constant uchar TAG_BC_APP = 46;   // APP breadcrumb (fun slot)
constant uchar TAG_BC_DP0 = 47;   // DP0 breadcrumb (shared expr slot)
constant uchar TAG_BC_DP1 = 48;   // DP1 breadcrumb (shared expr slot)
constant uchar TAG_BC_OP2 = 49;   // OP2 breadcrumb (lhs slot)
constant uchar TAG_BC_OP2_NUM = 50; // OP2-NUM breadcrumb (rhs slot)
constant uint  BC_SENTINEL = 0xFFFFFFFF;

// --- Limits ---
constant uint WNF_MAX_ITERS = ~0u; // max uint32 (~4.3G iters per WNF call)

// --- Kernel parameters (set by host each pass) ---
struct Params {
  uint frontier_count;
  uint book_count;
  uint book_heap_words;
  uint shard_base;
  uint shard_words;
  uint _pad0;
  uint _pad1;
  uint _pad2;
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
    case TAG_DRY: return 2;
    case TAG_MAT: return 2;
    case TAG_SWI: return 2;
    case TAG_USE: return 1;
    case TAG_OP2: return 2;
    case TAG_DSU: return 3;
    case TAG_DDU: return 3;
    case TAG_EQL: return 2;
    case TAG_AND: return 2;
    case TAG_OR:  return 2;
    case TAG_INC: return 1;
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
inline Term term_new_ref(uint nam)           { return term_new(0, TAG_REF, nam, 0); }
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

inline Term term_new_op2_at(device ulong* heap, uint loc,
                            uint opr, Term x, Term y) {
  heap_set(heap, loc + 0, x);
  heap_set(heap, loc + 1, y);
  return term_new(0, TAG_OP2, opr, loc);
}

inline Term term_new_op2(device ulong* heap, thread Alloc& alloc,
                         uint opr, Term x, Term y) {
  return term_new_op2_at(heap, heap_alloc(alloc, 2), opr, x, y);
}

inline Term term_new_dup_at(device ulong* heap, uint loc,
                            uint lab, Term val, Term bod) {
  heap_set(heap, loc + 0, val);
  heap_set(heap, loc + 1, bod);
  return term_new(0, TAG_DUP, lab, loc);
}

inline Term term_new_dup(device ulong* heap, thread Alloc& alloc,
                         uint lab, Term val, Term bod) {
  return term_new_dup_at(heap, heap_alloc(alloc, 2), lab, val, bod);
}

// ============================================================================
// Clone (creates DP0/DP1 pair pointing to same location)
// ============================================================================

inline Copy term_clone_at(uint loc, uint lab) {
  return Copy { term_new_dp0(lab, loc), term_new_dp1(lab, loc) };
}

// ============================================================================
// REF/ALO helpers
// ============================================================================

inline uint book_lookup(uint nam, uint book_count, device uint* book) {
  if (nam >= book_count) return 0;
  return book[nam];
}

inline uint alo_bind_lookup(device ulong* heap, uint ls, uint len, uint lvl) {
  if (lvl == 0 || lvl > len) return 0;
  uint idx = len - lvl;
  uint it = ls;
  for (uint i = 0; i < idx && it != 0; i++) {
    it = (uint)(heap_read(heap, it) & 0xFFFFFFFF);
  }
  return it != 0 ? (uint)(heap_read(heap, it) >> 32) : 0;
}

inline Term term_new_alo(device ulong* heap, thread Alloc& alloc,
                         uint len, uint ls_loc, uint tm_loc) {
  uint alo_loc = heap_alloc(alloc, 1);
  heap_set(heap, alo_loc, ((ulong)ls_loc << 32) | tm_loc);
  return term_new(0, TAG_ALO, len, alo_loc);
}

inline Term wnf_alo_var(device ulong* heap, uint ls, uint len, uint lvl, uchar tag) {
  uint bind = alo_bind_lookup(heap, ls, len, lvl);
  return bind ? term_new_var(bind) : term_new(0, tag, 0, lvl);
}

inline Term wnf_alo_cop(device ulong* heap, uint ls, uint len, uint lvl,
                        uint lab, uint side, uchar tag) {
  uint bind = alo_bind_lookup(heap, ls, len, lvl);
  uchar out_tag = side == 0 ? TAG_DP0 : TAG_DP1;
  return bind ? term_new(0, out_tag, lab, bind) : term_new(0, tag, lab, lvl);
}

inline Term wnf_alo_lam(device ulong* heap, thread Alloc& alloc,
                        uint ls_loc, uint len, uint lam_ext, uint body_loc) {
  uint lam_body = heap_alloc(alloc, 1);
  uint bind_ent = heap_alloc(alloc, 1);
  heap_set(heap, bind_ent, ((ulong)lam_body << 32) | ls_loc);
  uint alo_loc = heap_alloc(alloc, 1);
  heap_set(heap, alo_loc, ((ulong)bind_ent << 32) | body_loc);
  heap_set(heap, lam_body, term_new(0, TAG_ALO, len + 1, alo_loc));
  return term_new(0, TAG_LAM, lam_ext, lam_body);
}

inline Term wnf_alo_dup(device ulong* heap, thread Alloc& alloc,
                        uint ls_loc, uint len, uint book_loc, uint lab) {
  uint dup_term_val = heap_alloc(alloc, 1);
  uint bind_ent = heap_alloc(alloc, 1);
  heap_set(heap, bind_ent, ((ulong)dup_term_val << 32) | ls_loc);
  Term alo0 = term_new_alo(heap, alloc, len, ls_loc, book_loc + 0);
  heap_set(heap, dup_term_val, alo0);
  Term alo1 = term_new_alo(heap, alloc, len, ls_loc, book_loc + 0);
  Term alo2 = term_new_alo(heap, alloc, len + 1, bind_ent, book_loc + 1);
  return term_new_dup(heap, alloc, lab, alo1, alo2);
}

inline Term wnf_alo_nod(device ulong* heap, thread Alloc& alloc,
                        uint ls_loc, uint len, uint loc, uchar tag, uint ext, uint ari) {
  if (ari == 0) {
    return term_new(0, tag, ext, 0);
  }
  uint out_loc = heap_alloc(alloc, ari);
  for (uint i = 0; i < ari; i++) {
    heap_set(heap, out_loc + i, term_new_alo(heap, alloc, len, ls_loc, loc + i));
  }
  return term_new(0, tag, ext, out_loc);
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

// (#a op #b) -> #(a op b)
inline Term wnf_op2_num_num_raw(uint opr, uint a, uint b) {
  if (opr == OP_SUB) {
    return term_new_num(a - b);
  }
  uint result = 0;
  switch (opr) {
    case OP_ADD: result = a + b; break;
    case OP_SUB: result = a - b; break;
    case OP_MUL: result = a * b; break;
    case OP_DIV: result = b != 0 ? a / b : 0; break;
    case OP_MOD: result = b != 0 ? a % b : 0; break;
    case OP_AND: result = a & b; break;
    case OP_OR:  result = a | b; break;
    case OP_XOR: result = a ^ b; break;
    case OP_LSH: result = a << b; break;
    case OP_RSH: result = a >> b; break;
    case OP_NOT: result = ~b; break;
    case OP_EQ:  result = a == b ? 1u : 0u; break;
    case OP_NE:  result = a != b ? 1u : 0u; break;
    case OP_LT:  result = a < b ? 1u : 0u; break;
    case OP_LE:  result = a <= b ? 1u : 0u; break;
    case OP_GT:  result = a > b ? 1u : 0u; break;
    case OP_GE:  result = a >= b ? 1u : 0u; break;
    default:     result = 0; break;
  }
  return term_new_num(result);
}

inline Term wnf_op2_era() {
  return term_new_era();
}

inline Term wnf_op2_num_era() {
  return term_new_era();
}

// @@opr(&L{a,b}, y) -> &L{@@opr(a,Y0), @@opr(b,Y1)}
inline Term wnf_op2_sup(device ulong* heap, thread Alloc& alloc,
                        uint opr, Term sup, Term y) {
  uint lab     = term_ext(sup);
  uint sup_loc = term_val(sup);
  uint y_loc   = heap_alloc(alloc, 1);
  heap_set(heap, y_loc, y);
  Copy Y       = term_clone_at(y_loc, lab);
  Term op0     = term_new_op2(heap, alloc, opr, heap_read(heap, sup_loc + 0), Y.k0);
  Term op1     = term_new_op2(heap, alloc, opr, heap_read(heap, sup_loc + 1), Y.k1);
  return term_new_sup(heap, alloc, lab, op0, op1);
}

// (x op &L{a,b}) where x is NUM -> &L{(X0 op a), (X1 op b)}
inline Term wnf_op2_num_sup(device ulong* heap, thread Alloc& alloc,
                            uint opr, Term x, Term sup) {
  uint lab     = term_ext(sup);
  uint sup_loc = term_val(sup);
  uint x_loc   = heap_alloc(alloc, 1);
  heap_set(heap, x_loc, x);
  Copy X       = term_clone_at(x_loc, lab);
  Term op0     = term_new_op2(heap, alloc, opr, X.k0, heap_read(heap, sup_loc + 0));
  Term op1     = term_new_op2(heap, alloc, opr, X.k1, heap_read(heap, sup_loc + 1));
  return term_new_sup(heap, alloc, lab, op0, op1);
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
         device uint* book, device ulong* book_heap, uint book_count,
         uint book_heap_words,
         Term term, thread Alloc& alloc, thread uint& itrs, thread uint& bailouts) {
  // Unified breadcrumb chain: both APP and DP frames stored in heap.
  // Zero thread-local stack → maximum GPU occupancy.
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

        // Write DP breadcrumb into the shared expression slot.
        // Encodes: side (via tag), label (ext), previous breadcrumb (val).
        uchar bc_tag = (tag == TAG_DP0) ? TAG_BC_DP0 : TAG_BC_DP1;
        heap_set(heap, loc, term_new(0, bc_tag, term_ext(cur), prev));
        prev = loc;
        cur = taken;
        continue;
      }

      if (tag == TAG_APP) {
        uint app_loc = term_val(cur);
        Term fun = heap_read(heap, app_loc);
        // Write APP breadcrumb into function slot (pointer reversal)
        heap_set(heap, app_loc, term_new(0, TAG_BC_APP, 0, prev));
        prev = app_loc;
        cur = fun;
        continue;
      }

      if (tag == TAG_OP2) {
        uint op2_loc = term_val(cur);
        Term x = heap_read(heap, op2_loc + 0);
        // Write OP2 breadcrumb into lhs slot (pointer reversal)
        heap_set(heap, op2_loc + 0, term_new(0, TAG_BC_OP2, term_ext(cur), prev));
        prev = op2_loc + 0;
        cur = x;
        continue;
      }

      if (tag == TAG_DUP) {
        uint loc = term_val(cur);
        cur = heap_read(heap, loc + 1);
        continue;
      }

      if (tag == TAG_REF) {
        uint nam = term_ext(cur);
        uint book_loc = book_lookup(nam, book_count, book);
        if (book_loc != 0) {
          uint alo_loc = heap_alloc(alloc, 1);
          heap_set(heap, alo_loc, (ulong)book_loc);
          cur = term_new(0, TAG_ALO, 0, alo_loc);
          continue;
        }
        entering = false;
        continue;
      }

      if (tag == TAG_ALO) {
        uint alo_loc = term_val(cur);
        ulong pair = heap_read(heap, alo_loc);
        uint tm_loc = (uint)(pair & 0xFFFFFFFF);
        uint ls_loc = (uint)(pair >> 32);
        uint len = term_ext(cur);
        if (tm_loc >= book_heap_words) {
          entering = false;
          continue;
        }
        Term book_tm = book_heap[tm_loc];
        uchar b_tag = term_tag(book_tm);

        if (b_tag == TAG_VAR || b_tag == TAG_BJV) {
          cur = wnf_alo_var(heap, ls_loc, len, term_val(book_tm), b_tag);
          continue;
        }
        if (b_tag == TAG_DP0 || b_tag == TAG_DP1 || b_tag == TAG_BJ0 || b_tag == TAG_BJ1) {
          uint side = (b_tag == TAG_DP0 || b_tag == TAG_BJ0) ? 0 : 1;
          cur = wnf_alo_cop(heap, ls_loc, len, term_val(book_tm), term_ext(book_tm), side, b_tag);
          continue;
        }
        if (b_tag == TAG_LAM) {
          cur = wnf_alo_lam(heap, alloc, ls_loc, len, term_ext(book_tm), term_val(book_tm));
          continue;
        }
        if (b_tag == TAG_DUP) {
          cur = wnf_alo_dup(heap, alloc, ls_loc, len, term_val(book_tm), term_ext(book_tm));
          continue;
        }
        uint b_ari = term_arity(b_tag);
        if (b_ari > 0) {
          cur = wnf_alo_nod(heap, alloc, ls_loc, len, term_val(book_tm), b_tag,
                            term_ext(book_tm), b_ari);
          continue;
        }
        cur = book_tm;
        continue;
      }

      // LAM, SUP, ERA, NUM -> WHNF
      entering = false;
      continue;
    }

    // ==== APPLY PHASE ====
    // Single unified breadcrumb chain: read tag to determine frame type.
    if (prev == BC_SENTINEL) {
      return cur;
    }

    Term bc     = heap_read(heap, prev);
    uchar bc_tag = term_tag(bc);
    uint parent = term_val(bc);

    if (bc_tag == TAG_BC_DP0 || bc_tag == TAG_BC_DP1) {
      // --- DP breadcrumb ---
      uint side = (bc_tag == TAG_BC_DP0) ? 0 : 1;
      uint loc  = prev;
      uint lab  = term_ext(bc);
      uchar w_tag = term_tag(cur);
      prev = parent;

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
      // Stuck DP: relocate and substitute
      uint new_loc = heap_alloc(alloc, 1);
      heap_set(heap, new_loc, cur);
      heap_subst_var(heap, loc,
        term_new(0, side == 0 ? TAG_DP1 : TAG_DP0, lab, new_loc));
      cur = term_new(0, side == 0 ? TAG_DP0 : TAG_DP1, lab, new_loc);
      continue;
    }

    if (bc_tag == TAG_BC_APP) {
      // --- APP breadcrumb ---
      uint app_loc = prev;
      Term arg     = heap_read(heap, app_loc + 1);
      uchar w_tag  = term_tag(cur);
      prev = parent;

      if (w_tag == TAG_LAM) {
        itrs++;
        cur = wnf_app_lam(heap, cur, arg);
        entering = true;
        continue;
      }
      if (w_tag == TAG_SUP) {
        itrs++;
        Term app_term = term_new(0, TAG_APP, 0, app_loc);
        cur = wnf_app_sup(heap, alloc, app_term, cur);
        continue;
      }
      if (w_tag == TAG_ERA) {
        itrs++;
        cur = wnf_app_era();
        continue;
      }
      // Stuck APP: restore function slot and rebuild
      heap_set(heap, app_loc, cur);
      cur = term_new(0, TAG_APP, 0, app_loc);
      continue;
    }

    if (bc_tag == TAG_BC_OP2) {
      // --- OP2 breadcrumb (x reduced): (x op y) ---
      uint op2_loc = prev;
      uint opr     = term_ext(bc);
      Term y       = heap_read(heap, op2_loc + 1);
      uchar w_tag  = term_tag(cur);
      prev = parent;

      if (w_tag == TAG_ERA) {
        itrs++;
        cur = wnf_op2_era();
        continue;
      }
      if (w_tag == TAG_NUM) {
        uchar y_tag = term_tag(y);
        if (y_tag == TAG_NUM) {
          itrs++;
          cur = wnf_op2_num_num_raw(opr, term_val(cur), term_val(y));
          continue;
        }
        // x is NUM, now reduce y
        heap_set(heap, op2_loc + 0, cur);
        heap_set(heap, op2_loc + 1, term_new(0, TAG_BC_OP2_NUM, opr, prev));
        prev = op2_loc + 1;
        cur = y;
        entering = true;
        continue;
      }
      if (w_tag == TAG_SUP) {
        itrs++;
        cur = wnf_op2_sup(heap, alloc, opr, cur, y);
        continue;
      }

      // Stuck OP2: restore lhs slot and rebuild
      heap_set(heap, op2_loc + 0, cur);
      cur = term_new(0, TAG_OP2, opr, op2_loc);
      continue;
    }

    if (bc_tag == TAG_BC_OP2_NUM) {
      // --- OP2-NUM breadcrumb (y reduced): (x op y), x is NUM ---
      uint op2_loc = prev - 1;
      uint opr     = term_ext(bc);
      Term x       = heap_read(heap, op2_loc + 0);
      uchar w_tag  = term_tag(cur);
      prev = parent;

      if (w_tag == TAG_ERA) {
        itrs++;
        cur = wnf_op2_num_era();
        continue;
      }
      if (w_tag == TAG_NUM) {
        itrs++;
        cur = wnf_op2_num_num_raw(opr, term_val(x), term_val(cur));
        continue;
      }
      if (w_tag == TAG_SUP) {
        itrs++;
        cur = wnf_op2_num_sup(heap, alloc, opr, x, cur);
        continue;
      }

      // Stuck OP2-NUM: restore rhs slot and rebuild
      heap_set(heap, op2_loc + 1, cur);
      cur = term_new(0, TAG_OP2, opr, op2_loc);
      continue;
    }

    // Unknown breadcrumb (shouldn't happen)
    return cur;
  }

  // Hit iteration limit -- unwind all remaining breadcrumbs
  bailouts++;
  while (prev != BC_SENTINEL) {
    Term bc = heap_read(heap, prev);
    uchar bc_tag = term_tag(bc);
    uint parent = term_val(bc);

    if (bc_tag == TAG_BC_DP0 || bc_tag == TAG_BC_DP1) {
      // Restore the partial value into the DP slot, return the DP term
      uint loc = prev;
      uint lab = term_ext(bc);
      uchar dp_tag = (bc_tag == TAG_BC_DP0) ? TAG_DP0 : TAG_DP1;
      heap_set(heap, loc, cur);
      cur = term_new(0, dp_tag, lab, loc);
    } else if (bc_tag == TAG_BC_OP2) {
      uint op2_loc = prev;
      uint opr     = term_ext(bc);
      Term y       = heap_read(heap, op2_loc + 1);
      cur = term_new_op2_at(heap, op2_loc, opr, cur, y);
    } else if (bc_tag == TAG_BC_OP2_NUM) {
      uint op2_loc = prev - 1;
      uint opr     = term_ext(bc);
      Term x       = heap_read(heap, op2_loc + 0);
      cur = term_new_op2_at(heap, op2_loc, opr, x, cur);
    } else {
      // APP breadcrumb: restore APP node
      Term arg = heap_read(heap, prev + 1);
      cur = term_new_app_at(heap, prev, cur, arg);
    }
    prev = parent;
  }
  return cur;
}

// ============================================================================
// wnf_at: reduce term at heap location, write back if changed
// ============================================================================

inline Term wnf_at(device ulong* heap, device atomic_uint* locks,
                   device uint* book, device ulong* book_heap, uint book_count,
                   uint book_heap_words,
                   uint loc, thread Alloc& alloc, thread uint& itrs, thread uint& bailouts) {
  Term cur = heap_read(heap, loc);
  uchar tag = term_tag(cur);

  // Fast path: already WHNF
  if (tag == TAG_LAM || tag == TAG_SUP || tag == TAG_ERA || tag == TAG_NUM) {
    return cur;
  }

  Term res = wnf(heap, locks, book, book_heap, book_count,
                 book_heap_words,
                 cur, alloc, itrs, bailouts);
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
  device uint*          book           [[buffer(8)]],
  device ulong*         book_heap      [[buffer(9)]],
  uint                  tid            [[thread_position_in_grid]],
  uint                  grid_size      [[threads_per_grid]])
{
  uint frontier_count = params.frontier_count;
  if (frontier_count == 0 || frontier_count > (1u << 23)) {
    return;
  }
  uint book_count = params.book_count;
  uint book_heap_words = params.book_heap_words;

  uint local_itrs = 0;
  uint local_bailouts = 0;

  // --- Per-thread shard allocator ---
  // Each dispatched thread gets an equal pre-reserved local chunk:
  // [shard_base + tid*shard_words, shard_base + (tid+1)*shard_words).
  // If exhausted, heap_alloc falls back to the global atomic cursor.
  uint shard_pos = params.shard_base + tid * params.shard_words;
  Alloc alloc = params.shard_words == 0
    ? Alloc{ 0, 0, alloc_cursor }
    : Alloc{ shard_pos, shard_pos + params.shard_words, alloc_cursor };

  // --- Thread coarsening: each thread processes multiple frontier entries ---
  for (uint fi = tid; fi < frontier_count; fi += grid_size) {

    // --- WNF at frontier location ---
    uint loc    = frontier[fi];
    uint prev_bailouts = local_bailouts;
    Term result = wnf_at(heap, locks, book, book_heap, book_count,
                         book_heap_words,
                         loc, alloc, local_itrs, local_bailouts);

    // --- If result is a DP, retry (another thread may have resolved it) ---
    uchar tag = term_tag(result);
    if (tag == TAG_DP0 || tag == TAG_DP1) {
      Term retry = wnf(heap, locks, book, book_heap, book_count,
                       book_heap_words,
                       result, alloc, local_itrs, local_bailouts);
      if (retry != result) {
        result = retry;
        heap_set(heap, loc, result);
        tag = term_tag(result);
      }
    }

    // --- Compute frontier entries to enqueue ---
    uint val   = term_val(result);
    uint need  = 0;
    uint slots[16];
    bool bailed = (local_bailouts > prev_bailouts);

    if (bailed && tag != TAG_LAM && tag != TAG_SUP &&
        tag != TAG_ERA && tag != TAG_NUM) {
      slots[0] = loc;
      need = 1;
    } else if (tag == TAG_DP0 || tag == TAG_DP1) {
      need = 0;
    } else {
      uint ari = term_arity(tag);
      need = ari;
      for (uint i = 0; i < ari; i++) slots[i] = val + i;
    }

    // --- Frontier enqueue ---
    // Use per-thread atomic reservation for correctness when only a subset of
    // lanes are active (small/deep frontiers).
    uint my_base = 0;
    if (need > 0) {
      my_base = atomic_fetch_add_explicit(next_count, need, memory_order_relaxed);
    }
    for (uint i = 0; i < need; i++) {
      next_frontier[my_base + i] = slots[i];
    }
  }

  // --- Report interactions + bailouts ---
  if (local_itrs > 0) {
    atomic_fetch_add_explicit(itr_count, local_itrs, memory_order_relaxed);
  }
  if (local_bailouts > 0) {
    atomic_fetch_add_explicit(itr_count + 1, local_bailouts, memory_order_relaxed);
  }
}
