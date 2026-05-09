// HVM4 Pure Runtime
// =================
// Single-file C runtime for pure Interaction Calculus programs.
// Organized in broad sections after Bend's core.ts: Types, Term, Heap,
// Parser, WNF, CNF, Eval, and CLI.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <assert.h>
#include <sys/mman.h>

// Types
// =====

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef const char *str;

typedef u64 Term;

typedef struct {
  Term k0;
  Term k1;
} Copy;

// Function Definition Macro
// =========================

#define fn static inline
//#define fn_noinline static __attribute__((noinline))

// Tags
// ====
// Hot tags first (0-7): APP, VAR, LAM, DP0, DP1, SUP, DUP, ALO

#define APP  0
#define VAR  1
#define LAM  2
#define DP0  3
#define DP1  4
#define SUP  5
#define DUP  6
#define ALO  7
#define REF  8
#define NAM  9
#define DRY 10
#define ERA 11
#define MAT 12
#define C00 13
#define C01 14
#define C02 15
#define C03 16
#define C04 17
#define C05 18
#define C06 19
#define C07 20
#define C08 21
#define C09 22
#define C10 23
#define C11 24
#define C12 25
#define C13 26
#define C14 27
#define C15 28
#define C16 29
#define NUM 30
#define SWI 31  // Same as MAT but for numbers (for parser/printer distinction)
#define USE 32
#define OP2 33  // Op2(opr, x, y): strict on x, then y
#define DSU 34  // DSu(lab, a, b): strict on lab, creates SUP
#define DDU 35  // DDu(lab, val, bod): strict on lab, creates DUP term
#define EQL 36  // Eql(a, b): structural equality, strict on a, then b
#define AND 37  // And(a, b): short-circuit AND, strict on a only
#define OR  38  // Or(a, b): short-circuit OR, strict on a only
#define UNS 39  // Unscoped(xf, xv): binds an unscoped lambda/var pair to xf and xv
#define ANY 40  // Any: wildcard that duplicates itself and equals anything
#define INC 41  // Inc(x): priority wrapper for collapse ordering - decreases priority
#define BJV 42  // Bjv(n): quoted lambda-bound variable (de Bruijn level)
#define BJ0 43  // Bj0(n): quoted dup-bound variable (side 0, de Bruijn level)
#define BJ1 44  // Bj1(n): quoted dup-bound variable (side 1, de Bruijn level)
#define CLO 45  // Compiled closure waiting to be materialized into the graph

// LAM Ext Flags
// =============
#define LAM_ERA_MASK 0x800000u  // binder unused in lambda body

// Stack frame tags (0x40+) - internal to WNF, encode reduction state
// Note: regular term tags (APP, MAT, USE, DP0, DP1, OP2, DSU, DDU) also used as frames
// These frames reuse existing heap nodes to avoid allocation
#define F_OP2_NUM     0x43  // (x op □): ext=opr, val=x_num_val
#define F_EQL_L       0x44  // (□ === b): val=eql_loc, b at HEAP[eql_loc+1]
#define F_EQL_R       0x45  // (a === □): val=eql_loc, a stored at HEAP[eql_loc]
#define F_ALO_MAT     0x46  // (@{s} λ{...} □): ext=len, val=reusable APP loc

// Operation codes (stored in EXT field of OP2)
#define OP_ADD 0
#define OP_SUB 1
#define OP_MUL 2
#define OP_DIV 3
#define OP_MOD 4
#define OP_AND 5
#define OP_OR  6
#define OP_XOR 7
#define OP_LSH 8
#define OP_RSH 9
#define OP_NOT 10  // unary: Op2(OP_NOT, 0, x)
#define OP_EQ  11
#define OP_NE  12
#define OP_LT  13
#define OP_LE  14
#define OP_GT  15
#define OP_GE  16

// Bit Layout
// ==========

// The high bit of the tag byte is the SUB flag. `term_tag()` returns only the
// low 7 constructor bits, keeping the public tag space unchanged.
#define TAG_BITS 8
#define EXT_BITS 24
#define VAL_BITS 32
#define TAG_SHIFT 56
#define EXT_SHIFT 32
#define VAL_SHIFT 0

#define TAG_SUB_MASK (1u << (TAG_BITS - 1))
#define TAG_MASK     (TAG_SUB_MASK - 1u)
#define EXT_MASK     ((1u << EXT_BITS) - 1u)
#define VAL_MASK     ((u64)UINT32_MAX)

// Packed ALO pair node (1 word):
// - high 32 bits: bind-list head location
// - low 32 bits: static/book term location
#define ALO_TM_BITS 32
#define ALO_TM_MASK VAL_MASK
#define ALO_LS_MASK VAL_MASK

// Capacities
// ==========

#define HEAP_CAP (1ULL << VAL_BITS)
#define BOOK_CAP (1ULL << EXT_BITS)
#define WNF_CAP  (1ULL << 32)
#define HEAP_FREE_MAX 16

// Heap Globals
// ============

static Term *HEAP;
static u64   HEAP_NEXT = 1;
static u64   HEAP_FREE[HEAP_FREE_MAX + 1];

// Book Globals
// ============

static u64 *BOOK;

// WNF Globals
// ===========

typedef struct __attribute__((aligned(256))) {
  Term *stack;
  u64   stack_bytes;
  u32   s_pos;
  u8    stack_mmap;
} WnfBank;

static WnfBank WNF_BANK = {0};
static u64 ITRS = 0;
#define WNF_STACK (WNF_BANK.stack)
#define WNF_S_POS (WNF_BANK.s_pos)
static int ITRS_ENABLED = 1;
#define ITRS_INC(name) \
  do { \
    if (__builtin_expect(ITRS_ENABLED != 0, 1)) { \
      ITRS++; \
      if (__builtin_expect(STEPS_ITRS_LIM != 0, 0)) { \
        STEPS_LAST_ITR = (name); \
      } \
    } \
  } while (0)
static u32 FRESH = 1;

static int DEBUG          = 0;
static int SILENT         = 0;
static int STEPS_ENABLE   = 0;
static u64 STEPS_ITRS_LIM = 0;
static u64 STEPS_ROOT_LOC = 0;
static str STEPS_LAST_ITR = NULL;

#ifdef HVM_PROFILE
static u64 PROF_ENTER[TAG_MASK + 1];
static u64 PROF_FRAME[TAG_MASK + 1];
static u64 PROF_ALO_BOOK[TAG_MASK + 1];
static u64 PROF_ALO_BOOK_CLOSED[TAG_MASK + 1];
static u64 PROF_ALO_BOOK_OPEN[TAG_MASK + 1];
static u64 PROF_APP_WHNF[TAG_MASK + 1];
static u64 PROF_MAT_WHNF[TAG_MASK + 1];
static u64 PROF_ALLOC[HEAP_FREE_MAX + 1];
static u64 PROF_FREE[HEAP_FREE_MAX + 1];
#define PROF_INC(arr, idx) do { (arr)[(idx) & TAG_MASK]++; } while (0)
#define PROF_SIZE(arr, size) do { if ((size) <= HEAP_FREE_MAX) { (arr)[size]++; } } while (0)
#else
#define PROF_INC(arr, idx) do { } while (0)
#define PROF_SIZE(arr, size) do { } while (0)
#endif

// Nick Alphabet
// =============

static const char *nick_alphabet = "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$";

// Parser Types
// ============

typedef struct {
  char *file;
  char *src;
  u32   pos;
  u32   len;
  u32   line;
  u32   col;
} PState;

typedef struct {
  u32 name;
  u32 lvl;
  u32 lab;
  u32 forked;  // 1 if this is a fork variable
  u32 cloned;  // 1 if this is a cloned variable (λ&x or ! &x = v)
  int side;    // forced dup side for destructured dup aliases, -1 otherwise
} PBind;

// Parser Globals
// ==============

static char  *PARSE_SEEN_FILES[1024];
static u32    PARSE_SEEN_FILES_LEN = 0;
static PBind  PARSE_BINDS[16384];
static u32    PARSE_BINDS_LEN = 0;
static u32    PARSE_FRESH_LAB = 0x800000u; // start near top of 24-bit label space
static int    PARSE_FORK_SIDE = -1;      // -1 = off, 0 = left branch (DP0), 1 = right branch (DP1)
#define PARSE_DYN_LAB EXT_MASK

// Term
// ====

fn Term term_new(u8 sub, u8 tag, u32 ext, u64 val) {
  u64 tag_byte = ((u64)(tag & TAG_MASK)) | ((u64)(sub != 0) << 7);
  return (tag_byte << TAG_SHIFT)
       | ((u64)(ext & EXT_MASK) << EXT_SHIFT)
       | ((u64)(val & VAL_MASK));
}

fn u8 term_sub_get(Term t) {
  return ((t >> TAG_SHIFT) & TAG_SUB_MASK) != 0;
}

fn Term term_sub_set(Term t, u8 sub) {
  Term mask = ((u64)TAG_SUB_MASK) << TAG_SHIFT;
  return sub != 0 ? (t | mask) : (t & ~mask);
}

fn u8 term_tag(Term t) {
  return (t >> TAG_SHIFT) & TAG_MASK;
}

fn u32 term_ext(Term t) {
  return (t >> EXT_SHIFT) & EXT_MASK;
}

fn u32 term_val(Term t) {
  return (u32)((t >> VAL_SHIFT) & VAL_MASK);
}

static const u8 TERM_ARITY[TAG_MASK + 1] = {
  [APP] = 2,
  [VAR] = 0,
  [LAM] = 1,
  [DP0] = 0,
  [DP1] = 0,
  [SUP] = 2,
  [DUP] = 2,
  [ALO] = 0,
  [REF] = 0,
  [NAM] = 0,
  [DRY] = 2,
  [ERA] = 0,
  [MAT] = 2,
  [C00] = 0,
  [C01] = 1,
  [C02] = 2,
  [C03] = 3,
  [C04] = 4,
  [C05] = 5,
  [C06] = 6,
  [C07] = 7,
  [C08] = 8,
  [C09] = 9,
  [C10] = 10,
  [C11] = 11,
  [C12] = 12,
  [C13] = 13,
  [C14] = 14,
  [C15] = 15,
  [C16] = 16,
  [NUM] = 0,
  [SWI] = 2,
  [USE] = 1,
  [OP2] = 2,
  [DSU] = 3,
  [DDU] = 3,
  [EQL] = 2,
  [AND] = 2,
  [OR] = 2,
  [UNS] = 1,
  [ANY] = 0,
  [INC] = 1,
  [BJV] = 0,
  [BJ0] = 0,
  [BJ1] = 0,
  [CLO] = 0,
};

fn u32 term_arity(Term t) {
  u8 tag = term_tag(t);
  return TERM_ARITY[tag];
}

// Heap
// ====

fn u64 heap_alloc(u64 size) {
  PROF_SIZE(PROF_ALLOC, size);
  if (__builtin_expect(size > 0 && size <= HEAP_FREE_MAX, 1)) {
    u64 at = HEAP_FREE[size];
    if (__builtin_expect(at != 0, 0)) {
      HEAP_FREE[size] = (u64)HEAP[at];
      return at;
    }
  }
  u64 at   = HEAP_NEXT;
  u64 next = at + size;
  if (__builtin_expect(next <= HEAP_CAP && next >= at, 1)) {
    HEAP_NEXT = next;
    return at;
  }
  fprintf(stderr, "Out of heap memory (need %llu words)\n", (unsigned long long)size);
  exit(1);
}

fn void heap_free(u64 loc, u64 size) {
  if (__builtin_expect(loc == 0 || size == 0 || size > HEAP_FREE_MAX, 0)) {
    return;
  }
  PROF_SIZE(PROF_FREE, size);
  HEAP[loc] = (Term)HEAP_FREE[size];
  HEAP_FREE[size] = loc;
}

fn u64 heap_alloc_total(void) {
  return HEAP_NEXT > 0 ? HEAP_NEXT - 1 : 0;
}

fn Term heap_read(u64 loc) {
  return HEAP[loc];
}

fn Term heap_take(u64 loc) {
  Term term = HEAP[loc];
  if (__builtin_expect(term != 0, 1)) {
    return term;
  }
  fprintf(stderr, "ERROR: heap_take saw zero at %llu\n", (unsigned long long)loc);
  abort();
}

fn void heap_set(u64 loc, Term val) {
  HEAP[loc] = val;
}

fn void heap_free_term(Term term) {
  if (term_tag(term) == ALO && term_ext(term) > 0) {
    heap_free(term_val(term), 1);
    return;
  }
  u32 size = term_arity(term);
  if (size != 0) {
    heap_free(term_val(term), size);
  }
}

// Term Constructors
// =================

fn Term term_new_at(u64 loc, u8 tag, u32 ext, u32 ari, Term *args) {
  for (u32 i = 0; i < ari; i++) {
    heap_set(loc + i, args[i]);
  }
  return term_new(0, tag, ext, loc);
}

fn Term term_new_(u8 tag, u32 ext, u32 ari, Term *args) {
  return term_new_at(heap_alloc(ari), tag, ext, ari, args);
}

fn Term term_new_nam(u32 nam) {
  return term_new(0, NAM, nam, 0);
}

fn Term term_new_alo(u64 ls_loc, u32 len, u64 tm_loc) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }
  u64 alo_loc = heap_alloc(1);
  heap_set(alo_loc, ((ls_loc & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK));
  return term_new(0, ALO, len, alo_loc);
}

fn Term term_new_alo_at(u64 alo_loc, u64 ls_loc, u32 len, u64 tm_loc) {
  if (len == 0) {
    return term_new(0, ALO, 0, tm_loc);
  }
  heap_set(alo_loc, ((ls_loc & ALO_LS_MASK) << ALO_TM_BITS) | (tm_loc & ALO_TM_MASK));
  return term_new(0, ALO, len, alo_loc);
}

fn Term term_new_dry_at(u64 loc, Term fun, Term arg) {
  heap_set(loc + 0, fun);
  heap_set(loc + 1, arg);
  return term_new(0, DRY, 0, loc);
}

fn Term term_new_dry(Term fun, Term arg) {
  return term_new_dry_at(heap_alloc(2), fun, arg);
}

fn Term term_new_var(u64 loc) {
  return term_new(0, VAR, 0, loc);
}

fn Term term_new_ref(u32 nam) {
  return term_new(0, REF, nam, 0);
}

fn Term term_new_era(void) {
  return term_new(0, ERA, 0, 0);
}

fn Term term_new_any(void) {
  return term_new(0, ANY, 0, 0);
}

fn Term term_new_dp0(u32 lab, u64 loc) {
  return term_new(0, DP0, lab, loc);
}

fn Term term_new_dp1(u32 lab, u64 loc) {
  return term_new(0, DP1, lab, loc);
}

fn Term term_new_lam_at(u64 loc, Term bod) {
  heap_set(loc, bod);
  return term_new(0, LAM, 0, loc);
}

fn Term term_new_lam(Term bod) {
  return term_new_lam_at(heap_alloc(1), bod);
}

fn Term term_new_app_at(u64 loc, Term fun, Term arg) {
  heap_set(loc + 0, fun);
  heap_set(loc + 1, arg);
  return term_new(0, APP, 0, loc);
}

fn Term term_new_app(Term fun, Term arg) {
  return term_new_app_at(heap_alloc(2), fun, arg);
}

fn Term term_new_sup_at(u64 loc, u32 lab, Term tm0, Term tm1) {
  heap_set(loc + 0, tm0);
  heap_set(loc + 1, tm1);
  return term_new(0, SUP, lab, loc);
}

fn Term term_new_sup(u32 lab, Term tm0, Term tm1) {
  return term_new_sup_at(heap_alloc(2), lab, tm0, tm1);
}

fn Term term_new_dup_at(u64 loc, u32 lab, Term val, Term bod) {
  heap_set(loc + 0, val);
  heap_set(loc + 1, bod);
  return term_new(0, DUP, lab, loc);
}

fn Term term_new_dup(u32 lab, Term val, Term bod) {
  return term_new_dup_at(heap_alloc(2), lab, val, bod);
}

fn Term term_new_mat_at(u64 loc, u32 nam, Term val, Term nxt) {
  heap_set(loc + 0, val);
  heap_set(loc + 1, nxt);
  return term_new(0, MAT, nam, loc);
}

fn Term term_new_mat(u32 nam, Term val, Term nxt) {
  return term_new_mat_at(heap_alloc(2), nam, val, nxt);
}

// SWI: λ{num: f; g} - number switch (same as MAT but for parser/printer)
fn Term term_new_swi(u32 num, Term f, Term g) {
  u64 loc = heap_alloc(2);
  heap_set(loc + 0, f);
  heap_set(loc + 1, g);
  return term_new(0, SWI, num, loc);
}

// USE: λ{f} - reduce arg and apply
// fields = [f]
fn Term term_new_use_at(u64 loc, Term f) {
  heap_set(loc, f);
  return term_new(0, USE, 0, loc);
}

fn Term term_new_use(Term f) {
  return term_new_use_at(heap_alloc(1), f);
}

fn Term term_new_ctr_at(u64 loc, u32 nam, u32 ari, Term *args) {
  if (ari == 0) {
    return term_new(0, C00, nam, 0);
  }
  if (ari == 1) {
    heap_set(loc + 0, args[0]);
    return term_new(0, C01, nam, loc);
  }
  return term_new_at(loc, C00 + ari, nam, ari, args);
}

fn Term term_new_ctr(u32 nam, u32 ari, Term *args) {
  if (ari == 0) {
    return term_new(0, C00, nam, 0);
  }
  return term_new_ctr_at(heap_alloc(ari), nam, ari, args);
}

// Op2(opr, x, y): binary operation, strict on x first
// Layout: heap_read(loc+0) = x, heap_read(loc+1) = y
// EXT field = operation code (OP_ADD, OP_MUL, etc.)
fn Term term_new_op2_at(u64 loc, u32 opr, Term x, Term y) {
  heap_set(loc + 0, x);
  heap_set(loc + 1, y);
  return term_new(0, OP2, opr, loc);
}

fn Term term_new_op2(u32 opr, Term x, Term y) {
  return term_new_op2_at(heap_alloc(2), opr, x, y);
}

// DynSup(lab, a, b): dynamic superposition, strict on lab
// Layout: heap_read(loc+0) = lab, heap_read(loc+1) = a, heap_read(loc+2) = b
fn Term term_new_dsu_at(u64 loc, Term lab, Term a, Term b) {
  heap_set(loc + 0, lab);
  heap_set(loc + 1, a);
  heap_set(loc + 2, b);
  return term_new(0, DSU, 0, loc);
}

fn Term term_new_dsu(Term lab, Term a, Term b) {
  return term_new_dsu_at(heap_alloc(3), lab, a, b);
}

// DynDup(lab, val, bod): dynamic DUP binder, strict on lab
// Layout: heap_read(loc+0) = lab, heap_read(loc+1) = val, heap_read(loc+2) = bod
fn Term term_new_ddu_at(u64 loc, Term lab, Term val, Term bod) {
  heap_set(loc + 0, lab);
  heap_set(loc + 1, val);
  heap_set(loc + 2, bod);
  return term_new(0, DDU, 0, loc);
}

fn Term term_new_ddu(Term lab, Term val, Term bod) {
  return term_new_ddu_at(heap_alloc(3), lab, val, bod);
}

// Eql(a, b): structural equality, strict on a first then b
// Layout: heap_read(loc+0) = a, heap_read(loc+1) = b
// Returns #1 if equal, #0 if not
fn Term term_new_eql_at(u64 loc, Term a, Term b) {
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  return term_new(0, EQL, 0, loc);
}

fn Term term_new_eql(Term a, Term b) {
  return term_new_eql_at(heap_alloc(2), a, b);
}

fn Term term_new_and_at(u64 loc, Term a, Term b) {
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  return term_new(0, AND, 0, loc);
}

fn Term term_new_and(Term a, Term b) {
  return term_new_and_at(heap_alloc(2), a, b);
}

// Or(a, b): short-circuit OR, strict on a only
// Layout: heap_read(loc+0) = a, heap_read(loc+1) = b
// Returns #1 if a is non-zero, b if a is zero
fn Term term_new_or_at(u64 loc, Term a, Term b) {
  heap_set(loc + 0, a);
  heap_set(loc + 1, b);
  return term_new(0, OR, 0, loc);
}

fn Term term_new_or(Term a, Term b) {
  return term_new_or_at(heap_alloc(2), a, b);
}

// UNS: ! ${f, v}; body - unscoped binding
// fields = [body]
fn Term term_new_uns(Term bod) {
  u64 loc = heap_alloc(1);
  heap_set(loc, bod);
  return term_new(0, UNS, 0, loc);
}

// INC: ↑x - priority wrapper for collapse ordering
// fields = [x]
fn Term term_new_inc(Term x) {
  u64 loc = heap_alloc(1);
  heap_set(loc, x);
  return term_new(0, INC, 0, loc);
}

fn Term term_new_num(u32 n) {
  return term_new(0, NUM, 0, n);
}

fn Copy term_clone_at(u64 loc, u32 lab) {
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

// OP2 Helpers
// ===========
//
// Applies one numeric OP2 code to two `u32` operands.

// OP2
// ---

// Computes one numeric OP2 result.
fn u32 term_op2_u32(u32 opr, u32 a, u32 b) {
  if (__builtin_expect(opr == OP_SUB, 1)) {
    return a - b;
  }

  switch (opr) {
    case OP_ADD: {
      return a + b;
    }
    case OP_SUB: {
      return a - b;
    }
    case OP_MUL: {
      return a * b;
    }
    case OP_DIV: {
      return b != 0 ? a / b : 0;
    }
    case OP_MOD: {
      return b != 0 ? a % b : 0;
    }
    case OP_AND: {
      return a & b;
    }
    case OP_OR: {
      return a | b;
    }
    case OP_XOR: {
      return a ^ b;
    }
    case OP_LSH: {
      return a << b;
    }
    case OP_RSH: {
      return a >> b;
    }
    case OP_NOT: {
      return ~b;
    }
    case OP_EQ: {
      return a == b ? 1 : 0;
    }
    case OP_NE: {
      return a != b ? 1 : 0;
    }
    case OP_LT: {
      return a < b ? 1 : 0;
    }
    case OP_LE: {
      return a <= b ? 1 : 0;
    }
    case OP_GT: {
      return a > b ? 1 : 0;
    }
    case OP_GE: {
      return a >= b ? 1 : 0;
    }
    default: {
      return 0;
    }
  }
}

// Heap Substitution
// =================

fn void heap_subst_var(u64 loc, Term val) {
  heap_set(loc, term_sub_set(val, 1));
}

fn void heap_subst_var_dup(u64 loc, Term val) {
  heap_set(loc, term_sub_set(val, 1));
}

fn Term heap_subst_cop(u8 side, u64 loc, Term r0, Term r1) {
  heap_set(loc, term_sub_set(side == 0 ? r1 : r0, 1));
  return side == 0 ? r0 : r1;
}

// Nick
// ====

fn int nick_letter_to_b64(char c) {
  if (c == '_') {
    return 0;
  }
  if (c >= 'a' && c <= 'z') {
    return 1 + (c - 'a');
  }
  if (c >= 'A' && c <= 'Z') {
    return 27 + (c - 'A');
  }
  if (c >= '0' && c <= '9') {
    return 53 + (c - '0');
  }
  if (c == '$') {
    return 63;
  }
  return -1;
}

fn char nick_b64_to_letter(int b64) {
  if (b64 == 0) {
    return '_';
  }
  if (b64 >= 1 && b64 <= 26) {
    return 'a' + (b64 - 1);
  }
  if (b64 >= 27 && b64 <= 52) {
    return 'A' + (b64 - 27);
  }
  if (b64 >= 53 && b64 <= 62) {
    return '0' + (b64 - 53);
  }
  if (b64 == 63) {
    return '$';
  }
  return '?';
}

fn void nick_to_str(u32 name, char *buf, u32 buf_size) {
  // Extract 4 characters from the 24-bit name (6 bits each)
  // Names are stored most significant first
  char tmp[5];
  int len = 0;
  for (int i = 3; i >= 0; i--) {
    int b64 = (name >> (i * 6)) & 0x3F;
    if (b64 != 0 || len > 0) {  // Skip leading underscores (zeros)
      tmp[len++] = nick_b64_to_letter(b64);
    }
  }
  if (len == 0) {
    tmp[len++] = '_';  // Empty name becomes single underscore
  }
  tmp[len] = '\0';
  // Copy to output buffer
  for (int i = 0; i < len && i < (int)buf_size - 1; i++) {
    buf[i] = tmp[i];
  }
  buf[len < (int)buf_size - 1 ? len : buf_size - 1] = '\0';
}

fn int nick_is_init(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

fn int nick_is_char(char c) {
  return nick_letter_to_b64(c) >= 0;
}

fn void sys_error(const char *msg);

fn u32 nick_from_str(const char *name, u32 len) {
  if (len == 0) {
    sys_error("Empty name in name_from_str");
  }
  if (!nick_is_init(name[0])) {
    sys_error("Invalid name in name_from_str");
  }
  u32 k = 0;
  for (u32 i = 0; i < len; i++) {
    char c = name[i];
    if (!nick_is_char(c)) {
      sys_error("Invalid name in name_from_str");
    }
    k = ((k << 6) + nick_letter_to_b64(c)) & EXT_MASK;
  }
  return k;
}

fn u32 table_find(const char *name, u32 len);

// Built-in constructor symbols (initialized at runtime).
static u32 SYM_ZER = 0;
static u32 SYM_SUC = 0;
static u32 SYM_NIL = 0;
static u32 SYM_CON = 0;
static u32 SYM_CHR = 0;

fn void symbols_init(void) {
  SYM_ZER = table_find("ZER", 3);
  SYM_SUC = table_find("SUC", 3);
  SYM_NIL = table_find("NIL", 3);
  SYM_CON = table_find("CON", 3);
  SYM_CHR = table_find("CHR", 3);
}

// System
// ======

fn void sys_error(const char *msg) {
  fprintf(stderr, "ERROR: %s\n", msg);
  exit(1);
}

fn void sys_runtime_error(const char *msg) {
  fprintf(stderr, "RUNTIME_ERROR: %s\n", msg);
  exit(1);
}

fn void sys_path_join(char *out, int size, const char *base, const char *rel) {
  if (rel[0] == '/') {
    snprintf(out, size, "%s", rel);
    return;
  }
  const char *slash = strrchr(base, '/');
  if (slash) {
    snprintf(out, size, "%.*s/%s", (int)(slash - base), base, rel);
  } else {
    snprintf(out, size, "%s", rel);
  }
}

fn char *sys_file_read(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    return NULL;
  }
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *src = malloc(len + 1);
  if (!src) {
    sys_error("OOM");
  }
  fread(src, 1, len, fp);
  src[len] = 0;
  fclose(fp);
  return src;
}

#ifndef MAP_ANONYMOUS
  #define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef MAP_NORESERVE
  #define MAP_NORESERVE 0
#endif

fn void *sys_mmap_anon(size_t bytes) {
  int   prot  = PROT_READ | PROT_WRITE;
  int   flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
  void *map   = mmap(NULL, bytes, prot, flags, -1, 0);
  if (map == MAP_FAILED) {
    return NULL;
  }
  return map;
}

fn void sys_munmap_anon(void *ptr, size_t bytes) {
  if (ptr == NULL || bytes == 0) {
    return;
  }
  munmap(ptr, bytes);
}

// Table
// =====

// Name table globals
// ==================
// Single global intern table shared by refs, defs, ctors, labels, and names.
// IDs are 24-bit and stored in EXT fields.

typedef struct {
  char **data;
  u32    len;
} NameTable;

static NameTable TABLE = {0};

fn void sys_error(const char *msg);

// Finds or creates an ID for the given name in the global table.
// If the name exists, returns its existing ID.
// If not, assigns a new unique ID and stores the name.
fn u32 table_find(const char *name, u32 len) {
  if (TABLE.data == NULL) {
    sys_error("name table not initialized");
  }
  // Linear scan for existing name
  for (u32 i = 0; i < TABLE.len; i++) {
    if (strlen(TABLE.data[i]) == len && memcmp(TABLE.data[i], name, len) == 0) {
      return i;
    }
  }
  if (TABLE.len >= BOOK_CAP) {
    sys_error("name table overflow");
  }
  // Not found - create new entry
  u32  id   = TABLE.len++;
  char *copy = malloc(len + 1);
  if (copy == NULL) {
    sys_error("Memory allocation failed");
  }
  memcpy(copy, name, len);
  copy[len] = '\0';
  TABLE.data[id] = copy;
  return id;
}

// Returns the name string for a given id, or NULL if not set.
fn char *table_get(u32 id) {
  if (TABLE.data == NULL || id >= TABLE.len) {
    return NULL;
  }
  return TABLE.data[id];
}

// Print
// =====

// Prints nick-encoded names (base64-like alphabet) used by parser/printer round-trip.
fn void print_name(FILE *f, u32 n) {
  if (n < 64) {
    fputc(nick_alphabet[n], f);
  } else {
    print_name(f, n / 64);
    fputc(nick_alphabet[n % 64], f);
  }
}

// Print a Unicode codepoint as UTF-8.
// Emits a codepoint as UTF-8 without validation; escaping helpers gate usage.
enum {
  PRINT_ESC_CHAR = 0,
  PRINT_ESC_STR  = 1,
};

fn void print_utf8(FILE *f, u32 c) {
  if (c < 0x80) {
    fputc(c, f);
  } else if (c < 0x800) {
    fputc(0xC0 | (c >> 6), f);
    fputc(0x80 | (c & 0x3F), f);
  } else if (c < 0x10000) {
    fputc(0xE0 | (c >> 12), f);
    fputc(0x80 | ((c >> 6) & 0x3F), f);
    fputc(0x80 | (c & 0x3F), f);
  } else {
    fputc(0xF0 | (c >> 18), f);
    fputc(0x80 | ((c >> 12) & 0x3F), f);
    fputc(0x80 | ((c >> 6) & 0x3F), f);
    fputc(0x80 | (c & 0x3F), f);
  }
}

// Returns true when the codepoint can be printed with escapes.
fn int print_utf8_can_escape(u32 c, u8 mode) {
  if (c > 0x10FFFF) {
    return 0;
  }
  if (c >= 0xD800 && c <= 0xDFFF) {
    return 0;
  }
  switch (c) {
    case '\n': {
      return 1;
    }
    case '\t': {
      return 1;
    }
    case '\r': {
      return 1;
    }
    case '\0': {
      return 1;
    }
    case '\\': {
      return 1;
    }
    case '\'': {
      if (mode == PRINT_ESC_CHAR) {
        return 1;
      }
      break;
    }
    case '"': {
      if (mode == PRINT_ESC_STR) {
        return 1;
      }
      break;
    }
  }
  if (c < 0x20) {
    return 0;
  }
  if (c == 0x7F) {
    return 0;
  }
  if (c >= 0x80 && c <= 0x9F) {
    return 0;
  }
  return 1;
}

// Prints an escaped codepoint when possible, returning 1 on success.
fn int print_utf8_escape(FILE *f, u32 c, u8 mode) {
  if (!print_utf8_can_escape(c, mode)) {
    return 0;
  }
  switch (c) {
    case '\n': {
      fputs("\\n", f);
      return 1;
    }
    case '\t': {
      fputs("\\t", f);
      return 1;
    }
    case '\r': {
      fputs("\\r", f);
      return 1;
    }
    case '\0': {
      fputs("\\0", f);
      return 1;
    }
    case '\\': {
      fputs("\\\\", f);
      return 1;
    }
    case '\'': {
      if (mode == PRINT_ESC_CHAR) {
        fputs("\\'", f);
        return 1;
      }
      break;
    }
    case '"': {
      if (mode == PRINT_ESC_STR) {
        fputs("\\\"", f);
        return 1;
      }
      break;
    }
  }
  print_utf8(f, c);
  return 1;
}

// Pretty-printer overview
// - Dynamic links: LAM/VAR and DP0/DP1 point to heap locations; DUP is a
//   syntactic binder; it yields a DUP node (DP0/DP1 share its expr loc).
// - Static terms (inside ALO) are immutable and use BJV/BJ0/BJ1 de Bruijn levels.
// - NAM is a literal stuck name (^x), unrelated to binders.
// - Dynamic printing assigns globally unique names to each LAM body location.
// - Dup names are keyed by the DUP node expr location and printed after the term.
// - Static printing renders quoted/book terms and applies ALO substitutions.
// - Substitutions live in heap slots with the SUB bit set; these must be
//   unmarked before printing, and print_term_at asserts this invariant.
// - Lambda names: lowercase (a, b, ..., aa, ab, ...), dup names: uppercase.
// - Quoted lambdas are tagged by LAM.ext = depth + 1 (linked LAM.ext = 0).
// - Name tables are fixed-size (PRINT_NAME_MAX) to keep the printer simple.

typedef struct {
  u64 loc;
  u32 name;
} LamBind;

// DupBind records a DUP node keyed by its expr location.
typedef struct {
  u64 loc;
  u32 name;
  u32 lab;
} DupBind;

// PrintState keeps naming tables and ALO printing mode.
// - quoted/subst/subst_len: current printing mode, bind list head, and length.
// Fixed-size tables: keep naming simple and bounded.
#define PRINT_NAME_MAX 65536
static LamBind PRINT_LAMS[PRINT_NAME_MAX];
static DupBind PRINT_DUPS[PRINT_NAME_MAX];

typedef struct {
  u32 lam_len;
  u32 dup_len;
  u32 dup_print;
  u32 next_lam;
  u32 next_dup;
  u8  quoted;
  u64 subst;
  u32 subst_len;
} PrintState;

// Core recursive printer; always called through print_term_at.
fn void print_term_go(FILE *f, Term term, u32 depth, PrintState *st);
// Guards against printing a term with the SUB bit set.
fn void print_term_at(FILE *f, Term term, u32 depth, PrintState *st) {
  assert(!term_sub_get(term));
  print_term_go(f, term, depth, st);
}

// Temporarily switches print mode (quoted + subst) for nested ALO rendering.
fn void print_term_mode(FILE *f, Term term, u32 depth, u8 quoted, u64 subst, u32 subst_len, PrintState *st) {
  u8  old_quoted = st->quoted;
  u64 old_subst  = st->subst;
  u32 old_len    = st->subst_len;
  st->quoted = quoted;
  st->subst  = subst;
  st->subst_len = quoted ? subst_len : 0;
  print_term_at(f, term, depth, st);
  st->quoted = old_quoted;
  st->subst  = old_subst;
  st->subst_len = old_len;
}

// Base-26 alpha printer: 1->a, 26->z, 27->aa. 0 prints '_' for unscoped vars.
fn void print_alpha_name(FILE *f, u32 n, char base) {
  if (n == 0) {
    fputc('_', f);
    return;
  }
  char buf[32];
  u32  len = 0;
  while (n > 0) {
    n--;
    buf[len++] = (char)(base + (n % 26));
    n /= 26;
  }
  for (u32 i = 0; i < len; i++) {
    fputc(buf[len - 1 - i], f);
  }
}

// Emits a lambda name (lowercase alpha).
fn void print_lam_name(FILE *f, u32 name) {
  print_alpha_name(f, name, 'a');
}

// Emits a dup name (uppercase alpha).
fn void print_dup_name(FILE *f, u32 name) {
  print_alpha_name(f, name, 'A');
}

// Initializes the printer state and name counters.
fn void print_state_init(PrintState *st) {
  memset(st, 0, sizeof(*st));
  st->next_lam    = 1;
  st->next_dup    = 1;
}

// No-op for fixed tables; kept for symmetry with print_state_init.
fn void print_state_free(PrintState *st) {
  (void)st;
}

// Returns the global name for a lambda body location, allocating if needed.
fn u32 print_state_lam(PrintState *st, u64 loc) {
  for (u32 i = 0; i < st->lam_len; i++) {
    if (PRINT_LAMS[i].loc == loc) {
      return PRINT_LAMS[i].name;
    }
  }
  if (st->lam_len >= PRINT_NAME_MAX) {
    fprintf(stderr, "print_state: too many lambdas\n");
    exit(1);
  }
  u32 name = st->next_lam++;
  PRINT_LAMS[st->lam_len] = (LamBind){.loc = loc, .name = name};
  st->lam_len++;
  return name;
}

// Returns the global name for a DUP node keyed by its expr location.
fn u32 print_state_dup(PrintState *st, u64 loc, u32 lab) {
  for (u32 i = 0; i < st->dup_len; i++) {
    if (PRINT_DUPS[i].loc == loc) {
      return PRINT_DUPS[i].name;
    }
  }
  if (st->dup_len >= PRINT_NAME_MAX) {
    fprintf(stderr, "print_state: too many dups\n");
    exit(1);
  }
  u32 name = st->next_dup++;
  PRINT_DUPS[st->dup_len] = (DupBind){.loc = loc, .name = name, .lab = lab};
  st->dup_len++;
  return name;
}

// Looks up an ALO bind list entry by index (0 = innermost), returning a dynamic loc.
fn u64 alo_subst_get(u64 ls_loc, u32 idx) {
  u64 ls = ls_loc;
  for (u32 i = 0; i < idx && ls != 0; i++) {
    ls = term_val(HEAP[ls + 1]);
  }
  return ls;
}

// Prints an interned definition/reference name, with fallback for unknown ids.
fn void print_def_name(FILE *f, u32 nam) {
  char *name = table_get(nam);
  if (name != NULL) {
    fputs(name, f);
  } else {
    print_name(f, nam);
  }
}

// Prints an interned name (used by ctors/labels/stuck names), with fallback for unknown ids.
fn void print_sym_name(FILE *f, u32 nam) {
  char *name = table_get(nam);
  if (name != NULL) {
    fputs(name, f);
  } else {
    print_name(f, nam);
  }
}

// Prints match constructor labels with special sugar for nat/list forms.
fn void print_mat_name(FILE *f, u32 nam) {
  if (nam == SYM_ZER) {
    fputs("0n", f);
  } else if (nam == SYM_SUC) {
    fputs("1n+", f);
  } else if (nam == SYM_NIL) {
    fputs("[]", f);
  } else if (nam == SYM_CON) {
    fputs("<>", f);
  } else {
    fputc('#', f);
    print_sym_name(f, nam);
  }
}

// Prints APP/DRY spines as f(x,y,...) with a parenthesis around lambdas.
fn void print_app(FILE *f, Term term, u32 depth, PrintState *st) {
  Term spine[256];
  u32  len  = 0;
  Term curr = term;
  while ((term_tag(curr) == APP || term_tag(curr) == DRY) && len < 256) {
    u64 loc = term_val(curr);
    spine[len++] = HEAP[loc + 1];
    curr = HEAP[loc];
  }
  if (term_tag(curr) == LAM) {
    fputc('(', f);
    print_term_at(f, curr, depth, st);
    fputc(')', f);
  } else {
    print_term_at(f, curr, depth, st);
  }
  fputc('(', f);
  for (u32 i = 0; i < len; i++) {
    if (i > 0) {
      fputc(',', f);
    }
    print_term_at(f, spine[len - 1 - i], depth, st);
  }
  fputc(')', f);
}

// Prints constructors, with sugar for nat, char, string, and list forms.
fn void print_ctr(FILE *f, Term t, u32 d, PrintState *st) {
  u32 nam = term_ext(t), ari = term_tag(t) - C00;
  u64 loc = term_val(t);
  // Nat: count SUCs, print as Nn or Nn+x
  if (nam == SYM_ZER || nam == SYM_SUC) {
    u32 n = 0;
    while (term_tag(t) == C01 && term_ext(t) == SYM_SUC) {
      n++;
      t = HEAP[term_val(t)];
    }
    fprintf(f, "%un", n);
    if (!(term_tag(t) == C00 && term_ext(t) == SYM_ZER)) {
      fputc('+', f);
      print_term_at(f, t, d, st);
    }
    return;
  }
  // Char: 'x' or '\n'
  if (nam == SYM_CHR && ari == 1 && term_tag(HEAP[loc]) == NUM) {
    u32 c = term_val(HEAP[loc]);
    if (print_utf8_can_escape(c, PRINT_ESC_CHAR)) {
      fputc('\'', f);
      print_utf8_escape(f, c, PRINT_ESC_CHAR);
      fputc('\'', f);
      return;
    }
  }
  // List/String
  if (nam == SYM_NIL || nam == SYM_CON) {
    // Check if string (non-empty, all escapable chars)
    int is_str = (nam == SYM_CON);
    for (Term x = t; term_tag(x) == C02 && term_ext(x) == SYM_CON; x = HEAP[term_val(x) + 1]) {
      Term h = HEAP[term_val(x)];
      if (!(term_tag(h) == C01 && term_ext(h) == SYM_CHR)) {
        is_str = 0;
        break;
      }
      if (term_tag(HEAP[term_val(h)]) != NUM) {
        is_str = 0;
        break;
      }
      u32 c = term_val(HEAP[term_val(h)]);
      if (!print_utf8_can_escape(c, PRINT_ESC_STR)) {
        is_str = 0;
        break;
      }
    }
    Term end = t;
    while (term_tag(end) == C02 && term_ext(end) == SYM_CON) {
      end = HEAP[term_val(end) + 1];
    }
    if (is_str && term_tag(end) == C00 && term_ext(end) == SYM_NIL) {
      fputc('"', f);
      for (Term x = t; term_tag(x) == C02; x = HEAP[term_val(x) + 1]) {
        u32 c = term_val(HEAP[term_val(HEAP[term_val(x)])]);
        print_utf8_escape(f, c, PRINT_ESC_STR);
      }
      fputc('"', f);
      return;
    }
    // Proper list: [a,b,c]
    if (term_tag(end) == C00 && term_ext(end) == SYM_NIL) {
      fputc('[', f);
      for (Term x = t; term_tag(x) == C02; x = HEAP[term_val(x) + 1]) {
        if (x != t) {
          fputc(',', f);
        }
        print_term_at(f, HEAP[term_val(x)], d, st);
      }
      fputc(']', f);
      return;
    }
    // Improper list: h<>t
    if (nam == SYM_CON) {
      print_term_at(f, HEAP[loc], d, st);
      fputs("<>", f);
      print_term_at(f, HEAP[loc + 1], d, st);
      return;
    }
  }
  // Default CTR
  fputc('#', f);
  print_sym_name(f, nam);
  fputc('{', f);
  for (u32 i = 0; i < ari; i++) {
    if (i) {
      fputc(',', f);
    }
    print_term_at(f, HEAP[loc + i], d, st);
  }
  fputc('}', f);
}

// Recursive printer that handles both dynamic (linked) and quoted (book) terms.
fn void print_term_go(FILE *f, Term term, u32 depth, PrintState *st) {
  u8  quoted = st->quoted;
  u64 subst  = st->subst;
  switch (term_tag(term)) {
    case NAM: {
      // Literal stuck name (^x).
      print_name(f, term_ext(term));
      break;
    }
    case DRY: {
      // Stuck application ^(f x) rendered as f(x).
      print_app(f, term, depth, st);
      break;
    }
    case BJV: {
      // Quoted VAR: val is de Bruijn level; try ALO substitution.
      u64 lvl  = term_val(term);
      u64 bind = 0;
      if (quoted && lvl > 0 && lvl <= st->subst_len) {
        bind = alo_subst_get(subst, st->subst_len - lvl);
      }
      if (bind != 0) {
        Term val = HEAP[bind];
        if (term_sub_get(val)) {
          val = term_sub_set(val, 0);
          print_term_mode(f, val, depth, 0, 0, 0, st);
        } else {
          print_term_mode(f, term_new_var(bind), depth, 0, 0, 0, st);
        }
      } else {
        u32 nam = (lvl > st->subst_len) ? (lvl - st->subst_len) : 0;
        if (nam > depth) {
          nam = 0;
        }
        print_alpha_name(f, nam, 'a');
      }
      break;
    }
    case NUM: {
      fprintf(f, "%u", (u32)term_val(term));
      break;
    }
    case REF: {
      fputc('@', f);
      print_def_name(f, term_ext(term));
      break;
    }
    case ERA: {
      fputs("&{}", f);
      break;
    }
    case ANY: {
      fputc('*', f);
      break;
    }
    case BJ0:
    case BJ1: {
      // Quoted BJ_: val is de Bruijn level; try ALO substitution.
      u64 lvl  = term_val(term);
      u64 bind = 0;
      if (quoted && lvl > 0 && lvl <= st->subst_len) {
        bind = alo_subst_get(subst, st->subst_len - lvl);
      }
      if (bind != 0) {
        Term val = HEAP[bind];
        if (term_sub_get(val)) {
          val = term_sub_set(val, 0);
          print_term_mode(f, val, depth, 0, 0, 0, st);
        } else {
          u8  tag = term_tag(term) == BJ0 ? DP0 : DP1;
          u32 lab = term_ext(term);
          print_term_mode(f, term_new(0, tag, lab, bind), depth, 0, 0, 0, st);
        }
      } else {
        u32 nam = (lvl > st->subst_len) ? (lvl - st->subst_len) : 0;
        if (nam > depth) {
          nam = 0;
        }
        if (nam == 0) {
          fputc('_', f);
        } else {
          print_alpha_name(f, nam, 'A');
        }
        fputs(term_tag(term) == BJ0 ? "₀" : "₁", f);
      }
      break;
    }
    case VAR: {
      // Runtime VAR: val is binding lam body location.
      u64 loc = term_val(term);
      if (loc != 0 && term_sub_get(HEAP[loc])) {
        print_term_mode(f, term_sub_set(HEAP[loc], 0), depth, 0, 0, 0, st);
      } else {
        u32 nam = print_state_lam(st, loc);
        print_lam_name(f, nam);
      }
      break;
    }
    case DP0:
    case DP1: {
      // Runtime DP_: val is a DUP node expr location.
      u64 loc = term_val(term);
      if (loc != 0 && term_sub_get(HEAP[loc])) {
        print_term_mode(f, term_sub_set(HEAP[loc], 0), depth, 0, 0, 0, st);
      } else {
        u32 nam = print_state_dup(st, loc, term_ext(term));
        print_dup_name(f, nam);
        fputs(term_tag(term) == DP0 ? "₀" : "₁", f);
      }
      break;
    }
    case LAM: {
      // Quoted mode uses depth-based names; dynamic mode uses global naming.
      u64 loc = term_val(term);
      fputs("λ", f);
      if (quoted) {
        print_alpha_name(f, depth + 1, 'a');
        fputc('.', f);
        print_term_at(f, HEAP[loc], depth + 1, st);
      } else {
        u32 nam = print_state_lam(st, loc);
        print_lam_name(f, nam);
        fputc('.', f);
        print_term_at(f, HEAP[loc], depth + 1, st);
      }
      break;
    }
    case APP: {
      print_app(f, term, depth, st);
      break;
    }
    case SUP: {
      u64 loc = term_val(term);
      fputc('&', f);
      print_name(f, term_ext(term));
      fputc('{', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputc(',', f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc('}', f);
      break;
    }
    case DUP: {
      // DUP term is a syntactic binder; dynamic mode queues its DUP node and prints the body.
      u64 loc = term_val(term);
      if (quoted) {
        fputc('!', f);
        print_alpha_name(f, depth + 1, 'A');
        fputc('&', f);
        print_name(f, term_ext(term));
        fputc('=', f);
        print_term_at(f, HEAP[loc + 0], depth, st);
        fputc(';', f);
        print_term_at(f, HEAP[loc + 1], depth + 1, st);
      } else {
        print_state_dup(st, loc, term_ext(term));
        print_term_at(f, HEAP[loc + 1], depth, st);
      }
      break;
    }
    case MAT:
    case SWI: {
      fputs("λ{", f);
      Term cur = term;
      while (term_tag(cur) == MAT || term_tag(cur) == SWI) {
        u64 loc = term_val(cur);
        if (term_tag(cur) == SWI) {
          fprintf(f, "%u", term_ext(cur));
        } else {
          print_mat_name(f, term_ext(cur));
        }
        fputc(':', f);
        print_term_at(f, HEAP[loc + 0], depth, st);
        Term next = HEAP[loc + 1];
        if (term_tag(next) == MAT || term_tag(next) == SWI) {
          fputc(';', f);
        }
        cur = next;
      }
      // Handle tail: NUM(0) = empty, USE = wrapped default, other = default.
      if (term_tag(cur) == NUM && term_val(cur) == 0) {
        // empty default - just close
      } else if (term_tag(cur) == USE) {
        fputc(';', f);
        print_term_at(f, HEAP[term_val(cur)], depth, st);
      } else {
        fputc(';', f);
        print_term_at(f, cur, depth, st);
      }
      fputc('}', f);
      break;
    }
    case USE: {
      u64 loc = term_val(term);
      fputs("λ{", f);
      print_term_at(f, HEAP[loc], depth, st);
      fputc('}', f);
      break;
    }
    case C00 ... C16: {
      print_ctr(f, term, depth, st);
      break;
    }
    case OP2: {
      u32 opr = term_ext(term);
      u64 loc = term_val(term);
      static const char *op_syms[] = {
        "+", "-", "*", "/", "%", "&&", "||", "^", "<<", ">>",
        "~", "==", "!=", "<", "<=", ">", ">="
      };
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputc(' ', f);
      if (opr < 17) {
        fputs(op_syms[opr], f);
      } else {
        fprintf(f, "?%u", opr);
      }
      fputc(' ', f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case DSU: {
      u64 loc = term_val(term);
      fputs("&(", f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs("){", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(',', f);
      print_term_at(f, HEAP[loc + 2], depth, st);
      fputc('}', f);
      break;
    }
    case DDU: {
      u64 loc = term_val(term);
      fputs("!(", f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(")=", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(';', f);
      print_term_at(f, HEAP[loc + 2], depth, st);
      break;
    }
    case ALO: {
      // ALO prints as @{book_term}, applying ALO substitutions to book vars.
      u32 len     = term_ext(term);
      u64 tm_loc;
      u64 ls_loc;
      if (len == 0) {
        tm_loc = term_val(term);
        ls_loc = 0;
      } else {
        u64 alo_loc = term_val(term);
        u64 pair = HEAP[alo_loc];
        ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
        tm_loc = pair & ALO_TM_MASK;
      }
      fputs("@{", f);
      print_term_mode(f, HEAP[tm_loc], 0, 1, ls_loc, len, st);
      fputc('}', f);
      break;
    }
    case EQL: {
      u64 loc = term_val(term);
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(" === ", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case AND: {
      u64 loc = term_val(term);
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(" .&. ", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case OR: {
      u64 loc = term_val(term);
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(" .|. ", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case UNS: {
      // UNS binds an unscoped lam/var pair; show them with global names.
      u64 loc   = term_val(term);
      Term lamf = HEAP[loc];
      u64 locf  = term_val(lamf);
      Term lamv = HEAP[locf];
      u64 locv  = term_val(lamv);
      u32 namf  = print_state_lam(st, locf);
      u32 namv  = print_state_lam(st, locv);
      Term body = HEAP[locv];
      fputs("! ", f);
      print_lam_name(f, namf);
      fputs(" = λ ", f);
      print_lam_name(f, namv);
      fputs(" ; ", f);
      print_term_at(f, body, depth + 2, st);
      break;
    }
    case INC: {
      u64 loc = term_val(term);
      fputs("↑", f);
      print_term_at(f, HEAP[loc], depth, st);
      break;
    }
  }
}

// Prints all discovered dup definitions after the main term.
fn void print_term_finish(FILE *f, PrintState *st) {
  int need_sep = (st->dup_print == 0);
  while (st->dup_print < st->dup_len) {
    if (need_sep) {
      fputc(';', f);
      need_sep = 0;
    }
    u32 idx = st->dup_print++;
    u64 loc = PRINT_DUPS[idx].loc;
    u32 lab = PRINT_DUPS[idx].lab;
    u32 nam = PRINT_DUPS[idx].name;
    fputc('!', f);
    print_dup_name(f, nam);
    fputc('&', f);
    print_name(f, lab);
    fputc('=', f);
    Term val = HEAP[loc];
    if (term_sub_get(val)) {
      val = term_sub_set(val, 0);
    }
    print_term_at(f, val, 0, st);
    fputc(';', f);
  }
}

// Entry point that sets up state, prints the term, then prints floating dups.
fn void print_term_ex(FILE *f, Term term) {
  PrintState st;
  print_state_init(&st);
  print_term_at(f, term, 0, &st);
  print_term_finish(f, &st);
  print_state_free(&st);
}

// Prints a dynamic term (linked, global naming, deferred dup printing).
fn void print_term(Term term) {
  print_term_ex(stdout, term);
}

// Prints a static/quoted term to a custom stream at a given initial depth.
fn void print_term_quoted_ex(FILE *f, Term term, u32 depth) {
  PrintState st;
  print_state_init(&st);
  st.quoted = 1;
  st.subst  = 0;
  st.subst_len = 0;
  print_term_at(f, term, depth, &st);
  print_term_finish(f, &st);
  print_state_free(&st);
}

// Prints a static/quoted term (BJV/BJ0/BJ1) with depth-based lambda names.
fn void print_term_quoted(Term term) {
  print_term_quoted_ex(stdout, term, 0);
}

// Runtime Types
// =============

// Runtime Shared Types
// ====================
// Defines small structs shared by CLI and runtime helpers.

// Runtime evaluation behavior flags for running one entrypoint.
typedef struct {
  int do_collapse;
  int collapse_limit;
  int stats;
  int silent;
  int step_by_step;
} RuntimeEvalCfg;

// Parse
// =====

fn void parse_error(PState *s, const char *expected, char detected) {
  fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m (%s:%d:%d)\n", s->file, s->line, s->col);
  fprintf(stderr, "- expected: %s\n", expected);
  if (detected == 0) {
    fprintf(stderr, "- detected: EOF\n");
  } else {
    fprintf(stderr, "- detected: '%c'\n", detected);
  }
  exit(1);
}

fn void parse_error_var(PState *s, u32 nam, int is_dup, int skipped) {
  char  nam_fallback[32];
  char *nam_buf = table_get(nam);
  if (nam_buf == NULL) {
    snprintf(nam_fallback, sizeof(nam_fallback), "#%u", nam);
    nam_buf = nam_fallback;
  }
  fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m (%s:%d:%d)\n", s->file, s->line, s->col);
  if (is_dup && skipped) {
    fprintf(stderr, "- dup variable '%s' requires subscript ₀ or ₁\n", nam_buf);
  } else if (!is_dup && skipped) {
    fprintf(stderr, "- non-dup variable '%s' must be used without subscript (₀ or ₁)\n", nam_buf);
  } else {
    fprintf(stderr, "- undefined variable '%s'\n", nam_buf);
  }
  exit(1);
}

fn void parse_error_affine(PState *s, u32 nam, u32 side, u32 uses) {
  char  nam_fallback[32];
  char *nam_buf = table_get(nam);
  if (nam_buf == NULL) {
    snprintf(nam_fallback, sizeof(nam_fallback), "#%u", nam);
    nam_buf = nam_fallback;
  }
  fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m (%s:%d:%d)\n", s->file, s->line, s->col);
  fprintf(stderr, "- variable '%s%s' used %d times\n", nam_buf, side == 0 ? "₀" : side == 1 ? "₁" : "", uses);
  fprintf(stderr, "- hint: declare variable as '&%s' to allow multiple uses\n", nam_buf);
  exit(1);
}

fn int parse_at_end(PState *s) {
  return s->pos >= s->len;
}

fn char parse_peek_at(PState *s, u32 offset) {
  u32 idx = s->pos + offset;
  return (idx >= s->len) ? 0 : s->src[idx];
}

fn char parse_peek(PState *s) {
  return parse_peek_at(s, 0);
}

fn void parse_advance(PState *s) {
  if (parse_at_end(s)) {
    return;
  }
  if (s->src[s->pos] == '\n') {
    s->line++;
    s->col = 1;
  } else {
    s->col++;
  }
  s->pos++;
}

fn int parse_starts_with(PState *s, const char *str) {
  u32 i = 0;
  while (str[i]) {
    if (parse_peek_at(s, i) != str[i]) {
      return 0;
    }
    i++;
  }
  return 1;
}

fn int parse_match(PState *s, const char *str) {
  if (!parse_starts_with(s, str)) {
    return 0;
  }
  while (*str) {
    parse_advance(s);
    str++;
  }
  return 1;
}

fn int parse_sep(PState *s) {
  char c = parse_peek(s);
  if (c != ',' && c != ';') {
    return 0;
  }
  parse_advance(s);
  return 1;
}

fn int parse_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

fn void parse_skip_comment(PState *s) {
  while (!parse_at_end(s) && parse_peek(s) != '\n') {
    parse_advance(s);
  }
}

fn void parse_skip(PState *s) {
  while (!parse_at_end(s)) {
    if (parse_is_space(parse_peek(s))) {
      parse_advance(s);
      continue;
    }
    if (parse_starts_with(s, "//")) {
      parse_skip_comment(s);
      continue;
    }
    break;
  }
}

fn void parse_consume(PState *s, const char *str) {
  parse_skip(s);
  if (!parse_match(s, str)) {
    parse_error(s, str, parse_peek(s));
  }
  parse_skip(s);
}

fn void parse_bind_push_side(u32 name, u32 depth, u32 lab, int side, u32 cloned) {
  PARSE_BINDS[PARSE_BINDS_LEN++] = (PBind){name, depth + 1, lab, 0, cloned, side};
  return;
}

fn void parse_bind_push(u32 name, u32 depth, u32 lab, u32 forked, u32 cloned) {
  PARSE_BINDS[PARSE_BINDS_LEN++] = (PBind){name, depth + 1, lab, forked, cloned, -1};
  return;
}

fn void parse_bind_pop(void) {
  PARSE_BINDS_LEN--;
}

// Lookup binding by name. Skips to outer binds on false shadowing (dup var + lam bind, lam var + dup bind, etc)
// Returns 1 and sets the bind if found, skipped = 1 if skipped a binding with the same name.
fn PBind* parse_bind_lookup(u32 name, int side, int *skipped) {
  *skipped = 0;
  for (int i = PARSE_BINDS_LEN - 1; i >= 0; i--) {
    PBind* bind = &PARSE_BINDS[i];
    if (bind->name == name) {
      // Destructured dup aliases are used without an explicit subscript.
      if (bind->side >= 0) {
        if (side != -1) {
          *skipped = 1;
          continue;
        }
        return bind;
      }
      // Skip dup bindings if no subscript and not in fork mode
      if (side == -1 && bind->lab != 0 && !bind->forked) {
        *skipped = 1;
        continue;
      }
      // Skip non-dup bindings if subscript or fork mode
      if (side != -1 && bind->lab == 0) {
        *skipped = 1;
        continue;
      }
      return bind;
    }
  }
  return NULL;
}

// Count the number of uses of a target variable in a term
// Variables are identified by tag + level (and ext for BJ mode).
fn u32 count_uses(Term t, u32 lvl, u8 tgt, u32 ext) {
  Term *ts = (Term*)malloc(sizeof(Term) * 1024); // not recursive, since this is a desugared term
  int ts_idx = 0;
  ts[ts_idx++] = t;

  u32 uses = 0;
  while (ts_idx > 0) {
    t = ts[--ts_idx];
    u8  tg = term_tag(t);
    u32 vl = term_val(t);
    if (tg == tgt && vl == lvl && (tgt == BJV || term_ext(t) == ext)) {
      uses++;
    }
    u32 ari = term_arity(t);
    for (u32 i = 0; i < ari; i++) {
      u64 loc = vl + i;
      ts[ts_idx++] = HEAP[loc];
    }
  }
  free(ts);
  return uses;
}

// Auto-dup: rewrites a cloned binder with N uses into N-1 nested DUP nodes.
// Example: [x,x,x] becomes !d0&=x; !d1&=d0₁; [d0₀,d1₀,d1₁]
//
// The transformation is purely structural and does not evaluate anything.
// It must preserve binding structure and linearity:
// - Every occurrence of the target ref is replaced by exactly one occurrence
//   of either a BJ0 or BJ1 that belongs to the newly created DUP chain.
// - The chain ensures that the two sides of each DUP are consumed exactly once.
//
// The key correctness invariant is that the chain length matches the number
// of target occurrences in the (already desugared) body. The uses count is
// passed from the parser's PBind tracking.
//
// We traverse all children and sum uses. This keeps every occurrence unique
// across the whole term, which is required because SNF will traverse every
// branch (including matcher chains).
//
// Works for both BJV refs (let/lambda bindings) and BJ refs (dup bindings).
// - Target is identified by tag + level (and ext for BJ mode).
// - Outer refs (level > base depth) are shifted by n to account for new dup terms.

fn void auto_dup_go(u64 loc, u32 lvl, u32 base, u32 *use, u32 n, u32 lab, u8 tgt, u32 ext) {
  Term t = HEAP[loc];
  u8  tg = term_tag(t);
  u32 vl = term_val(t);

  // Replace target ref with BJ0/BJ1 chain
  if (tg == tgt && vl == lvl && (tgt == BJV || term_ext(t) == ext)) {
    u32 i = (*use)++;
    if (i < n) {
      HEAP[loc] = term_new(0, BJ0, lab + i, base + 1 + i);
    } else {
      HEAP[loc] = term_new(0, BJ1, lab + n - 1, base + n);
    }
    return;
  }

  // Shift outer refs
  if ((tg == BJV || tg == BJ0 || tg == BJ1) && vl > base) {
    HEAP[loc] = term_new(0, tg, term_ext(t), vl + n);
    return;
  }

  // Recurse into children
  switch (tg) {
    case LAM: {
      auto_dup_go(vl, lvl, base, use, n, lab, tgt, ext);
      return;
    }
    case DUP: {
      auto_dup_go(vl + 0, lvl, base, use, n, lab, tgt, ext);
      auto_dup_go(vl + 1, lvl, base, use, n, lab, tgt, ext);
      return;
    }
    default: {
      u32 ari = term_arity(t);
      for (u32 i = 0; i < ari; i++) {
        auto_dup_go(vl + i, lvl, base, use, n, lab, tgt, ext);
      }
    }
  }
}

fn Term parse_auto_dup(Term body, u32 lvl, u32 base, u8 tgt, u32 ext, u32 uses) {
  if (uses <= 1) {
    return body;
  }
  u32 n = uses - 1;
  if (PARSE_FRESH_LAB >= PARSE_DYN_LAB || PARSE_FRESH_LAB + n > PARSE_DYN_LAB) {
    fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m\n");
    fprintf(stderr, "- out of auto-dup labels in 24-bit space\n");
    exit(1);
  }
  u32 lab = PARSE_FRESH_LAB;
  PARSE_FRESH_LAB += n;

  // Walk body's children
  u8  tg  = term_tag(body);
  u32 vl  = term_val(body);
  u32 use = 0;

  switch (tg) {
    case LAM: {
      auto_dup_go(vl, lvl, base, &use, n, lab, tgt, ext);
      break;
    }
    case DUP: {
      auto_dup_go(vl + 0, lvl, base, &use, n, lab, tgt, ext);
      auto_dup_go(vl + 1, lvl, base, &use, n, lab, tgt, ext);
      break;
    }
    default: {
      u32 ari = term_arity(body);
      for (u32 i = 0; i < ari; i++) {
        auto_dup_go(vl + i, lvl, base, &use, n, lab, tgt, ext);
      }
    }
  }

  // Build dup chain: !d0&=x; !d1&=d0₁; ... body
  Term result = body;
  for (int i = n - 1; i >= 0; i--) {
    Term v   = (i == 0) ? term_new(0, tgt, ext, lvl) : term_new(0, BJ1, lab + i - 1, base + i);
    u64  loc = heap_alloc(2);
    HEAP[loc + 0] = v;
    HEAP[loc + 1] = result;
    result = term_new(0, DUP, lab + i, loc);
  }

  return result;
}

fn u32 parse_name(PState *s) {
  parse_skip(s);
  char c = parse_peek(s);
  if (!nick_is_init(c)) {
    parse_error(s, "name", c);
  }
  u32 start = s->pos;
  while (nick_is_char(parse_peek(s))) {
    parse_advance(s);
  }
  u32 len = s->pos - start;
  u32 id  = table_find(s->src + start, len);
  parse_skip(s);
  return id;
}

// Parses a name as a 24-bit base64 number.
// Used for NAM (stuck names) and static labels that must stay numeric.
fn u32 parse_name_num(PState *s) {
  parse_skip(s);
  char c = parse_peek(s);
  if (!nick_is_init(c)) {
    parse_error(s, "name", c);
  }
  u64 k = 0;
  while (nick_is_char(parse_peek(s))) {
    c = parse_peek(s);
    k = (k << 6) | nick_letter_to_b64(c);
    if (k > EXT_MASK) {
      fprintf(stderr, "\033[1;31mPARSE_ERROR\033[0m (%s:%d:%d)\n", s->file, s->line, s->col);
      fprintf(stderr, "- base64 name '");
      print_name(stderr, k);
      fprintf(stderr, "' exceeds 24-bit limit (max 0x%06X)\n", EXT_MASK);
      exit(1);
    }
    parse_advance(s);
  }
  parse_skip(s);
  return (u32)k;
}

// Like parse_name, but returns a unique ID from the global table.
// Used for function definitions and references.
fn u32 parse_name_ref(PState *s) {
  parse_skip(s);
  char c = parse_peek(s);
  if (!nick_is_init(c)) {
    parse_error(s, "name", c);
  }
  u32 start = s->pos;
  while (nick_is_char(parse_peek(s))) {
    parse_advance(s);
  }
  u32 len = s->pos - start;
  u32 id  = table_find(s->src + start, len);
  parse_skip(s);
  return id;
}

// Decode UTF-8 codepoint at current position, advance past it
fn u32 parse_utf8(PState *s) {
  u8 b0 = (u8)s->src[s->pos];
  parse_advance(s);
  if (b0 < 0x80) return b0;
  if ((b0 & 0xE0) == 0xC0 && s->pos < s->len) {
    u8 b1 = (u8)s->src[s->pos];
    parse_advance(s);
    return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  if ((b0 & 0xF0) == 0xE0 && s->pos + 1 < s->len) {
    u8 b1 = (u8)s->src[s->pos];
    parse_advance(s);
    u8 b2 = (u8)s->src[s->pos];
    parse_advance(s);
    return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
  }
  if ((b0 & 0xF8) == 0xF0 && s->pos + 2 < s->len) {
    u8 b1 = (u8)s->src[s->pos];
    parse_advance(s);
    u8 b2 = (u8)s->src[s->pos];
    parse_advance(s);
    u8 b3 = (u8)s->src[s->pos];
    parse_advance(s);
    return ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12)
         | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
  }
  return b0;
}

fn Term parse_term(PState *s, u32 depth);
fn u32  parse_char_lit(PState *s);

fn Term parse_term_lam(PState *s, u32 depth) {
  parse_skip(s);
  // Era λ{}, Use λ{f} or Lambda-match λ{A:f; B:g; ...; i}
  if (parse_peek(s) == '{') {
    parse_consume(s, "{");
    parse_skip(s);
    if (parse_peek(s) == '}') {
      parse_consume(s, "}");
      return term_new_era();
    }
    Term  term = term_new_num(0);
    Term *tip  = &term;
    while (1) {
      parse_skip(s);
      u8  tag = 0;
      u32 ext = 0;
      PState save = *s;
      // Select the match case:
      if (parse_peek(s) == '\'') {
        u32 code = parse_char_lit(s);
        parse_skip(s);
        tag = SWI;
        ext = code;
      }
      else if (isdigit(parse_peek(s))) {
        while (isdigit(parse_peek(s))) {
          ext = ext * 10 + (parse_peek(s) - '0');
          parse_advance(s);
        }
        parse_skip(s);
        if (parse_peek(s) == ':') {
          tag = SWI;
        } else if (parse_peek(s) == 'n') {
          if (ext == 0 && parse_peek_at(s, 1) != '+') {
            parse_advance(s);
            tag = MAT;
            ext = SYM_ZER;
          } else if (ext == 1 && parse_peek_at(s, 1) == '+') {
            parse_advance(s);
            parse_advance(s);
            tag = MAT;
            ext = SYM_SUC;
          }
        }
      }
      else if (parse_peek(s) == '#') {
        parse_advance(s);
        tag = MAT;
        ext = parse_name(s);
      }
      else if (parse_peek(s) == '[' && parse_peek_at(s, 1) == ']') {
        parse_advance(s);
        parse_advance(s);
        tag = MAT;
        ext = SYM_NIL;
      }
      else if (parse_peek(s) == '<' && parse_peek_at(s, 1) == '>') {
        parse_advance(s);
        parse_advance(s);
        tag = MAT;
        ext = SYM_CON;
      }
      if (tag) {
        parse_skip(s);
        parse_consume(s, ":");
        Term val = parse_term(s, depth);
        parse_skip(s);
        parse_sep(s);
        u64 loc = heap_alloc(2);
        HEAP[loc + 0] = val;
        HEAP[loc + 1] = term_new_num(0);
        *tip = term_new(0, tag, ext, loc);
        tip  = &HEAP[loc + 1];
        continue;
      }
      // Not a match case, backtrack
      *s = save;

      // Use: λ{f}
      if (term == term_new_num(0)) {
        Term f = parse_term(s, depth);
        parse_skip(s);
        parse_sep(s);
        parse_skip(s);
        parse_consume(s, "}");
        return term_new_use(f);
      }
      // Match ending with no default
      if (parse_peek(s) == '}') {
        parse_consume(s, "}");
        return term;
      }
      // Match ending with default
      // Optional "_:" before default case
      if (parse_peek(s) == '_') {
        parse_advance(s);
        parse_skip(s);
        parse_consume(s, ":");
      }
      *tip = parse_term(s, depth);
      parse_skip(s);
      parse_sep(s);
      parse_skip(s);
      parse_consume(s, "}");
      return term;
    }
  }

  // Unscoped lambda: λ$x. body
  if (parse_peek(s) == '$') {
    parse_advance(s);  // consume '$'
    u32 nam = parse_name(s);
    parse_skip(s);

    // Bind unscoped var at depth+1 (reserve depth+1 for hidden f binder)
    parse_bind_push(nam, depth + 1, 0, 0, 0);
    Term body;
    if (parse_sep(s)) {
      body = parse_term_lam(s, depth + 2);
    } else {
      parse_consume(s, ".");
      body = parse_term(s, depth + 2);
    }
    parse_bind_pop();

    // Affine check for unscoped var
    u32 uses = count_uses(body, depth + 2, BJV, 0);
    if (uses > 1) {
      parse_error_affine(s, nam, -1, uses);
    }

    // Build: !${f,x}; f(body) with fresh f
    Term f_ref = term_new(0, BJV, 0, depth + 1);
    Term app   = term_new_app(f_ref, body);
    u64 loc_x  = heap_alloc(1);
    HEAP[loc_x] = app;
    u32 lam_x_ext = depth + 2;
    if (uses == 0) {
      lam_x_ext |= LAM_ERA_MASK;
    }
    Term lam_x = term_new(0, LAM, lam_x_ext, loc_x);
    u64 loc_f = heap_alloc(1);
    HEAP[loc_f] = lam_x;
    Term lam_f = term_new(0, LAM, depth + 1, loc_f);
    return term_new_uns(lam_f);
  }

  // Parse argument: [&]name[&[label|(label)]]
  u32 cloned = parse_match(s, "&");
  u32 nam    = parse_name(s);
  parse_skip(s);

  // Inline dup: λx&L or λx&(L) or λx&
  if (parse_peek(s) == '&') {
    parse_advance(s);
    parse_skip(s);
    int  dyn      = parse_peek(s) == '(';
    Term lab_term = 0;
    u32  lab      = 0;
    if (dyn) {
      parse_consume(s, "(");
      lab_term = parse_term(s, depth + 1);  // +1 because we're inside the outer lambda
      parse_consume(s, ")");
    } else {
      char c = parse_peek(s);
      if (c == ',' || c == ';' || c == '.') {
        if (PARSE_FRESH_LAB >= PARSE_DYN_LAB) {
          parse_error(s, "available auto-dup label (< 0xFFFFFF)", parse_peek(s));
        }
        lab = PARSE_FRESH_LAB++;
      } else {
        lab = parse_name_num(s);
      }
    }
    parse_skip(s);
    u32 d = dyn ? 3 : 2;
    parse_bind_push(nam, depth + 1, dyn ? PARSE_DYN_LAB : lab, 0, cloned);
    Term body;
    if (parse_sep(s)) {
      body = parse_term_lam(s, depth + d);
    } else {
      parse_consume(s, ".");
      body = parse_term(s, depth + d);
    }
    parse_bind_pop();
    if (dyn) {
      //λx&(L) -> λx.!x&(L) = x;
      u32 uses0 = count_uses(body, depth + 2, BJV, 0);
      u32 uses1 = count_uses(body, depth + 3, BJV, 0);
      if (cloned) {
        body = parse_auto_dup(body, depth + 2, depth + 3, BJV, 0, uses0);
        body = parse_auto_dup(body, depth + 3, depth + 3, BJV, 0, uses1);
      }
      if (!cloned && uses0 > 1) {
        parse_error_affine(s, nam, 0, uses0);
      }
      if (!cloned && uses1 > 1) {
        parse_error_affine(s, nam, 1, uses1);
      }
      u64 loc1 = heap_alloc(1);
      HEAP[loc1] = body;
      u64 loc0 = heap_alloc(1);
      HEAP[loc0] = term_new(0, LAM, depth + 3, loc1);
      Term ddu = term_new_ddu(lab_term, term_new(0, BJV, 0, depth + 1), term_new(0, LAM, depth + 2, loc0));
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = ddu;
      u32 lam_ext = depth + 1;
      if (uses0 == 0 && uses1 == 0) {
        lam_ext |= LAM_ERA_MASK;
      }
      return term_new(0, LAM, lam_ext, lam_loc);
    } else {
      //λx&L
      u32 uses0 = count_uses(body, depth + 2, BJ0, lab);
      u32 uses1 = count_uses(body, depth + 2, BJ1, lab);
      if (cloned) {
        body = parse_auto_dup(body, depth + 2, depth + 2, BJ1, lab, uses1);
        body = parse_auto_dup(body, depth + 2, depth + 2, BJ0, lab, uses0);
      }
      if (!cloned && uses0 > 1) {
        parse_error_affine(s, nam, 0, uses0);
      }
      if (!cloned && uses1 > 1) {
        parse_error_affine(s, nam, 1, uses1);
      }
      u64 dup_term_loc = heap_alloc(2);
      HEAP[dup_term_loc + 0] = term_new(0, BJV, 0, depth + 1);
      HEAP[dup_term_loc + 1] = body;
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = term_new(0, DUP, lab, dup_term_loc);
      u32 lam_ext = depth + 1;
      if (uses0 == 0 && uses1 == 0) {
        lam_ext |= LAM_ERA_MASK;
      }
      return term_new(0, LAM, lam_ext, lam_loc);
    }
  }

  // Simple single arg (with separator recursion for cloned/complex args)
  parse_bind_push(nam, depth, 0, 0, cloned);
  Term body;
  if (parse_sep(s)) {
    body = parse_term_lam(s, depth + 1);
  } else {
    parse_consume(s, ".");
    body = parse_term(s, depth + 1);
  }
  parse_bind_pop();
  u32 uses = count_uses(body, depth + 1, BJV, 0);
  if (cloned) {
    body = parse_auto_dup(body, depth + 1, depth + 1, BJV, 0, uses);
  }
  if (!cloned && uses > 1) {
    parse_error_affine(s, nam, -1, uses);
  }
  u32 lam_ext = depth + 1;
  if (uses == 0) {
    lam_ext |= LAM_ERA_MASK;
  }
  u64 loc = heap_alloc(1);
  HEAP[loc] = body;
  return term_new(0, LAM, lam_ext, loc);
}

fn Term parse_term(PState *s, u32 depth);

fn u32 parse_term_dup_pat_name(PState *s, u32 *cloned) {
  parse_skip(s);
  *cloned = 0;
  if (parse_peek(s) == '&') {
    parse_advance(s);
    parse_skip(s);
    *cloned = 1;
  }
  return parse_name(s);
}

fn int parse_term_dup_pat(PState *s, u32 depth, Term *out) {
  PState save = *s;
  parse_skip(s);
  if (!parse_match(s, "&")) {
    return 0;
  }
  parse_skip(s);

  int  dyn      = 0;
  int  fresh    = 0;
  Term lab_term = 0;
  u32  lab      = 0;
  if (parse_peek(s) == '(') {
    dyn = 1;
    parse_consume(s, "(");
    lab_term = parse_term(s, depth);
    parse_consume(s, ")");
    parse_skip(s);
    if (parse_peek(s) != '{') {
      parse_error(s, "{", parse_peek(s));
    }
  } else if (parse_peek(s) == '{') {
    fresh = 1;
  } else {
    PState lab_save = *s;
    char c = parse_peek(s);
    if (!nick_is_init(c)) {
      *s = save;
      return 0;
    }
    while (nick_is_char(parse_peek(s))) {
      parse_advance(s);
    }
    parse_skip(s);
    if (parse_peek(s) != '{') {
      *s = save;
      return 0;
    }
    *s = lab_save;
    lab = parse_name_num(s);
  }

  parse_consume(s, "{");
  u32 cloned0 = 0;
  u32 cloned1 = 0;
  u32 nam0    = parse_term_dup_pat_name(s, &cloned0);
  parse_skip(s);
  parse_sep(s);
  u32 nam1 = parse_term_dup_pat_name(s, &cloned1);
  parse_skip(s);
  parse_sep(s);
  parse_consume(s, "}");
  if (nam0 == nam1) {
    parse_error(s, "distinct names in dup pattern", parse_peek(s));
  }
  if (fresh) {
    if (PARSE_FRESH_LAB >= PARSE_DYN_LAB) {
      parse_error(s, "available auto-dup label (< 0xFFFFFF)", parse_peek(s));
    }
    lab = PARSE_FRESH_LAB++;
  }

  parse_consume(s, "=");
  Term val = parse_term(s, depth);
  parse_skip(s);
  parse_sep(s);
  parse_skip(s);

  if (dyn) {
    parse_bind_push_side(nam0, depth, PARSE_DYN_LAB, 0, cloned0);
    parse_bind_push_side(nam1, depth, PARSE_DYN_LAB, 1, cloned1);
    Term body = parse_term(s, depth + 2);
    parse_bind_pop();
    parse_bind_pop();
    u32 uses0 = count_uses(body, depth + 1, BJV, 0);
    u32 uses1 = count_uses(body, depth + 2, BJV, 0);
    if (cloned0) {
      body = parse_auto_dup(body, depth + 1, depth + 2, BJV, 0, uses0);
    }
    if (cloned1) {
      body = parse_auto_dup(body, depth + 2, depth + 2, BJV, 0, uses1);
    }
    if (!cloned0 && uses0 > 1) {
      parse_error_affine(s, nam0, -1, uses0);
    }
    if (!cloned1 && uses1 > 1) {
      parse_error_affine(s, nam1, -1, uses1);
    }
    u64 loc1   = heap_alloc(1);
    HEAP[loc1] = body;
    Term lam1  = term_new(0, LAM, depth + 2, loc1);
    u64 loc0   = heap_alloc(1);
    HEAP[loc0] = lam1;
    Term lam0  = term_new(0, LAM, depth + 1, loc0);
    *out = term_new_ddu(lab_term, val, lam0);
    return 1;
  }

  parse_bind_push_side(nam0, depth, lab, 0, cloned0);
  parse_bind_push_side(nam1, depth, lab, 1, cloned1);
  Term body = parse_term(s, depth + 1);
  parse_bind_pop();
  parse_bind_pop();
  u32 uses0 = count_uses(body, depth + 1, BJ0, lab);
  u32 uses1 = count_uses(body, depth + 1, BJ1, lab);
  if (cloned1) {
    body = parse_auto_dup(body, depth + 1, depth + 1, BJ1, lab, uses1);
  }
  if (cloned0) {
    body = parse_auto_dup(body, depth + 1, depth + 1, BJ0, lab, uses0);
  }
  if (!cloned0 && uses0 > 1) {
    parse_error_affine(s, nam0, -1, uses0);
  }
  if (!cloned1 && uses1 > 1) {
    parse_error_affine(s, nam1, -1, uses1);
  }
  *out = term_new_dup(lab, val, body);
  return 1;
}

fn Term parse_term_dup(PState *s, u32 depth) {
  parse_skip(s);
  // Check for !!x = val or !!&x = val (strict let, optionally cloned)
  int strict = parse_match(s, "!");
  parse_skip(s);
  Term pat;
  if (parse_term_dup_pat(s, depth, &pat)) {
    return pat;
  }
  // Check for cloned: '&' BEFORE name
  u32 cloned = 0;
  if (parse_peek(s) == '&') {
    parse_advance(s);  // consume &
    parse_skip(s);
    cloned = 1;
  }
  u32 nam = parse_name(s);
  parse_skip(s);

  // Let sugar: ! x = val; body  →  (λx.body)(val)
  // Or cloned let: ! &x = val; body
  // Or unscoped lambda: ! f = λ x ; body
  if (parse_peek(s) == '=') {
    parse_advance(s);
    parse_skip(s);
    // Check for unscoped lambda: ! f = λ x ; body
    // Lookahead: save state, try λ name ;, restore if not matched
    PState save = *s;
    if (parse_match(s, "λ")) {
      parse_skip(s);
      u32 nam_v = parse_name(s);
      parse_skip(s);
      if (parse_match(s, ";")) {
        // Confirmed unscoped lambda
        parse_skip(s);
        parse_bind_push(nam, depth, 0, 0, 0);
        parse_bind_push(nam_v, depth + 1, 0, 0, 0);
        u64 loc_f = heap_alloc(1);
        u64 loc_v = heap_alloc(1);
        Term body = parse_term(s, depth + 2);
        parse_bind_pop();
        parse_bind_pop();
        u32 uses_f = count_uses(body, depth + 1, BJV, 0);
        u32 uses_v = count_uses(body, depth + 2, BJV, 0);
        if (uses_f > 1) {
          parse_error_affine(s, nam, -1, uses_f);
        }
        if (uses_v > 1) {
          parse_error_affine(s, nam_v, -1, uses_v);
        }
        HEAP[loc_v] = body;
        Term lam_v = term_new(0, LAM, depth + 2, loc_v);
        HEAP[loc_f] = lam_v;
        Term lam_f = term_new(0, LAM, depth + 1, loc_f);
        return term_new_uns(lam_f);
      }
      // Not unscoped lambda, restore position
      *s = save;
    }
    Term val = parse_term(s, depth);
    parse_skip(s);
    parse_sep(s);
    parse_bind_push(nam, depth, 0, 0, cloned);
    u64  loc  = heap_alloc(1);
    Term body = parse_term(s, depth + 1);
    parse_bind_pop();
    u32 uses = count_uses(body, depth + 1, BJV, 0);
    if (cloned) {
      body = parse_auto_dup(body, depth + 1, depth + 1, BJV, 0, uses);
    }
    if (!cloned && uses > 1) {
      parse_error_affine(s, nam, -1, uses);
    }
    HEAP[loc] = body;
    Term lam = term_new(0, LAM, depth + 1, loc);
    if (strict) {
      // !!x = val; body  →  (λ{λx.body(x)})(val)
      lam = term_new_use(lam);
    }
    return term_new_app(lam, val);
  }

  // Regular DUP term: !x&label = val; body  or  !x& = val; body (auto-label)
  // Cloned DUP term: !&X &label = val; body  or  !&X & = val; body (auto-label)
  // Dynamic DUP term: !x&(lab) = val; body  (lab is an expression)
  // Cloned Dynamic DUP term: !&X &(lab) = val; body
  parse_consume(s, "&");
  parse_skip(s);
  // Dynamic label: (expr)
  if (parse_peek(s) == '(') {
    parse_consume(s, "(");
    Term lab_term = parse_term(s, depth);
    parse_consume(s, ")");
    parse_consume(s, "=");
    Term val = parse_term(s, depth);
    parse_skip(s);
    parse_sep(s);
    parse_skip(s);
    parse_bind_push(nam, depth, PARSE_DYN_LAB, 0, cloned);
    Term body = parse_term(s, depth + 2);
    parse_bind_pop();
    u32 uses0 = count_uses(body, depth + 1, BJV, 0);
    u32 uses1 = count_uses(body, depth + 2, BJV, 0);
    if (cloned) {
      body = parse_auto_dup(body, depth + 1, depth + 2, BJV, 0, uses0);
      body = parse_auto_dup(body, depth + 2, depth + 2, BJV, 0, uses1);
    }
    if (!cloned && uses0 > 1) {
      parse_error_affine(s, nam, 0, uses0);
    }
    if (!cloned && uses1 > 1) {
      parse_error_affine(s, nam, 1, uses1);
    }
    u64 loc0   = heap_alloc(1);
    u64 loc1   = heap_alloc(1);
    HEAP[loc1] = body;
    Term lam1  = term_new(0, LAM, depth + 2, loc1);
    HEAP[loc0] = lam1;
    Term lam0  = term_new(0, LAM, depth + 1, loc0);
    return term_new_ddu(lab_term, val, lam0);
  }
  // Static label (or auto if next is =)
  u32 lab;
  if (parse_peek(s) == '=') {
    if (PARSE_FRESH_LAB >= PARSE_DYN_LAB) {
      parse_error(s, "available auto-dup label (< 0xFFFFFF)", parse_peek(s));
    }
    lab = PARSE_FRESH_LAB++;
  } else {
    lab = parse_name_num(s);
  }
  u64 loc = heap_alloc(2);
  parse_consume(s, "=");
  HEAP[loc] = parse_term(s, depth);
  parse_skip(s);
  parse_sep(s);
  parse_skip(s);
  parse_bind_push(nam, depth, lab, 0, cloned);
  Term body   = parse_term(s, depth + 1);
  parse_bind_pop();
  u32 uses0 = count_uses(body, depth + 1, BJ0, lab);
  u32 uses1 = count_uses(body, depth + 1, BJ1, lab);
  if (cloned) {
    body = parse_auto_dup(body, depth + 1, depth + 1, BJ1, lab, uses1);
    body = parse_auto_dup(body, depth + 1, depth + 1, BJ0, lab, uses0);
  }
  if (!cloned && uses0 > 1) {
    parse_error_affine(s, nam, 0, uses0);
  }
  if (!cloned && uses1 > 1) {
    parse_error_affine(s, nam, 1, uses1);
  }
  HEAP[loc + 1] = body;
  return term_new(0, DUP, lab, loc);
}

fn Term parse_term(PState *s, u32 depth);

// Fork: &Lλx,y,z{A;B} or &(L)λx,y,z{A;B}
// Desugars to: λx&L.λy&L.λz&L.&L{A';B'}
// where A' uses x₀,y₀,z₀ and B' uses x₁,y₁,z₁
fn Term parse_term_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth) {
  // Save outer fork state (allow nesting when inside a sup-fork)
  int saved_fork_side = PARSE_FORK_SIDE;
  u32 names[16];
  u32 n = 0;
  names[n++] = parse_name(s);
  parse_skip(s);
  while (parse_peek(s) != '{') {
    parse_sep(s);
    parse_skip(s);
    if (parse_peek(s) == '{') break;
    if (n >= 16) {
      parse_error(s, "at most 16 fork binders", parse_peek(s));
    }
    names[n++] = parse_name(s);
    parse_skip(s);
  }
  parse_consume(s, "{");
  u32 d = dyn ? 3 : 2;
  for (u32 i = 0; i < n; i++) {
    parse_bind_push(names[i], depth + i * d + 1, dyn ? PARSE_DYN_LAB : lab, 1, 0);
  }
  u32 body_depth = depth + n * d;
  // Optional &₀: before left branch
  if (parse_match(s, "&₀")) {
    parse_skip(s);
    parse_consume(s, ":");
  }
  PARSE_FORK_SIDE = 0;
  Term left = parse_term(s, body_depth);
  parse_skip(s);
  parse_sep(s);
  parse_skip(s);
  // Optional &₁: before right branch
  if (parse_match(s, "&₁")) {
    parse_skip(s);
    parse_consume(s, ":");
  }
  PARSE_FORK_SIDE = 1;
  Term right = parse_term(s, body_depth);
  PARSE_FORK_SIDE = saved_fork_side;
  parse_skip(s);
  parse_sep(s);
  parse_consume(s, "}");
  for (u32 i = 0; i < n; i++) {
    parse_bind_pop();
  }
  // Check affine for each forked variable
  for (u32 i = 0; i < n; i++) {
    u32 lvl = depth + i * d + 1;
    u32 uses0, uses1;
    if (dyn) {
      uses0 = count_uses(left, lvl, BJV, 0);
      uses1 = count_uses(right, lvl + 1, BJV, 0);
    } else {
      uses0 = count_uses(left, lvl, BJ0, lab);
      uses1 = count_uses(right, lvl, BJ1, lab);
    }
    if (uses0 > 1) {
      parse_error_affine(s, names[i], 0, uses0);
    }
    if (uses1 > 1) {
      parse_error_affine(s, names[i], 1, uses1);
    }
  }
  // Build body: DSU or SUP
  Term body;
  if (dyn) {
    body = term_new_dsu(lab_term, left, right);
  } else {
    body = term_new_sup(lab, left, right);
  }
  // Wrap with λx&L or λx&(L) for each arg (reverse order)
  for (int i = n - 1; i >= 0; i--) {
    u32 dd = depth + i * d;
    if (dyn) {
      u64 loc1 = heap_alloc(1);
      HEAP[loc1] = body;
      u64 loc0 = heap_alloc(1);
      HEAP[loc0] = term_new(0, LAM, dd + 3, loc1);
      Term ddu = term_new_ddu(lab_term, term_new(0, BJV, 0, dd + 1), term_new(0, LAM, dd + 2, loc0));
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = ddu;
      body = term_new(0, LAM, dd + 1, lam_loc);
    } else {
      Term dup = term_new_dup(lab, term_new(0, BJV, 0, dd + 1), body);
      u64 lam_loc = heap_alloc(1);
      HEAP[lam_loc] = dup;
      body = term_new(0, LAM, dd + 1, lam_loc);
    }
  }
  return body;
}

fn Term parse_term(PState *s, u32 depth);

// Core sup-fork logic shared by explicit [x,y,&z] and auto-fork !.
// Assumes variable arrays are already filled and PARSE_FORK_SIDE is saved by caller.
fn Term parse_term_sup_fork_core(
  PState *s, int dyn, Term lab_term, u32 lab, u32 depth,
  u32 *names, u32 *old_depths, u32 *old_tags, u32 *old_labs, u32 *cloned, u32 n,
  int saved_fork_side
) {
  // Reset PARSE_FORK_SIDE for parsing the branches of THIS fork
  PARSE_FORK_SIDE = -1;

  // Push forked bindings for each variable.
  u32 d = dyn ? 2 : 1;
  for (u32 i = 0; i < n; i++) {
    parse_bind_push(names[i], depth + i * d, dyn ? PARSE_DYN_LAB : lab, 1, cloned[i]);
  }

  u32 body_depth = depth + n * d;

  // Optional &₀: before left branch
  if (parse_match(s, "&₀")) {
    parse_skip(s);
    parse_consume(s, ":");
  }
  PARSE_FORK_SIDE = 0;
  Term left = parse_term(s, body_depth);
  parse_skip(s);
  parse_sep(s);
  parse_skip(s);

  // Optional &₁: before right branch
  if (parse_match(s, "&₁")) {
    parse_skip(s);
    parse_consume(s, ":");
  }
  PARSE_FORK_SIDE = 1;
  Term right = parse_term(s, body_depth);
  PARSE_FORK_SIDE = saved_fork_side;
  parse_skip(s);
  parse_sep(s);
  parse_consume(s, "}");

  // Pop forked bindings
  for (u32 i = 0; i < n; i++) {
    parse_bind_pop();
  }

  // Affine checks + auto-dup for each forked variable
  for (u32 i = 0; i < n; i++) {
    u32 lvl = depth + i * d + 1;
    u32 uses0, uses1;
    if (dyn) {
      uses0 = count_uses(left,  lvl,     BJV, 0);
      uses1 = count_uses(right, lvl + 1, BJV, 0);
    } else {
      uses0 = count_uses(left,  lvl, BJ0, lab);
      uses1 = count_uses(right, lvl, BJ1, lab);
    }
    if (cloned[i]) {
      if (dyn) {
        left  = parse_auto_dup(left,  lvl,     body_depth, BJV, 0, uses0);
        right = parse_auto_dup(right, lvl + 1, body_depth, BJV, 0, uses1);
      } else {
        left  = parse_auto_dup(left,  lvl, body_depth, BJ0, lab, uses0);
        right = parse_auto_dup(right, lvl, body_depth, BJ1, lab, uses1);
      }
    } else {
      if (uses0 > 1) {
        parse_error_affine(s, names[i], 0, uses0);
      }
      if (uses1 > 1) {
        parse_error_affine(s, names[i], 1, uses1);
      }
    }
  }

  // Build body: SUP
  Term body;
  if (dyn) {
    body = term_new_dsu(lab_term, left, right);
  } else {
    body = term_new_sup(lab, left, right);
  }

  // Wrap with DUP chain (reverse order)
  for (int i = n - 1; i >= 0; i--) {
    Term var_ref = term_new(0, old_tags[i], old_labs[i], old_depths[i]);
    if (dyn) {
      u64 loc1 = heap_alloc(1);
      HEAP[loc1] = body;
      Term lam1 = term_new(0, LAM, depth + i * d + 2, loc1);
      u64 loc0 = heap_alloc(1);
      HEAP[loc0] = lam1;
      Term lam0 = term_new(0, LAM, depth + i * d + 1, loc0);
      body = term_new_ddu(lab_term, var_ref, lam0);
    } else {
      body = term_new_dup(lab, var_ref, body);
    }
  }

  return body;
}

// Sup-fork: &L[x,y,&z]{A; B}  or  &(L)[x,y,&z]{A; B}
// Desugars to: !x &L = x; !y &L = y; !&z &L = z; &L{A'; B'}
// where A' uses x₀,y₀,z₀ and B' uses x₁,y₁,z₁
// Variables prefixed with & are cloned (can be used multiple times per branch).
fn Term parse_term_sup_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth) {
  int saved_fork_side = PARSE_FORK_SIDE;

  // Parse variable names: [x, y, &z]
  u32 names[16];
  u32 old_depths[16];
  u32 old_tags[16];
  u32 old_labs[16];
  u32 cloned[16];
  u32 n = 0;

  parse_skip(s);
  while (parse_peek(s) != ']') {
    parse_skip(s);
    if (n >= 16) {
      parse_error(s, "at most 16 sup-fork binders", parse_peek(s));
    }
    cloned[n] = 0;
    if (parse_peek(s) == '&') {
      parse_advance(s);
      parse_skip(s);
      cloned[n] = 1;
    }
    names[n] = parse_name(s);
    parse_skip(s);

    int skipped;
    PBind* bind = parse_bind_lookup(names[n], -1, &skipped);
    if (bind == NULL) {
      parse_error_var(s, names[n], 1, skipped);
    }
    old_depths[n] = bind->lvl;
    if (bind->side >= 0) {
      if (bind->lab == PARSE_DYN_LAB) {
        old_depths[n] = bind->lvl + bind->side;
        old_tags[n]   = BJV;
        old_labs[n]   = 0;
      } else {
        old_tags[n] = (bind->side == 0) ? BJ0 : BJ1;
        old_labs[n] = bind->lab;
      }
    } else if (bind->forked && saved_fork_side >= 0) {
      old_tags[n] = (saved_fork_side == 0) ? BJ0 : BJ1;
      old_labs[n] = bind->lab;
    } else {
      old_tags[n] = BJV;
      old_labs[n] = 0;
    }
    n++;

    parse_skip(s);
    parse_sep(s);
  }
  parse_consume(s, "]");
  parse_skip(s);
  parse_consume(s, "{");
  parse_skip(s);

  return parse_term_sup_fork_core(s, dyn, lab_term, lab, depth,
    names, old_depths, old_tags, old_labs, cloned, n, saved_fork_side);
}

// Auto-fork: &L!{A; B}  or  &(L)!{A; B}
// Like sup-fork but captures ALL in-scope variables (all cloned).
fn Term parse_term_auto_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth) {
  int saved_fork_side = PARSE_FORK_SIDE;

  u32 names[16];
  u32 old_depths[16];
  u32 old_tags[16];
  u32 old_labs[16];
  u32 cloned[16];
  u32 n = 0;

  // Collect all in-scope variables from the binding stack.
  for (u32 bi = 0; bi < PARSE_BINDS_LEN; bi++) {
    PBind *bind = &PARSE_BINDS[bi];
    if (bind->lab != 0 && !bind->forked && bind->side < 0) {
      continue; // skip raw dup bindings
    }
    // Deduplicate: keep innermost (last) binding per name
    int found = -1;
    for (u32 j = 0; j < n; j++) {
      if (names[j] == bind->name) { found = j; break; }
    }
    u32 slot = (found >= 0) ? found : n;
    if (found < 0) {
      if (n >= 16) parse_error(s, "at most 16 auto-fork captures", parse_peek(s));
      n++;
    }
    names[slot]     = bind->name;
    old_depths[slot] = bind->lvl;
    cloned[slot]    = 1;
    if (bind->side >= 0) {
      if (bind->lab == PARSE_DYN_LAB) {
        old_depths[slot] = bind->lvl + bind->side;
        old_tags[slot]   = BJV;
        old_labs[slot]   = 0;
      } else {
        old_tags[slot] = (bind->side == 0) ? BJ0 : BJ1;
        old_labs[slot] = bind->lab;
      }
    } else if (bind->forked && saved_fork_side >= 0) {
      old_tags[slot] = (saved_fork_side == 0) ? BJ0 : BJ1;
      old_labs[slot] = bind->lab;
    } else {
      old_tags[slot] = BJV;
      old_labs[slot] = 0;
    }
  }

  parse_consume(s, "{");
  parse_skip(s);

  return parse_term_sup_fork_core(s, dyn, lab_term, lab, depth,
    names, old_depths, old_tags, old_labs, cloned, n, saved_fork_side);
}

fn Term parse_term(PState *s, u32 depth);
fn Term parse_term_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth);
fn Term parse_term_sup_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth);
fn Term parse_term_auto_fork(PState *s, int dyn, Term lab_term, u32 lab, u32 depth);

fn Term parse_term_sup(PState *s, u32 depth) {
  parse_skip(s);
  if (parse_peek(s) == '{') {
    parse_consume(s, "{");
    parse_consume(s, "}");
    return term_new_era();
  }
  int  dyn      = parse_peek(s) == '(';
  Term lab_term = 0;
  u32  lab      = 0;
  if (dyn) {
    parse_consume(s, "(");
    lab_term = parse_term(s, depth);
    parse_consume(s, ")");
  } else {
    lab = parse_name_num(s);
  }
  parse_skip(s);
  // Fork: &Lλx,y,z{A;B} or &(L)λx,y,z{A;B}
  if (parse_match(s, "λ")) {
    return parse_term_fork(s, dyn, lab_term, lab, depth);
  }
  // Auto-fork: &L!{A;B} or &(L)!{A;B}
  if (parse_peek(s) == '!' && parse_peek_at(s, 1) == '{') {
    parse_consume(s, "!");
    return parse_term_auto_fork(s, dyn, lab_term, lab, depth);
  }
  // Sup-fork: &L[x,y,z]{A;B} or &(L)[x,y,z]{A;B}
  if (parse_peek(s) == '[') {
    parse_consume(s, "[");
    return parse_term_sup_fork(s, dyn, lab_term, lab, depth);
  }
  // Regular sup: &L{A,B} or &(L){A,B} or &L{A B}
  parse_consume(s, "{");
  Term tm0 = parse_term(s, depth);
  parse_skip(s);
  parse_sep(s);
  Term tm1 = parse_term(s, depth);
  parse_skip(s);
  parse_sep(s);
  parse_consume(s, "}");
  return dyn ? term_new_dsu(lab_term, tm0, tm1) : term_new_sup(lab, tm0, tm1);
}

fn Term parse_term(PState *s, u32 depth);

fn Term parse_term_ctr(PState *s, u32 depth) {
  u32  nam = parse_name_ref(s);
  Term args[16];
  u32  cnt = 0;
  parse_skip(s);
  if (parse_match(s, "{")) {
    parse_skip(s);
    while (parse_peek(s) != '}') {
      if (cnt >= 16) {
        parse_error(s, "at most 16 constructor fields", parse_peek(s));
      }
      args[cnt++] = parse_term(s, depth);
      parse_skip(s);
      parse_sep(s);
      parse_skip(s);
    }
    parse_consume(s, "}");
  }
  return term_new_ctr(nam, cnt, args);
}

fn Term parse_term_ref(PState *s) {
  return term_new_ref(parse_name_ref(s));
}

fn Term parse_term(PState *s, u32 depth);

// ^name or ^(f x)
fn Term parse_term_nam(PState *s, u32 depth) {
  parse_skip(s);
  if (parse_peek(s) == '(') {
    // ^(f x) -> DRY(f, x)
    parse_consume(s, "(");
    Term f = parse_term(s, depth);
    Term x = parse_term(s, depth);
    parse_consume(s, ")");
    return term_new_dry(f, x);
  } else {
    // ^name -> NAM(name)
    u32 nam = parse_name_num(s);
    return term_new(0, NAM, nam, 0);
  }
}

fn Term parse_term(PState *s, u32 depth);

fn Term parse_term_par(PState *s, u32 depth) {
  Term term = parse_term(s, depth);
  parse_consume(s, ")");
  return term;
}

fn Term parse_term_num(PState *s) {
  parse_skip(s);
  u32 n = 0;
  int has = 0;
  while (isdigit(parse_peek(s))) {
    has = 1;
    n = n * 10 + (u32)(parse_peek(s) - '0');
    parse_advance(s);
  }
  if (!has) {
    parse_error(s, "number", parse_peek(s));
  }
  parse_skip(s);
  return term_new_num(n);
}

fn Term parse_term(PState *s, u32 depth);

fn Term parse_term_nat(PState *s, u32 depth) {
  u32 sav = s->pos;
  u32 num = 0;
  while (isdigit(parse_peek(s))) {
    num = num * 10 + (parse_peek(s) - '0');
    parse_advance(s);
  }
  if (parse_peek(s) != 'n') { s->pos = sav; return 0; }
  parse_advance(s);
  Term t = parse_match(s, "+") ? parse_term(s, depth) : term_new_ctr(SYM_ZER, 0, 0);
  for (u32 i = 0; i < num; i++) t = term_new_ctr(SYM_SUC, 1, &t);
  return t;
}

fn u32 parse_char_esc(PState *s) {
  if (parse_peek(s) == 0) {
    parse_error(s, "character", 0);
  }
  if (parse_peek(s) == '\\') {
    parse_advance(s);
    char c = parse_peek(s);
    if (c == 0) {
      parse_error(s, "character", 0);
    }
    parse_advance(s);
    switch (c) {
      case 'n': return '\n';
      case 't': return '\t';
      case 'r': return '\r';
      case '0': return '\0';
      case '\\': return '\\';
      case '\'': return '\'';
      case '"': return '"';
      default: return (u32)(u8)c;
    }
  }
  return parse_utf8(s);
}

fn u32 parse_char_lit(PState *s) {
  parse_advance(s);
  u32 c = parse_char_esc(s);
  if (parse_peek(s) != '\'') {
    parse_error(s, "'", parse_peek(s));
  }
  parse_advance(s);
  return c;
}

fn Term parse_term_chr(PState *s) {
  u32 c = parse_char_lit(s);
  Term n = term_new_num(c);
  return term_new_ctr(SYM_CHR, 1, &n);
}

fn Term parse_term(PState *s, u32 depth);
fn u32  parse_char_esc(PState *s);

fn Term parse_term_str(PState *s, u32 depth) {
  char fence = parse_peek(s);
  parse_advance(s);
  Term t = term_new_ctr(SYM_NIL, 0, 0);
  u32 cs[4096]; u32 n = 0;
  while (parse_peek(s) != fence) {
    u32 c = parse_char_esc(s);
    cs[n++] = c;
  }
  parse_advance(s);
  for (int i = n - 1; i >= 0; i--) {
    Term c = term_new_num(cs[i]);
    Term h = term_new_ctr(SYM_CHR, 1, &c);
    Term a[2] = {h, t};
    t = term_new_ctr(SYM_CON, 2, a);
  }
  return t;
}

fn Term parse_term(PState *s, u32 depth);

fn Term parse_term_lst(PState *s, u32 depth) {
  parse_advance(s);
  parse_skip(s);
  if (parse_peek(s) == ']') { parse_advance(s); return term_new_ctr(SYM_NIL, 0, 0); }
  Term es[4096]; u32 n = 0;
  while (parse_peek(s) != ']') {
    es[n++] = parse_term(s, depth);
    parse_skip(s);
    parse_sep(s);
    parse_skip(s);
  }
  parse_consume(s, "]");
  Term t = term_new_ctr(SYM_NIL, 0, 0);
  for (int i = n - 1; i >= 0; i--) {
    Term a[2] = {es[i], t};
    t = term_new_ctr(SYM_CON, 2, a);
  }
  return t;
}

fn Term parse_term_var(PState *s, u32 depth) {
  parse_skip(s);
  u32 nam = parse_name(s);
  parse_skip(s);
  int side = parse_match(s, "₀") ? 0 : parse_match(s, "₁") ? 1 : -1;
  parse_skip(s);
  int skipped;
  PBind* bind = parse_bind_lookup(nam, side, &skipped);
  if (bind == NULL) {
    parse_error_var(s, nam, side == -1, skipped);
  }
  if (side == -1) {
    if (bind->side >= 0) {
      side = bind->side;
    } else if (bind->forked) {
      side = PARSE_FORK_SIDE;
    }
  }
  // Handle dynamic dup binding (lab=PARSE_DYN_LAB marker)
  // For dynamic dup, X₀ and X₁ become BJV references to nested lambdas
  if (bind->lab == PARSE_DYN_LAB) {
    u32 offset = (side == 1) ? 1 : 0;
    return term_new(0, BJV, 0, (u32)bind->lvl + offset);
  }
  u64 val = (u32)bind->lvl;
  u32 lab = bind->lab;
  u8  tag = (side == 0) ? BJ0 : (side == 1) ? BJ1 : BJV;
  return term_new(0, tag, lab, val);
}

fn Term parse_term_any(void) {
  return term_new_any();
}

fn Term parse_term(PState *s, u32 depth);

// Parses argument list: (a,b,c,...) and returns count
// Stores args in provided array
fn u32 parse_term_args(PState *s, u32 depth, Term *args, u32 max_args) {
  u32 cnt = 0;
  parse_skip(s);
  while (parse_peek(s) != ')') {
    if (cnt >= max_args) {
      parse_error(s, "too many arguments", parse_peek(s));
    }
    args[cnt++] = parse_term(s, depth);
    parse_skip(s);
    parse_sep(s);
    parse_skip(s);
  }
  parse_consume(s, ")");
  return cnt;
}

fn Term parse_term(PState *s, u32 depth);

// Returns operator precedence (higher = binds tighter)
fn int parse_term_opr_prec(int op) {
  switch (op) {
    case OP_OR:  return 1;
    case OP_AND: return 2;
    case OP_EQ: case OP_NE: return 3;
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: return 4;
    case OP_LSH: case OP_RSH: return 5;
    case OP_ADD: case OP_SUB: return 6;
    case OP_MUL: case OP_DIV: case OP_MOD: return 7;
    case OP_XOR: return 8;
    default: return 0;
  }
}

// Peek at next operator without consuming. Returns op code or -1.
fn int parse_term_opr_peek(PState *s) {
  parse_skip(s);
  char c = parse_peek(s);
  char c1 = parse_peek_at(s, 1);

  if (c == '=' && c1 == '=') return OP_EQ;
  if (c == '!' && c1 == '=') return OP_NE;
  if (c == '<' && c1 == '=') return OP_LE;
  if (c == '>' && c1 == '=') return OP_GE;
  if (c == '<' && c1 == '<') return OP_LSH;
  if (c == '>' && c1 == '>') return OP_RSH;
  if (c == '&' && c1 == '&') return OP_AND;
  if (c == '|' && c1 == '|') return OP_OR;

  if (c == '+') return OP_ADD;
  if (c == '-') return OP_SUB;
  if (c == '*') return OP_MUL;
  if (c == '/') return OP_DIV;
  if (c == '%') return OP_MOD;
  if (c == '^') return OP_XOR;
  if (c == '~') return OP_NOT;
  if (c == '<') return OP_LT;
  if (c == '>') return OP_GT;

  return -1;
}

// Consume an operator (call after peek confirms one exists)
fn void parse_term_opr_consume(PState *s, int op) {
  parse_skip(s);
  parse_advance(s);
  // Two-character operators need second advance
  if (op == OP_EQ || op == OP_NE || op == OP_LE || op == OP_GE ||
      op == OP_LSH || op == OP_RSH || op == OP_AND || op == OP_OR) {
    parse_advance(s);
  }
}

// Try to match an infix operator. Returns the op code or -1 if no match.
// If matched, advances the parser past the operator.
fn int parse_term_opr_match(PState *s) {
  parse_skip(s);
  char c = parse_peek(s);
  char c1 = parse_peek_at(s, 1);

  // Two-character operators
  if (c == '=' && c1 == '=') { parse_advance(s); parse_advance(s); return OP_EQ; }
  if (c == '!' && c1 == '=') { parse_advance(s); parse_advance(s); return OP_NE; }
  if (c == '<' && c1 == '=') { parse_advance(s); parse_advance(s); return OP_LE; }
  if (c == '>' && c1 == '=') { parse_advance(s); parse_advance(s); return OP_GE; }
  if (c == '<' && c1 == '<') { parse_advance(s); parse_advance(s); return OP_LSH; }
  if (c == '>' && c1 == '>') { parse_advance(s); parse_advance(s); return OP_RSH; }
  if (c == '&' && c1 == '&') { parse_advance(s); parse_advance(s); return OP_AND; }
  if (c == '|' && c1 == '|') { parse_advance(s); parse_advance(s); return OP_OR; }

  // Single-character operators (check they're not part of something else)
  if (c == '+') { parse_advance(s); return OP_ADD; }
  if (c == '-') { parse_advance(s); return OP_SUB; }
  if (c == '*') { parse_advance(s); return OP_MUL; }
  if (c == '/') { parse_advance(s); return OP_DIV; }
  if (c == '%') { parse_advance(s); return OP_MOD; }
  if (c == '^') { parse_advance(s); return OP_XOR; }
  if (c == '~') { parse_advance(s); return OP_NOT; }
  if (c == '<') { parse_advance(s); return OP_LT; }
  if (c == '>') { parse_advance(s); return OP_GT; }

  return -1;
}

fn Term parse_term(PState *s, u32 depth);
fn Term parse_term_atom(PState *s, u32 depth);
fn int parse_term_opr_peek(PState *s);
fn void parse_term_opr_consume(PState *s, int op);
fn int parse_term_opr_prec(int op);

fn Term parse_term_app_prec(Term f, PState *s, u32 depth, int min_prec);

fn Term parse_term_app(Term f, PState *s, u32 depth) {
  return parse_term_app_prec(f, s, depth, 0);
}

fn Term parse_term_app_prec(Term f, PState *s, u32 depth, int min_prec) {
  parse_skip(s);
  if (parse_match(s, "<>")) {
    Term t = parse_term(s, depth);
    Term a[2] = {f, t};
    return parse_term_app_prec(term_new_ctr(SYM_CON, 2, a), s, depth, min_prec);
  }
  // Structural equality: === (must check before == for numeric)
  if (parse_match(s, "===")) {
    Term rhs = parse_term_atom(s, depth);
    rhs = parse_term_app_prec(rhs, s, depth, 4);  // same precedence as ==
    return parse_term_app_prec(term_new_eql(f, rhs), s, depth, min_prec);
  }
  // Short-circuit AND: .&.
  if (parse_match(s, ".&.")) {
    Term rhs = parse_term_atom(s, depth);
    rhs = parse_term_app_prec(rhs, s, depth, 2);  // same precedence as &&
    return parse_term_app_prec(term_new_and(f, rhs), s, depth, min_prec);
  }
  // Short-circuit OR: .|.
  if (parse_match(s, ".|.")) {
    Term rhs = parse_term_atom(s, depth);
    rhs = parse_term_app_prec(rhs, s, depth, 1);  // same precedence as ||
    return parse_term_app_prec(term_new_or(f, rhs), s, depth, min_prec);
  }
  // Precedence climbing for infix operators
  int op = parse_term_opr_peek(s);
  if (op >= 0 && parse_term_opr_prec(op) >= min_prec) {
    parse_term_opr_consume(s, op);
    Term rhs = parse_term_atom(s, depth);
    // Parse higher-precedence ops on the right first
    rhs = parse_term_app_prec(rhs, s, depth, parse_term_opr_prec(op) + 1);
    f = term_new_op2(op, f, rhs);
    // Continue at same precedence level (left-associative)
    return parse_term_app_prec(f, s, depth, min_prec);
  }
  if (parse_peek(s) != '(') {
    return f;
  }
  parse_consume(s, "(");
  if (parse_peek(s) == ')') {
    parse_consume(s, ")");
    return parse_term_app_prec(f, s, depth, min_prec);
  }
  while (1) {
    Term arg = parse_term(s, depth);
    f = term_new_app(f, arg);
    parse_skip(s);
    parse_sep(s);
    parse_skip(s);
    if (parse_peek(s) == ')') {
      parse_consume(s, ")");
      break;
    }
  }
  return parse_term_app_prec(f, s, depth, min_prec);
}

// Parse INC: ↑x
fn Term parse_term_inc(PState *s, u32 depth) {
  Term x = parse_term_atom(s, depth);
  return term_new_inc(x);
}

// Parse a single atom (no trailing operators or function calls)
fn Term parse_term_atom(PState *s, u32 depth) {
  parse_skip(s);
  if (parse_match(s, "λ")) {
    return parse_term_lam(s, depth);
  } else if (parse_match(s, "!")) {
    return parse_term_dup(s, depth);
  } else if (parse_match(s, "&")) {
    return parse_term_sup(s, depth);
  } else if (parse_match(s, "#")) {
    return parse_term_ctr(s, depth);
  } else if (parse_match(s, "@")) {
    return parse_term_ref(s);
  } else if (parse_match(s, "^")) {
    return parse_term_nam(s, depth);
  } else if (parse_match(s, "↑")) {
    return parse_term_inc(s, depth);
  } else if (parse_match(s, "*")) {
    return parse_term_any();
  } else if (parse_match(s, "(")) {
    return parse_term_par(s, depth);
  } else if (parse_peek(s) == '[') {
    return parse_term_lst(s, depth);
  } else if (parse_peek(s) == '\'') {
    return parse_term_chr(s);
  } else if (parse_peek(s) == '"' || parse_peek(s) == '`') {
    return parse_term_str(s, depth);
  } else if (isdigit(parse_peek(s))) {
    Term t = parse_term_nat(s, depth);
    if (!t) t = parse_term_num(s);
    return t;
  } else {
    return parse_term_var(s, depth);
  }
}

fn Term parse_term(PState *s, u32 depth) {
  return parse_term_app(parse_term_atom(s, depth), s, depth);
}

fn void parse_def(PState *s);

fn void parse_include(PState *s) {
  // Parse filename
  parse_skip(s);
  parse_consume(s, "\"");
  u32 start = s->pos;
  while (parse_peek(s) != '"' && !parse_at_end(s)) {
    parse_advance(s);
  }
  u32 len = s->pos - start;
  parse_consume(s, "\"");

  // Resolve path
  char filename[256], path[1024];
  if (len >= sizeof(filename)) {
    sys_error("include path exceeds parser buffer (255 chars)");
  }
  memcpy(filename, s->src + start, len);
  filename[len] = 0;
  sys_path_join(path, sizeof(path), s->file, filename);

  // Check if already included
  for (u32 i = 0; i < PARSE_SEEN_FILES_LEN; i++) {
    if (strcmp(PARSE_SEEN_FILES[i], path) == 0) {
      return;
    }
  }
  if (PARSE_SEEN_FILES_LEN >= 1024) {
    sys_error("MAX_INCLUDES");
  }
  PARSE_SEEN_FILES[PARSE_SEEN_FILES_LEN++] = strdup(path);

  // Read and parse
  char *src = sys_file_read(path);
  if (!src) {
    fprintf(stderr, "Error: could not open '%s'\n", path);
    exit(1);
  }
  PState sub = {
    .file = PARSE_SEEN_FILES[PARSE_SEEN_FILES_LEN - 1],
    .src  = src,
    .pos  = 0,
    .len  = strlen(src),
    .line = 1,
    .col  = 1
  };
  parse_def(&sub);
  free(src);
}

fn void parse_def(PState *s) {
  parse_skip(s);
  if (parse_at_end(s)) {
    return;
  }
  if (parse_match(s, "#include")) {
    parse_include(s);
    parse_def(s);
    return;
  }
  if (parse_match(s, "@")) {
    u32 id = parse_name_ref(s);
    parse_consume(s, "=");
    PARSE_BINDS_LEN = 0;
    Term val        = parse_term(s, 0);
    u64  loc        = heap_alloc(1);
    HEAP[loc]       = val;
    BOOK[id]        = loc;
    parse_def(s);
    return;
  }
  parse_error(s, "definition or #include", parse_peek(s));
}

// Parse Program Entry
// -------------------
// Parses one source buffer as a top-level HVM program.

fn void parse_def(PState *s);

// Seeds parser state for one source text and parses all top-level definitions.
fn void parse_program(const char *source_path, char *src) {
  const char *path = source_path != NULL ? source_path : "<source>";
  char *file = strdup(path);
  if (file == NULL) {
    sys_error("Source path allocation failed");
  }

  if (PARSE_SEEN_FILES_LEN >= 1024) {
    free(file);
    sys_error("MAX_INCLUDES");
  }
  PARSE_SEEN_FILES[PARSE_SEEN_FILES_LEN++] = file;

  PState s = {
    .file = file,
    .src  = src,
    .pos  = 0,
    .len  = strlen(src),
    .line = 1,
    .col  = 1
  };
  parse_def(&s);
}

// WNF
// ===

fn Term graph_ref(u32 nam);
fn Term graph_expand(Term clo);

// WNF Interaction Counter Toggle
// ==============================
// Controls whether WNF interaction counters are active.

// Enables or disables interaction counting.
fn void wnf_set_itrs_enabled(int enabled) {
  ITRS_ENABLED = enabled != 0;
}

fn void wnf_stack_init(void) {
  WnfBank *bank = &WNF_BANK;
  if (bank->stack) {
    return;
  }

  u64 bytes = WNF_CAP * sizeof(Term);
  Term *stack      = (Term *)sys_mmap_anon(bytes);
  u8    stack_mmap = 1;

  if (stack == NULL) {
    stack = (Term *)malloc(bytes);
    stack_mmap = 0;
  }

  if (!stack) {
    fprintf(stderr, "wnf: stack allocation failed\n");
    exit(1);
  }

  bank->stack       = stack;
  bank->stack_bytes = bytes;
  bank->stack_mmap  = stack_mmap;
  bank->s_pos       = 1;
}

fn void wnf_stack_free(void) {
  WnfBank *bank = &WNF_BANK;
  if (!bank->stack) {
    return;
  }
  if (bank->stack_mmap) {
    sys_munmap_anon(bank->stack, bank->stack_bytes);
  } else {
    free(bank->stack);
  }
  bank->stack       = NULL;
  bank->stack_bytes = 0;
  bank->stack_mmap  = 0;
  bank->s_pos       = 0;
}

fn u64 wnf_itrs_total(void) {
  return ITRS;
}

// (&{} a)
// ------- APP-ERA
// &{}
fn Term wnf_app_era(void) {
  ITRS_INC("APP-ERA");
  return term_new_era();
}

// (name a)
// --------- APP-NAM
// ^(name a)
fn Term wnf_app_nam(u64 app_loc, Term nam) {
  heap_set(app_loc + 0, nam);
  return term_new(0, DRY, 0, app_loc);
}

// (^(f x) a)
// ----------- APP-DRY
// ^(^(f x) a)
fn Term wnf_app_dry(u64 app_loc, Term dry) {
  heap_set(app_loc + 0, dry);
  return term_new(0, DRY, 0, app_loc);
}

// (λx.f a)
// -------- APP-LAM
// x ← a
// f
fn Term wnf_app_lam(Term lam, Term arg) {
  ITRS_INC("APP-LAM");
  u64  loc     = term_val(lam);
  u32  lam_ext = term_ext(lam);
  Term body    = heap_read(loc);
  if (lam_ext & LAM_ERA_MASK) {
    heap_free_term(arg);
    heap_free(loc, 1);
    return body;
  }
  heap_subst_var(loc, arg);
  return body;
}

// (&L{f,g} a)
// ----------------- APP-SUP
// ! A &L = a
// &L{(f A₀),(g A₁)}
fn Term wnf_app_sup(u64 app_loc, Term sup, Term arg) {
  ITRS_INC("APP-SUP");
  u64  sup_loc = term_val(sup);
  u32  lab     = term_ext(sup);
  Term tm1     = heap_read(sup_loc + 1);
  Copy D = term_clone(lab, arg);
  heap_set(sup_loc + 1, D.k0);
  Term ap0 = term_new(0, APP, 0, sup_loc);
  Term ap1 = term_new_app(tm1, D.k1);
  return term_new_sup_at(app_loc, lab, ap0, ap1);
}

// (↑f x)
// -------- APP-INC
// ↑(f x)
fn Term wnf_app_inc(Term app, Term inc) {
  ITRS_INC("APP-INC");
  u64  app_loc = term_val(app);
  u64  inc_loc = term_val(inc);
  Term f       = heap_read(inc_loc);
  // Build APP(f, x) in-place at app_loc, then store it under INC at inc_loc.
  heap_set(app_loc + 0, f);
  heap_set(inc_loc + 0, term_new(0, APP, 0, app_loc));
  return inc;
}

// (λ{#K:h; m} &L{a,b})
// -------------------- APP-MAT-SUP
// ! H &L = h
// ! M &L = m
// &L{(λ{#K:H₀; M₀} a)
//   ,(λ{#K:H₁; M₁} b)}
fn Term wnf_app_mat_sup(Term mat, Term sup) {
  ITRS_INC("APP-MAT-SUP");
  u32  lab = term_ext(sup);
  Copy M   = term_clone(lab, mat);
  u64  loc = term_val(sup);
  Term a   = heap_read(loc + 0);
  Term b   = heap_read(loc + 1);
  return term_new_sup_at(loc, lab, term_new_app(M.k0, a), term_new_app(M.k1, b));
}

// (λ{#K:h; m} #K{a,b})
// -------------------- APP-MAT-CTR-MAT
// (h a b)
//
// (λ{#K:h; m} #L{a,b})
// -------------------- APP-MAT-CTR-MIS
// (m #L{a,b})
fn Term wnf_app_mat_ctr(Term mat, Term ctr) {
  u32 mat_ext = term_ext(mat);
  u32 ctr_ext = term_ext(ctr);
  u64 mat_loc = term_val(mat);
  if (mat_ext == ctr_ext) {
    ITRS_INC("APP-MAT-CTR-MAT");
    u32 ari = term_tag(ctr) - C00;
    Term res = heap_read(mat_loc);
    Term nxt = heap_read(mat_loc + 1);
    heap_free_term(nxt);
    if (ari == 0) {
      heap_free(mat_loc, 2);
      return res;
    }
    u64 ctr_loc = term_val(ctr);
    // Reuse MAT node storage for the first APP in the chain.
    Term arg0 = heap_read(ctr_loc + 0);
    res = term_new_app_at(mat_loc, res, arg0);
    if (ari == 1) {
      heap_free(ctr_loc, 1);
      return res;
    }
    // Reuse CTR node storage for the second APP.
    Term arg1 = heap_read(ctr_loc + 1);
    res = term_new_app_at(ctr_loc, res, arg1);
    if (ari == 2) {
      return res;
    }
    u64 apps = heap_alloc(2 * (u64)(ari - 2));
    for (u32 i = 2; i < ari; i++) {
      res = term_new_app_at(apps + 2 * (u64)(i - 2), res, heap_read(ctr_loc + i));
    }
    heap_free(ctr_loc + 2, ari - 2);
    return res;
  } else {
    ITRS_INC("APP-MAT-CTR-MIS");
    Term h = heap_read(mat_loc + 0);
    Term g = heap_read(mat_loc + 1);
    heap_free_term(h);
    return term_new_app_at(mat_loc, g, ctr);
  }
}

// (λ{#a:h; m} #a)
// --------------- APP-MAT-NUM-MAT
// h
//
// (λ{#a:h; m} #b)
// --------------- APP-MAT-NUM-MIS
// (m #b)
fn Term wnf_app_mat_num(Term mat, Term num) {
  u64 mat_loc = term_val(mat);
  u32 mat_ext = term_ext(mat);
  u64 num_val = term_val(num);
  if (mat_ext == num_val) {
    ITRS_INC("APP-MAT-NUM-MAT");
    Term res = heap_read(mat_loc + 0);
    Term nxt = heap_read(mat_loc + 1);
    heap_free_term(nxt);
    heap_free(mat_loc, 2);
    return res;
  } else {
    ITRS_INC("APP-MAT-NUM-MIS");
    Term h = heap_read(mat_loc + 0);
    Term g = heap_read(mat_loc + 1);
    heap_free_term(h);
    return term_new_app_at(mat_loc, g, num);
  }
}

// (λ{...} ↑x)
// ------------ MAT-INC
// ↑(λ{...} x)
fn Term wnf_mat_inc(Term mat, Term inc) {
  ITRS_INC("MAT-INC");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term app     = term_new_app(mat, x);
  heap_set(inc_loc, app);
  return term_new(0, INC, 0, inc_loc);
}

// ! X &L = name
// ------------ DUP-NAM
// X₀ ← name
// X₁ ← name
fn Term wnf_dup_nam(u32 lab, u64 loc, u8 side, Term nam) {
  ITRS_INC("DUP-NAM");
  heap_subst_var_dup(loc, nam);
  return nam;
}

// ! F &L = λx.f
// ---------------- DUP-LAM
// F₀ ← λ$x0.G₀
// F₁ ← λ$x1.G₁
// x  ← &L{$x0,$x1}
// ! G &L = f
fn Term wnf_dup_lam(u32 lab, u64 loc, u8 side, Term lam) {
  ITRS_INC("DUP-LAM");
  u64  lam_loc        = term_val(lam);
  u32  lam_ext        = term_ext(lam);
  Term bod            = heap_read(lam_loc);

  if (lam_ext & LAM_ERA_MASK) {
    heap_free(lam_loc, 1);
    u64  a      = heap_alloc(3);
    heap_set(a + 2, bod);
    Copy B      = term_clone_at(a + 2, lab);
    heap_set(a + 0, B.k0);
    heap_set(a + 1, B.k1);
    Term l0     = term_new(0, LAM, lam_ext, a + 0);
    Term l1     = term_new(0, LAM, lam_ext, a + 1);
    return heap_subst_cop(side, loc, l0, l1);
  }

  u64  a       = heap_alloc(5);
  heap_set(a + 4, bod);
  Copy B       = term_clone_at(a + 4, lab);
  heap_set(a + 0, B.k0);
  heap_set(a + 1, B.k1);
  heap_set(a + 2, term_new(0, VAR, 0, a + 0));
  heap_set(a + 3, term_new(0, VAR, 0, a + 1));
  Term su      = term_new(0, SUP, lab, a + 2);
  Term l0      = term_new(0, LAM, lam_ext, a + 0);
  Term l1      = term_new(0, LAM, lam_ext, a + 1);
  heap_subst_var(lam_loc, su);
  return heap_subst_cop(side, loc, l0, l1);
}

// ! X &L = &R{a,b}
// ---------------- DUP-SUP
// if L == R:
//   X₀ ← a
//   X₁ ← b
// else:
//   ! A &L = a
//   ! B &L = b
//   X₀ ← &R{A₀,B₀}
//   X₁ ← &R{A₁,B₁}
fn Term wnf_dup_sup(u32 lab, u64 loc, u8 side, Term sup) {
  ITRS_INC("DUP-SUP");
  u64 sup_loc = term_val(sup);
  u32 sup_lab = term_ext(sup);
  if (lab == sup_lab) {
    Term tm0 = heap_read(sup_loc + 0);
    Term tm1 = heap_read(sup_loc + 1);
    heap_free(sup_loc, 2);
    return heap_subst_cop(side, loc, tm0, tm1);
  } else {
    u64 base = heap_alloc(4);
    u64 at   = base;
    Copy A  = term_clone_at(sup_loc + 0, lab);
    Copy B  = term_clone_at(sup_loc + 1, lab);
    Term s0 = term_new_sup_at(at + 0, sup_lab, A.k0, B.k0);
    Term s1 = term_new_sup_at(at + 2, sup_lab, A.k1, B.k1);
    return heap_subst_cop(side, loc, s0, s1);
  }
}

// ! X &L = T{a,b,...}
// ------------------- DUP-NOD
// ! A &L = a
// ! B &L = b
// ...
// X₀ ← T{A₀,B₀,...}
// X₁ ← T{A₁,B₁,...}
fn Term wnf_dup_nod(u32 lab, u64 loc, u8 side, Term term) {
  ITRS_INC("DUP-NOD");
  u32 ari = term_arity(term);
  if (ari == 0) {
    heap_subst_var_dup(loc, term);
    return term;
  }
  u64  t_loc = term_val(term);
  u32  t_ext = term_ext(term);
  u8   t_tag = term_tag(term);
  u64  block = heap_alloc(2 * (u64)ari);
  u64  r0_loc = block;
  u64  r1_loc = block + ari;
  for (u32 i = 0; i < ari; i++) {
    Copy A = term_clone_at(t_loc + i, lab);
    heap_set(r0_loc + i, A.k0);
    heap_set(r1_loc + i, A.k1);
  }
  Term r0 = term_new(0, t_tag, t_ext, r0_loc);
  Term r1 = term_new(0, t_tag, t_ext, r1_loc);
  return heap_subst_cop(side, loc, r0, r1);
}

// @{s} n
// ------ ALO-VAR
// s[n] or n when substitution missing (n is a de Bruijn level)
fn Term wnf_alo_var(u64 ls, u32 len, Term book) {
  u32 lvl = (u32)term_val(book);
  u8  tag = term_tag(book);
  u32 ext = term_ext(book);
  if (lvl == 0 || lvl > len) {
    return term_new(0, tag, ext, lvl);
  }
  u32 idx = len - lvl;
  u64 it  = ls;
  for (u32 i = 0; i < idx && it != 0; i++) {
    it = term_val(heap_read(it + 1));
  }
  return it != 0 ? term_new_var(it) : term_new(0, tag, ext, lvl);
}

// @{s} n₀
// ------- ALO-DP0
// s[n]₀ or n₀ when substitution missing (n is a de Bruijn level)
//
// @{s} n₁
// ------- ALO-DP1
// s[n]₁ or n₁ when substitution missing (n is a de Bruijn level)
fn Term wnf_alo_cop(u64 ls, u32 len, Term book) {
  u32 lvl  = (u32)term_val(book);
  u32 lab  = term_ext(book);
  u8  tag  = term_tag(book);
  u8  side = (tag == DP0 || tag == BJ0) ? 0 : 1;
  if (lvl == 0 || lvl > len) {
    return term_new(0, tag, lab, lvl);
  }
  u32 idx = len - lvl;
  u64 it  = ls;
  for (u32 i = 0; i < idx && it != 0; i++) {
    it = term_val(heap_read(it + 1));
  }
  u8 rtag = side == 0 ? DP0 : DP1;
  return it != 0 ? term_new(0, rtag, lab, it) : term_new(0, tag, lab, lvl);
}

// @{s} λx.f
// ------------ ALO-LAM
// x' ← fresh
// λx'.@{x',s}f
fn Term wnf_alo_lam(u64 alo_loc, u64 ls_loc, u32 len, Term book) {
  u32 lam_ext  = term_ext(book);
  u64 lam_body = term_val(book);
  u64 bind_loc = heap_alloc(2);
  u64 loc      = (len > 0) ? alo_loc : heap_alloc(1);
  Term alo     = term_new_alo_at(loc, bind_loc, len + 1, lam_body);
  heap_set(bind_loc + 0, alo);
  heap_set(bind_loc + 1, term_new(0, NUM, 0, ls_loc));
  return term_new(0, LAM, lam_ext, bind_loc + 0);
}

fn Term wnf_alo_lam_app(u64 alo_loc, u64 ls_loc, u32 len, Term book, Term arg) {
  ITRS_INC("APP-LAM");
  u32 lam_ext  = term_ext(book);
  u64 lam_body = term_val(book);
  u64 bind_loc = heap_alloc(2);
  u64 loc      = (len > 0) ? alo_loc : heap_alloc(1);
  Term body    = term_new_alo_at(loc, bind_loc, len + 1, lam_body);
  heap_set(bind_loc + 0, body);
  heap_set(bind_loc + 1, term_new(0, NUM, 0, ls_loc));
  if (lam_ext & LAM_ERA_MASK) {
    heap_free_term(arg);
    heap_free(bind_loc, 1);
    return body;
  }
  heap_subst_var(bind_loc, arg);
  return body;
}

// @{s} ! x &L = v; t
// ------------------ ALO-DUP
// x' ← fresh
// ! x' &L = @{s} v
// @{x',s} t
fn Term wnf_alo_dup(u64 alo_loc, u64 ls_loc, u16 len, Term book) {
  u64 book_loc = term_val(book);
  u64 bind_ent = heap_alloc(2);
  Term alo_v = term_new_alo_at(alo_loc, ls_loc, len, book_loc + 0);
  heap_set(bind_ent + 0, alo_v);
  heap_set(bind_ent + 1, term_new(0, NUM, 0, ls_loc));
  return term_new_alo(bind_ent, len + 1, book_loc + 1);
}

// @{s} T{a,b,...}
// ---------------- ALO-NOD
// T{@{s}a, @{s}b, ...}
fn Term wnf_alo_nod(u64 alo_loc, u64 ls_loc, u32 len, Term book) {
  u64 loc = term_val(book);
  u8  tag = term_tag(book);
  u32 ext = term_ext(book);
  u32 ari = term_arity(book);
  Term args[16];
  if (ari == 0) {
    if (len > 0) {
      heap_free(alo_loc, 1);
    }
    return book;
  }
  args[0] = term_new_alo_at(alo_loc, ls_loc, len, loc + 0);
  for (u32 i = 1; i < ari; i++) {
    args[i] = term_new_alo(ls_loc, len, loc + i);
  }
  return term_new_(tag, ext, ari, args);
}

fn Term wnf_alo_arg(u64 ls_loc, u32 len, u64 tm_loc) {
  return term_new_alo(ls_loc, len, tm_loc);
}

fn Term wnf_alo_at(u64 alo_loc, u64 ls_loc, u32 len, u64 tm_loc) {
  return len > 0 ? term_new_alo_at(alo_loc, ls_loc, len, tm_loc) : term_new_alo(0, 0, tm_loc);
}

fn Term wnf_alo_mat_ctr(u64 app_loc, u64 alo_loc, u64 ls_loc, u32 len, Term mat, Term ctr) {
  u32 mat_ext = term_ext(mat);
  u32 ctr_ext = term_ext(ctr);
  u64 mat_loc = term_val(mat);
  if (mat_ext == ctr_ext) {
    ITRS_INC("APP-MAT-CTR-MAT");
    u32 ari = term_tag(ctr) - C00;
    Term res = wnf_alo_at(alo_loc, ls_loc, len, mat_loc + 0);
    if (ari == 0) {
      heap_free(app_loc, 2);
      return res;
    }
    u64 ctr_loc = term_val(ctr);
    res = term_new_app_at(app_loc, res, heap_read(ctr_loc + 0));
    if (ari == 1) {
      heap_free(ctr_loc, 1);
      return res;
    }
    res = term_new_app_at(ctr_loc, res, heap_read(ctr_loc + 1));
    if (ari == 2) {
      return res;
    }
    u64 apps = heap_alloc(2 * (u64)(ari - 2));
    for (u32 i = 2; i < ari; i++) {
      res = term_new_app_at(apps + 2 * (u64)(i - 2), res, heap_read(ctr_loc + i));
    }
    heap_free(ctr_loc + 2, ari - 2);
    return res;
  }
  ITRS_INC("APP-MAT-CTR-MIS");
  Term nxt = wnf_alo_at(alo_loc, ls_loc, len, mat_loc + 1);
  return term_new_app_at(app_loc, nxt, ctr);
}

fn Term wnf_alo_mat_num(u64 app_loc, u64 alo_loc, u64 ls_loc, u32 len, Term mat, Term num) {
  u64 mat_loc = term_val(mat);
  u32 mat_ext = term_ext(mat);
  u64 num_val = term_val(num);
  if (mat_ext == num_val) {
    ITRS_INC("APP-MAT-NUM-MAT");
    Term res = wnf_alo_at(alo_loc, ls_loc, len, mat_loc + 0);
    heap_free(app_loc, 2);
    return res;
  }
  ITRS_INC("APP-MAT-NUM-MIS");
  Term nxt = wnf_alo_at(alo_loc, ls_loc, len, mat_loc + 1);
  return term_new_app_at(app_loc, nxt, num);
}

fn Term wnf_alo_mat_frame_num(Term frame, Term num) {
  u64 app_loc = term_val(frame);
  u32 len     = term_ext(frame);
  u64 alo_loc = 0;
  u64 ls_loc  = 0;
  u64 tm_loc  = 0;
  if (len == 0) {
    tm_loc = heap_read(app_loc + 0);
  } else {
    alo_loc = heap_read(app_loc + 0);
    u64 pair = heap_read(alo_loc);
    ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
    tm_loc = pair & ALO_TM_MASK;
  }
  return wnf_alo_mat_num(app_loc, alo_loc, ls_loc, len, heap_read(tm_loc), num);
}

fn Term wnf_alo_mat_frame_ctr(Term frame, Term ctr) {
  u64 app_loc = term_val(frame);
  u32 len     = term_ext(frame);
  u64 alo_loc = 0;
  u64 ls_loc  = 0;
  u64 tm_loc  = 0;
  if (len == 0) {
    tm_loc = heap_read(app_loc + 0);
  } else {
    alo_loc = heap_read(app_loc + 0);
    u64 pair = heap_read(alo_loc);
    ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
    tm_loc = pair & ALO_TM_MASK;
  }
  return wnf_alo_mat_ctr(app_loc, alo_loc, ls_loc, len, heap_read(tm_loc), ctr);
}

// @@opr(&{}, y)
// ------------- OP2-ERA
// &{}
fn Term wnf_op2_era() {
  ITRS_INC("OP2-ERA");
  return term_new_era();
}

// @@opr(&L{a,b}, y)
// ------------------------- OP2-SUP
// ! Y &L = y
// &L{@@opr(a,Y₀), @@opr(b,Y₁)}
fn Term wnf_op2_sup(u64 loc, u32 opr, Term sup, Term y) {
  ITRS_INC("OP2-SUP");
  u32  lab     = term_ext(sup);
  u64  sup_loc = term_val(sup);
  Copy Y       = term_clone(lab, y);
  Term op0     = term_new_op2_at(loc, opr, heap_read(sup_loc + 0), Y.k0);
  Term op1     = term_new_op2(opr, heap_read(sup_loc + 1), Y.k1);
  return term_new_sup_at(sup_loc, lab, op0, op1);
}

// (x op &{}) where x is NUM
// -------------- OP2-NUM-ERA
// &{}
fn Term wnf_op2_num_era() {
  ITRS_INC("OP2-NUM-ERA");
  return term_new_era();
}

// (#a op #b)
// -------------- OP2-NUM-NUM
// #(a opr b)
fn Term wnf_op2_num_num_raw(u32 opr, u32 a, u32 b) {
  ITRS_INC("OP2-NUM-NUM");
  return term_new_num(term_op2_u32(opr, a, b));
}

fn Term wnf_op2_num_num(u32 opr, Term x, Term y) {
  return wnf_op2_num_num_raw(opr, (u32)term_val(x), (u32)term_val(y));
}

// (x op &L{a,b}) where x is NUM
// ------------------------- OP2-NUM-SUP
// ! X &L = x
// &L{(X₀ op a), (X₁ op b)}
fn Term wnf_op2_num_sup(u32 opr, Term x, Term sup) {
  ITRS_INC("OP2-NUM-SUP");
  u32  lab     = term_ext(sup);
  u64  sup_loc = term_val(sup);
  Term op0     = term_new_op2(opr, x, heap_read(sup_loc + 0));
  Term op1     = term_new_op2(opr, x, heap_read(sup_loc + 1));
  return term_new_sup_at(sup_loc, lab, op0, op1);
}

// (↑x op y)
// ---------- OP2-INC-X
// ↑(x op y)
fn Term wnf_op2_inc_x(u64 op2_loc, u32 opr, Term inc, Term y) {
  ITRS_INC("OP2-INC-X");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term op      = term_new_op2_at(op2_loc, opr, x, y);
  heap_set(inc_loc, op);
  return inc;
}

// (#n op ↑y)
// ---------- OP2-INC-Y
// ↑(#n op y)
fn Term wnf_op2_inc_y(u32 opr, Term x, Term inc) {
  ITRS_INC("OP2-INC-Y");
  u64  inc_loc = term_val(inc);
  Term y       = heap_read(inc_loc);
  Term op      = term_new_op2(opr, x, y);
  heap_set(inc_loc, op);
  return inc;
}

// &(&{}){a, b}
// ------------ DSU-ERA
// &{}
fn Term wnf_dsu_era() {
  ITRS_INC("DSU-ERA");
  return term_new_era();
}

// &(#n){a, b}
// ----------- DSU-NUM
// &n{a, b}
fn Term wnf_dsu_num(u64 dsu_loc, Term lab_num, Term a, Term b) {
  ITRS_INC("DSU-NUM");
  u32 lab = term_val(lab_num);
  Term sup = term_new_sup_at(dsu_loc, lab, a, b);
  heap_free(dsu_loc + 2, 1);
  return sup;
}

// &(&L{x,y}){a, b}
// -------------------------- DSU-SUP
// ! A &L = a
// ! B &L = b
// &L{&(x){A₀,B₀}, &(y){A₁,B₁}}
fn Term wnf_dsu_sup(u64 dsu_loc, Term lab_sup, Term a, Term b) {
  ITRS_INC("DSU-SUP");
  u32  lab     = term_ext(lab_sup);
  u64  sup_loc = term_val(lab_sup);
  Copy A;
  Copy B;
  term_clone2(lab, a, b, &A, &B);
  Term ds0     = term_new_dsu_at(dsu_loc, heap_read(sup_loc + 0), A.k0, B.k0);
  Term ds1     = term_new_dsu(heap_read(sup_loc + 1), A.k1, B.k1);
  return term_new_sup_at(sup_loc, lab, ds0, ds1);
}

// &(↑x){a, b}
// ------------ DSU-INC
// ↑(&(x){a, b})
fn Term wnf_dsu_inc(u64 dsu_loc, Term inc, Term a, Term b) {
  ITRS_INC("DSU-INC");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term new_dsu = term_new_dsu_at(dsu_loc, x, a, b);
  heap_set(inc_loc, new_dsu);
  return inc;
}

// ! X &(&{}) = v; b
// ----------------- DDU-ERA
// &{}
fn Term wnf_ddu_era() {
  ITRS_INC("DDU-ERA");
  return term_new_era();
}

// ! X &(#n) = v; b
// ---------------- DDU-NUM
// ! X &n = v
// b(X₀, X₁)
fn Term wnf_ddu_num(u64 ddu_loc, Term lab_num, Term val, Term bod) {
  ITRS_INC("DDU-NUM");
  u32 lab   = term_val(lab_num);
  heap_set(ddu_loc + 2, val);
  Copy V    = term_clone_at(ddu_loc + 2, lab);
  Term app0 = term_new_app_at(ddu_loc, bod, V.k0);
  Term app1 = term_new_app(app0, V.k1);
  return app1;
}

// ! X &(&L{x,y}) = v; b
// ------------------------------ DDU-SUP
// ! V &L = v
// ! B &L = b
// &L{! X &(x) = V₀; B₀, ! X &(y) = V₁; B₁}
fn Term wnf_ddu_sup(u64 ddu_loc, Term lab_sup, Term val, Term bod) {
  ITRS_INC("DDU-SUP");
  u32  lab     = term_ext(lab_sup);
  u64  sup_loc = term_val(lab_sup);
  Copy V;
  Copy B;
  term_clone2(lab, val, bod, &V, &B);
  Term dd0     = term_new_ddu_at(ddu_loc, heap_read(sup_loc + 0), V.k0, B.k0);
  Term dd1     = term_new_ddu(heap_read(sup_loc + 1), V.k1, B.k1);
  return term_new_sup_at(sup_loc, lab, dd0, dd1);
}

// ! X &(↑x) = v; b
// ---------------- DDU-INC
// ↑(! X &(x) = v; b)
fn Term wnf_ddu_inc(u64 ddu_loc, Term inc, Term val, Term bod) {
  ITRS_INC("DDU-INC");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term new_ddu = term_new_ddu_at(ddu_loc, x, val, bod);
  heap_set(inc_loc, new_ddu);
  return inc;
}

// (λ{f} &{})
// ---------- USE-ERA
// &{}
fn Term wnf_use_era() {
  ITRS_INC("USE-ERA");
  return term_new_era();
}

// (λ{f} &L{a,b})
// ----------------- USE-SUP
// ! F &L = f
// &L{(λ{F₀} a), (λ{F₁} b)}
fn Term wnf_use_sup(Term use, Term sup) {
  ITRS_INC("USE-SUP");
  u64  use_loc = term_val(use);
  u32  lab     = term_ext(sup);
  u64  sup_loc = term_val(sup);
  Copy F       = term_clone_at(use_loc, lab);
  Term use0    = term_new_use(F.k0);
  Term use1    = term_new_use(F.k1);
  Term app0    = term_new_app(use0, heap_read(sup_loc + 0));
  Term app1    = term_new_app(use1, heap_read(sup_loc + 1));
  return term_new_sup_at(sup_loc, lab, app0, app1);
}

// (λ{f} x)
// --------- USE-VAL
// (f x)
fn Term wnf_use_val(Term use, Term val) {
  ITRS_INC("USE-VAL");
  u64  loc = term_val(use);
  Term f   = heap_read(loc);
  heap_free(loc, 1);
  return term_new_app(f, val);
}

// (use ↑x)
// --------- USE-INC
// ↑(use x)
fn Term wnf_use_inc(Term use, Term inc) {
  ITRS_INC("USE-INC");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term app     = term_new_app(use, x);
  heap_set(inc_loc, app);
  return term_new(0, INC, 0, inc_loc);
}

// (&{} === b)
// ----------- EQL-ERA-L
// &{}
fn Term wnf_eql_era_l(void) {
  ITRS_INC("EQL-ERA-L");
  return term_new_era();
}

// (a === &{})
// ----------- EQL-ERA-R
// &{}
fn Term wnf_eql_era_r(void) {
  ITRS_INC("EQL-ERA-R");
  return term_new_era();
}

// (* === b)
// ----------- EQL-ANY-L
// 1
fn Term wnf_eql_any_l(void) {
  ITRS_INC("1");
  return term_new_num(1);
}

// (a === *)
// ----------- EQL-ANY-R
// 1
fn Term wnf_eql_any_r(void) {
  ITRS_INC("1");
  return term_new_num(1);
}

// (&L{a0,a1} === b)
// ---------------------- EQL-SUP-L
// ! B &L = b
// &L{(a0 === B₀), (a1 === B₁)}
fn Term wnf_eql_sup_l(u64 eql_loc, Term sup, Term b) {
  ITRS_INC("EQL-SUP-L");
  u64  sup_loc = term_val(sup);
  u32  lab = term_ext(sup);
  Term a0  = heap_read(sup_loc + 0);
  Term a1  = heap_read(sup_loc + 1);
  Copy B   = term_clone(lab, b);
  Term eq0 = term_new_eql_at(eql_loc, a0, B.k0);
  Term eq1 = term_new_eql(a1, B.k1);
  return term_new_sup_at(sup_loc, lab, eq0, eq1);
}

// (a === &L{b0,b1})
// ---------------------- EQL-SUP-R
// ! A &L = a
// &L{(A₀ === b0), (A₁ === b1)}
fn Term wnf_eql_sup_r(u64 eql_loc, Term a, Term sup) {
  ITRS_INC("EQL-SUP-R");
  u64  sup_loc = term_val(sup);
  u32  lab = term_ext(sup);
  Term b0  = heap_read(sup_loc + 0);
  Term b1  = heap_read(sup_loc + 1);
  Copy A   = term_clone(lab, a);
  Term eq0 = term_new_eql_at(eql_loc, A.k0, b0);
  Term eq1 = term_new_eql(A.k1, b1);
  return term_new_sup_at(sup_loc, lab, eq0, eq1);
}

// (#a === #b)
// ------------ EQL-NUM
// #(a == b)
fn Term wnf_eql_num(Term a, Term b) {
  ITRS_INC("EQL-NUM");
  u32 av = (u32)term_val(a);
  u32 bv = (u32)term_val(b);
  return term_new_num(av == bv ? 1 : 0);
}

// (λax.af === λbx.bf)
// ------------------- EQL-LAM
// X := fresh_nam()
// ax ← X
// bx ← X
// af === bf
fn Term wnf_eql_lam(u64 eql_loc, Term a, Term b) {
  ITRS_INC("EQL-LAM");
  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);
  u32  a_ext = term_ext(a);
  u32  b_ext = term_ext(b);
  Term af    = heap_read(a_loc);
  Term bf    = heap_read(b_loc);
  // Generate fresh name for substitution
  u32 fresh = FRESH++;
  Term nam = term_new_nam(fresh);
  // Substitute both variable locations with the same name
  if (a_ext & LAM_ERA_MASK) {
    heap_free(a_loc, 1);
  } else {
    heap_subst_var(a_loc, nam);
  }
  if (b_ext & LAM_ERA_MASK) {
    heap_free(b_loc, 1);
  } else {
    heap_subst_var(b_loc, nam);
  }
  return term_new_eql_at(eql_loc, af, bf);
}

// (#K{a0,a1...} === #K{b0,b1...})  (same tag)
// --------------------------------------- EQL-CTR-MAT
// For SUC (1n+): ↑(pred === pred)
// For CON (<>): ↑((head === head) & ↑(tail === tail))
// Others: (a0 === b0) & (a1 === b1) & ...
//
// (#K{...} === #L{...})  (different tag)
// ------------------------------------- EQL-CTR-MIS
// #0
fn Term wnf_eql_ctr(u64 eql_loc, Term a, Term b) {
  ITRS_INC("EQL-CTR-MIS");
  u32 a_tag = term_tag(a);
  u32 b_tag = term_tag(b);
  u32 a_ext = term_ext(a);
  u32 b_ext = term_ext(b);

  // Different constructor tags or names -> #0
  if (a_tag != b_tag || a_ext != b_ext) {
    heap_free(eql_loc, 2);
    heap_free_term(a);
    heap_free_term(b);
    return term_new_num(0);
  }

  u32 arity = a_tag - C00;

  // Arity 0: equal
  if (arity == 0) {
    heap_free(eql_loc, 2);
    return term_new_num(1);
  }

  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);

  // SUC (1n+): recursive natural - wrap in INC for priority
  if (a_ext == SYM_SUC && arity == 1) {
    Term a0 = heap_read(a_loc);
    Term b0 = heap_read(b_loc);
    heap_free(a_loc, 1);
    heap_free(b_loc, 1);
    Term eq = term_new_eql_at(eql_loc, a0, b0);
    return term_new_inc(eq);
  }

  // CON (<>): recursive list - wrap tail and whole in INC
  if (a_ext == SYM_CON && arity == 2) {
    Term a0 = heap_read(a_loc + 0);
    Term a1 = heap_read(a_loc + 1);
    Term b0 = heap_read(b_loc + 0);
    Term b1 = heap_read(b_loc + 1);
    heap_free(a_loc, 2);
    heap_free(b_loc, 2);
    Term eq_h = term_new_eql_at(eql_loc, a0, b0);
    Term eq_t = term_new_inc(term_new_eql(a1, b1));
    return term_new_inc(term_new_and(eq_h, eq_t));
  }

  // Other constructors: no INC, just AND chain
  Term a_args[16];
  Term b_args[16];
  for (u32 i = 0; i < arity; i++) {
    a_args[i] = heap_read(a_loc + i);
    b_args[i] = heap_read(b_loc + i);
  }
  heap_free(a_loc, arity);
  heap_free(b_loc, arity);
  Term result = term_new_eql_at(eql_loc, a_args[0], b_args[0]);
  for (u32 i = 1; i < arity; i++) {
    Term eq_i = term_new_eql(a_args[i], b_args[i]);
    result = term_new_and(result, eq_i);
  }
  return result;
}

// (λ{#K:ah;am} === λ{#K:bh;bm})  (same tag)
// ----------------------------------------- EQL-MAT-MAT
// (ah === bh) & (am === bm)
//
// (λ{#K:...} === λ{#L:...})  (different tag)
// ----------------------------------------- EQL-MAT-MIS
// #0
fn Term wnf_eql_mat(u64 eql_loc, Term a, Term b) {
  ITRS_INC("EQL-MAT-MIS");
  u32 a_ext = term_ext(a);
  u32 b_ext = term_ext(b);

  // Different match tags -> #0
  if (a_ext != b_ext) {
    heap_free(eql_loc, 2);
    heap_free_term(a);
    heap_free_term(b);
    return term_new_num(0);
  }

  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);
  Term ah    = heap_read(a_loc + 0);
  Term am    = heap_read(a_loc + 1);
  Term bh    = heap_read(b_loc + 0);
  Term bm    = heap_read(b_loc + 1);
  heap_free(a_loc, 2);
  heap_free(b_loc, 2);

  // (ah === bh) .&. (am === bm)
  Term eq_h = term_new_eql(ah, bh);
  Term eq_m = term_new_eql(am, bm);
  return term_new_and_at(eql_loc, eq_h, eq_m);
}

// (λ{af} === λ{bf})
// ----------------- EQL-USE
// af === bf
fn Term wnf_eql_use(u64 eql_loc, Term a, Term b) {
  ITRS_INC("EQL-USE");
  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);
  Term af    = heap_read(a_loc);
  Term bf    = heap_read(b_loc);
  heap_free(a_loc, 1);
  heap_free(b_loc, 1);
  return term_new_eql_at(eql_loc, af, bf);
}

// (name === name)  (same tag/ext/val)
// -----------------------------------
// #1
//
// (name === other) (different tag/ext/val)
// ----------------------------------------
// #0
fn Term wnf_eql_nam(Term a, Term b) {
  ITRS_INC("-");
  u8  a_tag = term_tag(a);
  u8  b_tag = term_tag(b);
  u32 a_ext = term_ext(a);
  u32 b_ext = term_ext(b);
  u64 a_val = term_val(a);
  u64 b_val = term_val(b);
  return term_new_num((a_tag == b_tag) && (a_ext == b_ext) && (a_val == b_val));
}

// (^(af ax) === ^(bf bx))
// ----------------------- EQL-DRY
// (af === bf) & (ax === bx)
fn Term wnf_eql_dry(u64 eql_loc, Term a, Term b) {
  ITRS_INC("EQL-DRY");
  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);
  Term af    = heap_read(a_loc + 0);
  Term ax    = heap_read(a_loc + 1);
  Term bf    = heap_read(b_loc + 0);
  Term bx    = heap_read(b_loc + 1);
  heap_free(a_loc, 2);
  heap_free(b_loc, 2);

  // (af === bf) .&. (ax === bx)
  Term eq_f = term_new_eql(af, bf);
  Term eq_x = term_new_eql(ax, bx);
  return term_new_and_at(eql_loc, eq_f, eq_x);
}

// (↑a === b)
// ----------- EQL-INC-L
// ↑(a === b)
fn Term wnf_eql_inc_l(u64 loc, Term inc, Term b) {
  ITRS_INC("EQL-INC-L");
  u64  inc_loc = term_val(inc);
  Term a       = heap_read(inc_loc);
  Term eql     = term_new_eql_at(loc, a, b);
  heap_set(inc_loc, eql);
  return inc;
}

// (a === ↑b)
// ----------- EQL-INC-R
// ↑(a === b)
fn Term wnf_eql_inc_r(u64 loc, Term a, Term inc) {
  ITRS_INC("EQL-INC-R");
  u64  inc_loc = term_val(inc);
  Term b       = heap_read(inc_loc);
  Term eql     = term_new_eql_at(loc, a, b);
  heap_set(inc_loc, eql);
  return inc;
}

// (&{} .&. b)
// ----------- AND-ERA
// &{}
fn Term wnf_and_era(void) {
  ITRS_INC("AND-ERA");
  return term_new_era();
}

// (&L{a0,a1} .&. b)
// -------------------------- AND-SUP
// ! B &L = b
// &L{(a0 .&. B₀), (a1 .&. B₁)}
fn Term wnf_and_sup(u64 and_loc, Term sup, Term b) {
  ITRS_INC("AND-SUP");
  u32  sup_lab = term_ext(sup);
  u64  sup_loc = term_val(sup);
  Copy  B = term_clone(sup_lab, b);
  Term a0 = heap_read(sup_loc + 0);
  Term a1 = heap_read(sup_loc + 1);
  Term r0 = term_new_and_at(and_loc, a0, B.k0);
  Term r1 = term_new_and(a1, B.k1);
  return term_new_sup_at(sup_loc, sup_lab, r0, r1);
}

// (#0 .&. b)
// ---------- AND-ZER
// #0
//
// (#n .&. b)   [n ≠ 0]
// -------------------- AND-ONE
// b
fn Term wnf_and_num(Term num, Term b) {
  u64 val = term_val(num);
  if (val == 0) {
    ITRS_INC("AND-ZER");
    return term_new_num(0);
  } else {
    ITRS_INC("AND-ONE");
    return b;
  }
}

// (↑a & b)
// --------- AND-INC
// ↑(a & b)
fn Term wnf_and_inc(u64 and_loc, Term inc, Term b) {
  ITRS_INC("AND-INC");
  u64  inc_loc = term_val(inc);
  Term a       = heap_read(inc_loc);
  heap_set(and_loc + 0, a);
  heap_set(inc_loc, term_new(0, AND, 0, and_loc));
  return inc;
}

// (&{} .|. b)
// ----------- OR-ERA
// &{}
fn Term wnf_or_era(void) {
  ITRS_INC("OR-ERA");
  return term_new_era();
}

// (&L{a0,a1} .|. b)
// -------------------------- OR-SUP
// ! B &L = b
// &L{(a0 .|. B₀), (a1 .|. B₁)}
fn Term wnf_or_sup(u64 or_loc, Term sup, Term b) {
  ITRS_INC("OR-SUP");
  u64  sup_loc = term_val(sup);
  u32  lab = term_ext(sup);
  Term a0  = heap_read(sup_loc + 0);
  Term a1  = heap_read(sup_loc + 1);
  Copy B   = term_clone(lab, b);
  Term r0 = term_new_or_at(or_loc, a0, B.k0);
  Term r1 = term_new_or(a1, B.k1);
  return term_new_sup_at(sup_loc, lab, r0, r1);
}

// (#0 .|. b)
// ---------- OR-ZER
// b
//
// (#n .|. b)   [n ≠ 0]
// -------------------- OR-ONE
// #1
fn Term wnf_or_num(Term num, Term b) {
  u64 val = term_val(num);
  if (val == 0) {
    ITRS_INC("OR-ZER");
    return b;
  } else {
    ITRS_INC("OR-ONE");
    return term_new_num(1);
  }
}

// (↑a | b)
// --------- OR-INC
// ↑(a | b)
fn Term wnf_or_inc(u64 loc, Term inc, Term b) {
  ITRS_INC("OR-INC");
  u64  inc_loc = term_val(inc);
  Term a       = heap_read(inc_loc);
  Term or_tm   = term_new_or_at(loc, a, b);
  heap_set(inc_loc, or_tm);
  return inc;
}

// ! ${f, v}; t
// ------------- WNF UNS
// t(λy.λ$x.y, $x)
fn Term wnf_uns(Term uns) {
  ITRS_INC("WNF-UNS");
  u64  uns_loc = term_val(uns);
  Term bod     = heap_read(uns_loc + 0);
  heap_free(uns_loc, 1);
  u64  loc     = heap_alloc(2);
  Term x_var   = term_new_var(loc + 0);
  Term y_var   = term_new_var(loc + 1);
  Term x_lam   = term_new_lam_at(loc + 0, y_var);
  Term y_lam   = term_new_lam_at(loc + 1, x_lam);
  return term_new_app(term_new_app(bod, y_lam), x_var);
}

// WNF uses an explicit stack to avoid recursion.
// - Enter/reduce: walk into the head position, pushing eliminators as frames.
//   APP/OP2/EQL/AND/OR/DSU/DDU push their term as a frame and descend into the
//   left/strict field (APP fun, OP2 lhs, etc). DP0/DP1 push and descend into the
//   shared dup expr. MAT/USE add specialized frames when their scrutinee is ready.
// - Apply: once WHNF is reached, pop frames and dispatch the interaction using
//   the WHNF result. Frames reuse existing heap nodes to avoid allocations.
__attribute__((cold, noinline)) static Term wnf_rebuild(Term cur, Term *stack, u32 s_pos, u32 base) {
  while (s_pos > base) {
    Term frame = stack[--s_pos];

    switch (term_tag(frame)) {
      case APP: {
        u64  loc = term_val(frame);
        Term arg = heap_read(loc + 1);
        cur = term_new_app_at(loc, cur, arg);
        break;
      }
      case MAT:
      case SWI:
      case USE: {
        cur = term_new_app(frame, cur);
        break;
      }
      case DP0:
      case DP1: {
        u64 loc = term_val(frame);
        heap_set(loc, cur);
        cur = frame;
        break;
      }
      case OP2: {
        u64 loc = term_val(frame);
        heap_set(loc + 0, cur);
        cur = frame;
        break;
      }
      case F_OP2_NUM: {
        u32  opr = term_ext(frame);
        Term x   = term_new_num((u32)term_val(frame));
        cur = term_new_op2(opr, x, cur);
        break;
      }
      case EQL:
      case AND:
      case OR:
      case DSU:
      case DDU: {
        u64 loc = term_val(frame);
        heap_set(loc + 0, cur);
        cur = frame;
        break;
      }
      case F_EQL_R: {
        u64 loc = term_val(frame);
        heap_set(loc + 1, cur);
        cur = term_new(0, EQL, 0, loc);
        break;
      }
      case F_ALO_MAT: {
        u64 loc = term_val(frame);
        u32 len = term_ext(frame);
        u64 alo_loc = 0;
        u64 ls_loc = 0;
        u64 tm_loc = 0;
        if (len == 0) {
          tm_loc = heap_read(loc + 0);
        } else {
          alo_loc = heap_read(loc + 0);
          u64 pair = heap_read(alo_loc);
          ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
          tm_loc = pair & ALO_TM_MASK;
        }
        Term mat = wnf_alo_nod(alo_loc, ls_loc, len, heap_read(tm_loc));
        cur = term_new_app_at(loc, mat, cur);
        break;
      }
      default: {
        break;
      }
    }
  }

  WNF_S_POS = s_pos;
  return cur;
}

__attribute__((hot)) fn Term wnf(Term term) {
  wnf_stack_init();
  Term *stack = WNF_STACK;
  u32  s_pos  = WNF_S_POS;
  u32  base   = s_pos;
  Term next   = term;
  Term whnf;

  enter: {
    if (__builtin_expect(STEPS_ITRS_LIM != 0, 0) && ITRS >= STEPS_ITRS_LIM) {
      return wnf_rebuild(next, stack, s_pos, base);
    }
    if (__builtin_expect(DEBUG, 0)) {
      printf("wnf_enter: ");
      print_term(next);
      printf("\n");
    }

    PROF_INC(PROF_ENTER, term_tag(next));
    switch (term_tag(next)) {
      case VAR: {
        u64 loc = term_val(next);
        Term cell = heap_read(loc);
        if (term_sub_get(cell)) {
          next = term_sub_set(cell, 0);
          heap_free(loc, 1);
          goto enter;
        }
        whnf = next;
        goto apply;
      }

      case DP0:
      case DP1: {
        u64 loc = term_val(next);
        Term cell = heap_take(loc);
        if (term_sub_get(cell)) {
          next = term_sub_set(cell, 0);
          heap_free(loc, 1);
          goto enter;
        }
        stack[s_pos++] = next;
        next = cell;
        goto enter;
      }

      case APP: {
        u64  loc = term_val(next);
        Term fun = heap_read(loc);
        stack[s_pos++] = next;
        next = fun;
        goto enter;
      }

      case DUP: {
        u64  loc  = term_val(next);
        Term body = heap_read(loc + 1);
        heap_free(loc + 1, 1);
        next = body;
        goto enter;
      }

      case UNS: {
        next = wnf_uns(next);
        goto enter;
      }

      case REF: {
        u32 nam = term_ext(next);
        Term clo = graph_ref(nam);
        if (clo != 0) {
          next = clo;
          goto enter;
        }
        if (BOOK[nam] != 0) {
          next = term_new_alo(0, 0, BOOK[nam]);
          goto enter;
        }
        whnf = next;
        goto apply;
      }

      case CLO: {
        next = graph_expand(next);
        goto enter;
      }

      case ALO: {
        u32 len     = term_ext(next);
        u64 alo_loc;
        u64 tm_loc;
        u64 ls_loc;
        if (len == 0) {
          alo_loc = 0;
          tm_loc = term_val(next);
          ls_loc = 0;
        } else {
          alo_loc = term_val(next);
          u64 pair = heap_read(alo_loc);
          ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
          tm_loc = pair & ALO_TM_MASK;
        }
        Term book    = heap_read(tm_loc);
        PROF_INC(PROF_ALO_BOOK, term_tag(book));
        if (len == 0) {
          PROF_INC(PROF_ALO_BOOK_CLOSED, term_tag(book));
        } else {
          PROF_INC(PROF_ALO_BOOK_OPEN, term_tag(book));
        }

        switch (term_tag(book)) {
          case VAR:
          case BJV: {
            if (len > 0) {
              heap_free(alo_loc, 1);
            }
            next = wnf_alo_var(ls_loc, len, book);
            goto enter;
          }
          case DP0:
          case DP1:
          case BJ0:
          case BJ1: {
            if (len > 0) {
              heap_free(alo_loc, 1);
            }
            next = wnf_alo_cop(ls_loc, len, book);
            goto enter;
          }
          case LAM: {
            if (s_pos > base && term_tag(stack[s_pos - 1]) == APP) {
              Term frame   = stack[--s_pos];
              u64  app_loc = term_val(frame);
              Term arg     = heap_read(app_loc + 1);
              heap_free(app_loc, 2);
              next = wnf_alo_lam_app(alo_loc, ls_loc, len, book, arg);
              goto enter;
            }
            next = wnf_alo_lam(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case DUP: {
            next = wnf_alo_dup(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case APP: {
            u64 book_loc = term_val(book);
            u64 app_loc  = heap_alloc(2);
            Term fun     = wnf_alo_at(alo_loc, ls_loc, len, book_loc + 0);
            Term arg     = wnf_alo_arg(ls_loc, len, book_loc + 1);
            heap_set(app_loc + 1, arg);
            stack[s_pos++] = term_new(0, APP, 0, app_loc);
            next = fun;
            goto enter;
          }
          case DRY:
          case SUP: {
            next = wnf_alo_nod(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case MAT:
          case SWI: {
            if (s_pos > base && term_tag(stack[s_pos - 1]) == APP) {
              Term frame   = stack[--s_pos];
              u64  app_loc = term_val(frame);
              Term arg     = heap_read(app_loc + 1);
              heap_set(app_loc + 0, len > 0 ? (Term)alo_loc : (Term)tm_loc);
              stack[s_pos++] = term_new(0, F_ALO_MAT, len, app_loc);
              next = arg;
              goto enter;
            }
            next = wnf_alo_nod(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case USE: {
            if (s_pos > base && term_tag(stack[s_pos - 1]) == APP) {
              Term frame   = stack[--s_pos];
              u64  app_loc = term_val(frame);
              Term arg     = heap_read(app_loc + 1);
              Term fun     = wnf_alo_nod(alo_loc, ls_loc, len, book);
              heap_free(app_loc, 2);
              stack[s_pos++] = fun;
              next = arg;
              goto enter;
            }
            next = wnf_alo_nod(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case UNS:
          case INC:
          case OP2:
          case EQL:
          case AND:
          case OR:
          case DSU:
          case DDU: {
            next = wnf_alo_nod(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case C00 ... C16: {
            Term val = wnf_alo_nod(alo_loc, ls_loc, len, book);
            if (s_pos > base && term_tag(stack[s_pos - 1]) == F_ALO_MAT) {
              Term frame = stack[--s_pos];
              next = wnf_alo_mat_frame_ctr(frame, val);
              goto enter;
            }
            next = val;
            goto enter;
          }
          case NAM:
          case ERA:
          case ANY: {
            if (len > 0) {
              heap_free(alo_loc, 1);
            }
            whnf = book;
            goto apply;
          }
          case NUM: {
            if (s_pos > base && term_tag(stack[s_pos - 1]) == F_ALO_MAT) {
              Term frame = stack[--s_pos];
              if (len > 0) {
                heap_free(alo_loc, 1);
              }
              next = wnf_alo_mat_frame_num(frame, book);
              goto enter;
            }
            if (len > 0) {
              heap_free(alo_loc, 1);
            }
            whnf = book;
            goto apply;
          }
          case REF: {
            if (len > 0) {
              heap_free(alo_loc, 1);
            }
            u32 nam = term_ext(book);
            if (BOOK[nam] != 0) {
              next = term_new_alo(0, 0, BOOK[nam]);
              goto enter;
            }
            whnf = book;
            goto apply;
          }
        }
      }

      case OP2: {
        u64  loc = term_val(next);
        Term x   = heap_read(loc + 0);
        stack[s_pos++] = next;
        next = x;
        goto enter;
      }

      case EQL: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        stack[s_pos++] = next;
        next = a;
        goto enter;
      }

      case AND: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        stack[s_pos++] = next;
        next = a;
        goto enter;
      }

      case OR: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        stack[s_pos++] = next;
        next = a;
        goto enter;
      }

      case DSU: {
        u64  loc = term_val(next);
        Term lab = heap_read(loc + 0);
        stack[s_pos++] = next;
        next = lab;
        goto enter;
      }

      case DDU: {
        u64  loc = term_val(next);
        Term lab = heap_read(loc + 0);
        stack[s_pos++] = next;
        next = lab;
        goto enter;
      }

      case NAM:
      case BJV:
      case BJ0:
      case BJ1:
      case DRY:
      case ERA:
      case SUP:
      case LAM:
      case NUM:
      case MAT:
      case SWI:
      case USE:
      case INC:
      case C00 ... C16: {
        whnf = next;
        goto apply;
      }

      default: {
        whnf = next;
        goto apply;
      }
    }
  }

  apply: {
    if (__builtin_expect(DEBUG, 0)) {
      printf("wnf_apply: ");
      print_term(whnf);
      printf("\n");
    }

    while (s_pos > base) {
      if (__builtin_expect(STEPS_ITRS_LIM != 0, 0) && ITRS >= STEPS_ITRS_LIM) {
        return wnf_rebuild(whnf, stack, s_pos, base);
      }
      Term frame = stack[--s_pos];
      PROF_INC(PROF_FRAME, term_tag(frame));

      switch (term_tag(frame)) {
        // -----------------------------------------------------------------------
        // APP frame: (□ x) - we reduced func, now dispatch
        // -----------------------------------------------------------------------
        case APP: {
          u64  app_loc = term_val(frame);
          Term arg     = heap_read(app_loc + 1);
          PROF_INC(PROF_APP_WHNF, term_tag(whnf));

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(app_loc, 2);
              heap_free_term(arg);
              whnf = wnf_app_era();
              continue;
            }
            case NAM:
            case BJV:
            case BJ0:
            case BJ1: {
              whnf = wnf_app_nam(app_loc, whnf);
              continue;
            }
            case DRY: {
              whnf = wnf_app_dry(app_loc, whnf);
              continue;
            }
            case LAM: {
              heap_free(app_loc, 2);
              next = wnf_app_lam(whnf, arg);
              goto enter;
            }
            case SUP: {
              whnf = wnf_app_sup(app_loc, whnf, arg);
              continue;
            }
            case INC: {
              whnf = wnf_app_inc(frame, whnf);
              continue;
            }
            case MAT:
            case SWI: {
              heap_free(app_loc, 2);
              stack[s_pos++] = whnf;
              next = arg;
              goto enter;
            }
            case USE: {
              heap_free(app_loc, 2);
              stack[s_pos++] = whnf;
              next = arg;
              goto enter;
            }
            case NUM: {
              fprintf(stderr, "RUNTIME_ERROR: cannot apply a number\n");
              exit(1);
            }
            case C00 ... C16: {
              fprintf(stderr, "RUNTIME_ERROR: cannot apply a constructor\n");
              exit(1);
            }
            default: {
              heap_set(app_loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        case F_ALO_MAT: {
          u64 app_loc = term_val(frame);
          u32 len     = term_ext(frame);
          u64 alo_loc = 0;
          u64 ls_loc  = 0;
          u64 tm_loc  = 0;
          if (len == 0) {
            tm_loc = heap_read(app_loc + 0);
          } else {
            alo_loc = heap_read(app_loc + 0);
            u64 pair = heap_read(alo_loc);
            ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
            tm_loc = pair & ALO_TM_MASK;
          }
          Term mat = heap_read(tm_loc);

          switch (term_tag(whnf)) {
            case C00 ... C16: {
              next = wnf_alo_mat_ctr(app_loc, alo_loc, ls_loc, len, mat, whnf);
              goto enter;
            }
            case NUM: {
              next = wnf_alo_mat_num(app_loc, alo_loc, ls_loc, len, mat, whnf);
              goto enter;
            }
            default: {
              Term dyn = wnf_alo_nod(alo_loc, ls_loc, len, mat);
              heap_free(app_loc, 2);
              switch (term_tag(whnf)) {
                case ERA: {
                  heap_free_term(dyn);
                  whnf = wnf_app_era();
                  continue;
                }
                case SUP: {
                  whnf = wnf_app_mat_sup(dyn, whnf);
                  continue;
                }
                case INC: {
                  whnf = wnf_mat_inc(dyn, whnf);
                  continue;
                }
                case NAM:
                case BJV:
                case BJ0:
                case BJ1:
                case DRY: {
                  whnf = term_new_dry(dyn, whnf);
                  continue;
                }
                default: {
                  whnf = term_new_app(dyn, whnf);
                  continue;
                }
              }
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // MAT/SWI frame: (mat □) - we reduced arg, dispatch mat interaction
        // -----------------------------------------------------------------------
        case MAT:
        case SWI: {
          Term mat = frame;
          PROF_INC(PROF_MAT_WHNF, term_tag(whnf));
          switch (term_tag(whnf)) {
            case ERA: {
              heap_free_term(mat);
              whnf = wnf_app_era();
              continue;
            }
            case SUP: {
              whnf = wnf_app_mat_sup(mat, whnf);
              continue;
            }
            case INC: {
              whnf = wnf_mat_inc(mat, whnf);
              continue;
            }
            case C00 ... C16: {
              next = wnf_app_mat_ctr(mat, whnf);
              goto enter;
            }
            case NUM: {
              next = wnf_app_mat_num(mat, whnf);
              goto enter;
            }
            case NAM:
            case BJV:
            case BJ0:
            case BJ1:
            case DRY: {
              // (mat ^n) or (mat ^(f x)): stuck, produce DRY
              whnf = term_new_dry(mat, whnf);
              continue;
            }
            default: {
              whnf = term_new_app(mat, whnf);
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // USE frame: (use □) - we reduced arg, dispatch use interaction
        // -----------------------------------------------------------------------
        case USE: {
          Term use = frame;
          switch (term_tag(whnf)) {
            case ERA: {
              heap_free_term(use);
              whnf = wnf_use_era();
              continue;
            }
            case SUP: {
              whnf = wnf_use_sup(use, whnf);
              continue;
            }
            case INC: {
              whnf = wnf_use_inc(use, whnf);
              continue;
            }
            default: {
              next = wnf_use_val(use, whnf);
              goto enter;
            }
          }
        }

        // -----------------------------------------------------------------------
        // DP0/DP1 frame: DUP node - we reduced the expr, dispatch dup interaction
        // -----------------------------------------------------------------------
        case DP0:
        case DP1: {
          u8  side = (term_tag(frame) == DP0) ? 0 : 1;
          u64 loc  = term_val(frame);
          u32 lab  = term_ext(frame);

          switch (term_tag(whnf)) {
            case NAM:
            case BJV:
            case BJ0:
            case BJ1: {
              whnf = wnf_dup_nam(lab, loc, side, whnf);
              continue;
            }
            case LAM: {
              whnf = wnf_dup_lam(lab, loc, side, whnf);
              continue;
            }
            case SUP: {
              next = wnf_dup_sup(lab, loc, side, whnf);
              goto enter;
            }
            case ERA:
            case ANY:
            case NUM: {
              whnf = wnf_dup_nod(lab, loc, side, whnf);
              continue;
            }
            // case APP: // !! DO NOT ADD: DP0/DP1 do not interact with APP.
            case DRY:
            case MAT:
            case SWI:
            case USE:
            case INC:
            case OP2:
            case DSU:
            case DDU:
            case C00 ... C16: {
              next = wnf_dup_nod(lab, loc, side, whnf);
              goto enter;
            }
            default: {
              heap_set(loc, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // OP2 frame: (□ op y) - we reduced x, dispatch or transition to F_OP2_NUM
        // -----------------------------------------------------------------------
        case OP2: {
          u32  opr = term_ext(frame);
          u64  loc = term_val(frame);
          Term y   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(loc, 2);
              heap_free_term(y);
              whnf = wnf_op2_era();
              continue;
            }
            case NUM: {
              u8 y_tag = term_tag(y);
              if (y_tag == NUM) {
                heap_free(loc, 2);
                whnf = wnf_op2_num_num_raw(opr, (u32)term_val(whnf), (u32)term_val(y));
                continue;
              }
              // x is NUM, now reduce y: push F_OP2_NUM frame
              heap_free(loc, 2);
              stack[s_pos++] = term_new(0, F_OP2_NUM, opr, term_val(whnf));
              next = y;
              goto enter;
            }
            case SUP: {
              whnf = wnf_op2_sup(loc, opr, whnf, y);
              continue;
            }
            case INC: {
              whnf = wnf_op2_inc_x(loc, opr, whnf, y);
              continue;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // F_OP2_NUM frame: (x op □) - x is NUM, we reduced y, dispatch
        // -----------------------------------------------------------------------
        case F_OP2_NUM: {
          u32 opr   = term_ext(frame);
          u32 x_val = (u32)term_val(frame);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_op2_num_era();
              continue;
            }
            case NUM: {
              whnf = wnf_op2_num_num_raw(opr, x_val, (u32)term_val(whnf));
              continue;
            }
            case SUP: {
              Term x = term_new_num(x_val);
              whnf = wnf_op2_num_sup(opr, x, whnf);
              continue;
            }
            case INC: {
              Term x = term_new_num(x_val);
              whnf = wnf_op2_inc_y(opr, x, whnf);
              continue;
            }
            default: {
              // Stuck: (x op y) where x is NUM, y is not
              Term x = term_new_num(x_val);
              whnf = term_new_op2(opr, x, whnf);
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // EQL frame: (□ === b) - we reduced a, transition to F_EQL_R or dispatch
        // -----------------------------------------------------------------------
        case EQL: {
          u64  loc = term_val(frame);
          Term b   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(loc, 2);
              heap_free_term(b);
              whnf = wnf_eql_era_l();
              continue;
            }
            case ANY: {
              heap_free(loc, 2);
              heap_free_term(b);
              whnf = wnf_eql_any_l();
              continue;
            }
            case SUP: {
              whnf = wnf_eql_sup_l(loc, whnf, b);
              continue;
            }
            case INC: {
              whnf = wnf_eql_inc_l(loc, whnf, b);
              continue;
            }
            default: {
              // Store a's WHNF location, push F_EQL_R, enter b
              // We store a in heap_read(loc+0) for later retrieval
              heap_set(loc + 0, whnf);
              stack[s_pos++] = term_new(0, F_EQL_R, 0, loc);
              next = b;
              goto enter;
            }
          }
        }

        // -----------------------------------------------------------------------
        // F_EQL_R frame: (a === □) - we reduced b, now compare both WHNFs
        // -----------------------------------------------------------------------
        case F_EQL_R: {
          u64  loc = term_val(frame);
          Term a   = heap_read(loc + 0);  // a's WHNF was stored here

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(loc, 2);
              heap_free_term(a);
              whnf = wnf_eql_era_r();
              continue;
            }
            case ANY: {
              heap_free(loc, 2);
              heap_free_term(a);
              whnf = wnf_eql_any_r();
              continue;
            }
            case SUP: {
              whnf = wnf_eql_sup_r(loc, a, whnf);
              continue;
            }
            case INC: {
              whnf = wnf_eql_inc_r(loc, a, whnf);
              continue;
            }
            default: {
              // Both a and b are WHNF, now dispatch based on types
              u8 a_tag = term_tag(a);
              u8 b_tag = term_tag(whnf);

              // ANY === x or x === ANY
              if (a_tag == ANY || b_tag == ANY) {
                heap_free(loc, 2);
                heap_free_term(a_tag == ANY ? whnf : a);
                whnf = wnf_eql_any_r();
                continue;
              }
              // NUM === NUM
              if (a_tag == NUM && b_tag == NUM) {
                heap_free(loc, 2);
                whnf = wnf_eql_num(a, whnf);
                continue;
              }
              // LAM === LAM
              if (a_tag == LAM && b_tag == LAM) {
                next = wnf_eql_lam(loc, a, whnf);
                goto enter;
              }
              // CTR === CTR
              if (a_tag >= C00 && a_tag <= C16 && b_tag >= C00 && b_tag <= C16) {
                next = wnf_eql_ctr(loc, a, whnf);
                goto enter;
              }
              // MAT/SWI === MAT/SWI
              if ((a_tag == MAT || a_tag == SWI) && (b_tag == MAT || b_tag == SWI)) {
                next = wnf_eql_mat(loc, a, whnf);
                goto enter;
              }
              // USE === USE
              if (a_tag == USE && b_tag == USE) {
                next = wnf_eql_use(loc, a, whnf);
                goto enter;
              }
              // NAM/BJ* === NAM/BJ*
              if ((a_tag == NAM || a_tag == BJV || a_tag == BJ0 || a_tag == BJ1) &&
                  (b_tag == NAM || b_tag == BJV || b_tag == BJ0 || b_tag == BJ1)) {
                heap_free(loc, 2);
                whnf = wnf_eql_nam(a, whnf);
                continue;
              }
              // DRY === DRY
              if (a_tag == DRY && b_tag == DRY) {
                next = wnf_eql_dry(loc, a, whnf);
                goto enter;
              }
              // Otherwise: not equal
              ITRS_INC("EQL-NOT");
              heap_free(loc, 2);
              heap_free_term(a);
              heap_free_term(whnf);
              whnf = term_new_num(0);
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // DSU frame: &(□){a,b} - we reduced lab, dispatch
        // -----------------------------------------------------------------------
        case DSU: {
          u64  loc = term_val(frame);
          Term a   = heap_read(loc + 1);
          Term b   = heap_read(loc + 2);

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(loc, 3);
              heap_free_term(a);
              heap_free_term(b);
              whnf = wnf_dsu_era();
              continue;
            }
            case NUM: {
              whnf = wnf_dsu_num(loc, whnf, a, b);
              continue;
            }
            case SUP: {
              whnf = wnf_dsu_sup(loc, whnf, a, b);
              continue;
            }
            case INC: {
              whnf = wnf_dsu_inc(loc, whnf, a, b);
              continue;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // DDU frame: ! x &(□) = val; bod - we reduced lab, dispatch
        // -----------------------------------------------------------------------
        case DDU: {
          u64  loc = term_val(frame);
          Term val = heap_read(loc + 1);
          Term bod = heap_read(loc + 2);

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(loc, 3);
              heap_free_term(val);
              heap_free_term(bod);
              whnf = wnf_ddu_era();
              continue;
            }
            case NUM: {
              next = wnf_ddu_num(loc, whnf, val, bod);
              goto enter;
            }
            case SUP: {
              whnf = wnf_ddu_sup(loc, whnf, val, bod);
              continue;
            }
            case INC: {
              whnf = wnf_ddu_inc(loc, whnf, val, bod);
              continue;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // AND frame: (□ .&. b) - we reduced a, dispatch
        // -----------------------------------------------------------------------
        case AND: {
          u64  loc = term_val(frame);
          Term b   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(loc, 2);
              heap_free_term(b);
              whnf = wnf_and_era();
              continue;
            }
            case SUP: {
              whnf = wnf_and_sup(loc, whnf, b);
              continue;
            }
            case INC: {
              whnf = wnf_and_inc(loc, whnf, b);
              continue;
            }
            case NUM: {
              heap_free(loc, 2);
              if (term_val(whnf) == 0) {
                heap_free_term(b);
              }
              next = wnf_and_num(whnf, b);
              goto enter;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // OR frame: (□ .|. b) - we reduced a, dispatch
        // -----------------------------------------------------------------------
        case OR: {
          u64  loc = term_val(frame);
          Term b   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              heap_free(loc, 2);
              heap_free_term(b);
              whnf = wnf_or_era();
              continue;
            }
            case SUP: {
              whnf = wnf_or_sup(loc, whnf, b);
              continue;
            }
            case INC: {
              whnf = wnf_or_inc(loc, whnf, b);
              continue;
            }
            case NUM: {
              heap_free(loc, 2);
              if (term_val(whnf) != 0) {
                heap_free_term(b);
              }
              next = wnf_or_num(whnf, b);
              goto enter;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        default: {
          continue;
        }
      }
    }
  }

  WNF_S_POS = s_pos;
  return whnf;
}

fn Term wnf_at(u64 loc) {
  Term cur = heap_read(loc);
  switch (term_tag(cur)) {
    case NAM:
    case BJV:
    case BJ0:
    case BJ1:
    case DRY:
    case ERA:
    case SUP:
    case LAM:
    case NUM:
    case MAT:
    case SWI:
    case USE:
    case INC:
    case C00 ... C16: {
      return cur;
    }
    default: {
      break;
    }
  }
  Term res = wnf(cur);
  if (res != cur) {
    heap_set(loc, res);
  }
  return res;
}

#define STEPS_DASH_LEN 40

__attribute__((cold, noinline)) fn void steps_print_line(str itr) {
  for (u32 i = 0; i < STEPS_DASH_LEN; i++) {
    fputc('-', stdout);
  }
  if (itr != NULL) {
    fputc(' ', stdout);
    fputs(itr, stdout);
  }
  fputc('\n', stdout);
}

__attribute__((cold, noinline)) fn Term wnf_steps_at(u64 loc) {
  Term cur = heap_read(loc);
  switch (term_tag(cur)) {
    case NAM:
    case BJV:
    case BJ0:
    case BJ1:
    case DRY:
    case ERA:
    case SUP:
    case LAM:
    case NUM:
    case MAT:
    case SWI:
    case USE:
    case INC:
    case C00 ... C16: {
      return cur;
    }
    default: {
      break;
    }
  }

  for (;;) {
    u64 itrs = ITRS;
    STEPS_LAST_ITR = NULL;
    STEPS_ITRS_LIM = itrs + 1;
    Term res = wnf(cur);
    STEPS_ITRS_LIM = 0;
    if (res != cur) {
      heap_set(loc, res);
      cur = res;
    }
    if (ITRS == itrs) {
      break;
    }
    if (!SILENT && STEPS_ROOT_LOC != 0) {
      steps_print_line(STEPS_LAST_ITR);
      print_term(heap_read(STEPS_ROOT_LOC));
      printf("\n");
    }
  }

  return cur;
}

// Runtime
// =======

// Runtime Session Init
// --------------------
// Initializes process-global state for one program execution.

// Initializes runtime globals and evaluator flags.
fn void runtime_init(int debug, int silent, int steps_enable) {
  BOOK       = calloc(BOOK_CAP, sizeof(u64));
  HEAP       = NULL;
  TABLE.data = calloc(BOOK_CAP, sizeof(char *));

  if (HEAP_CAP <= ((u64)SIZE_MAX / sizeof(Term))) {
    size_t heap_bytes = (size_t)(HEAP_CAP * sizeof(Term));
    void  *heap_map   = sys_mmap_anon(heap_bytes);
    if (heap_map != NULL) {
      HEAP = (Term *)heap_map;
    }
  }

  if (!BOOK || !HEAP || !TABLE.data) {
    sys_error("Memory allocation failed");
  }

  HEAP_NEXT = 1;
  memset(HEAP_FREE, 0, sizeof(HEAP_FREE));
  symbols_init();
  DEBUG        = debug;
  SILENT       = silent;
  STEPS_ENABLE = steps_enable;
  ITRS         = 0;
}

// Runtime Session Free
// --------------------
// Releases process-global state for one program execution.

// Frees runtime-global allocations for the current process run.
fn void runtime_free(void) {
  if (HEAP != NULL) {
    size_t heap_bytes = (size_t)(HEAP_CAP * sizeof(Term));
    sys_munmap_anon(HEAP, heap_bytes);
    HEAP = NULL;
  }
  free(BOOK);
  free(TABLE.data);
  wnf_stack_free();
}

// Runtime Entry Lookup
// --------------------
// Resolves a named top-level entrypoint to a BOOK id.

// Resolves one top-level entry id by name; returns 1 when defined.
fn int runtime_entry(const char *name, u32 *out_id) {
  if (name == NULL || out_id == NULL) {
    return 0;
  }

  u32 len = (u32)strlen(name);
  u32 id  = table_find(name, len);
  if (BOOK[id] == 0) {
    return 0;
  }

  *out_id = id;
  return 1;
}

// Runtime Program Prepare
// =======================
// Parses one source buffer, validates static-space limits, and resolves @main.

// Forward declarations
// --------------------

fn void parse_program(const char *source_path, char *src);
fn int  runtime_entry(const char *name, u32 *out_id);
static void compile_program_terms(void);
static void neo_disable(void);
static int  nv_prepare_main(u32 main_id);

// Runtime Prepare
// ---------------

// Parses and validates one source buffer, returning @main id on success.
fn int runtime_prepare(u32 *main_id, const char *src_path, char *src) {
  if (main_id == NULL || src == NULL) {
    return 0;
  }

  parse_program(src_path, src);

  if (!runtime_entry("main", main_id)) {
    fprintf(stderr, "Error: @main not defined\n");
    return 0;
  }

  if (HEAP_NEXT < 50000) {
    compile_program_terms();
  } else {
    neo_disable();
  }

  nv_prepare_main(*main_id);

  return 1;
}

// Neo Eval
// ========
// General value-code evaluator for pure HVM programs.

#define NEO_INLINE static inline __attribute__((always_inline))
#define NEO_MAX_ARGS 16
#define NEO_AUTO_LAB_BASE 0x800000u
#define NEO_VAL_FROZEN UINT16_MAX
#define NEO_VAL_BORROWED 0x8000u
#define NEO_VAL_REFS_MASK 0x7FFFu

typedef struct Env Env;
typedef struct Val Val;
typedef struct Code Code;
typedef struct Arg Arg;
typedef struct Cases Cases;
typedef struct Case Case;
typedef struct LamDup LamDup;

enum {
  V_THUNK,
  V_LTHUNK,
  V_NUM,
  V_CTR,
  V_LAM,
  V_ELAM,
  V_SEL,
  V_PSEL,
  V_CALL2,
  V_MAT,
  V_SUP,
  V_VAR,
  V_APP,
  V_PRJ,
  V_DLAM,
  V_PLAM,
  V_BOX,
  V_ERA
};

enum {
  BC_ARG,
  BC_ARGS,
  BC_VAR,
  BC_DP0,
  BC_DP1,
  BC_REF,
  BC_CALL_REF,
  BC_NUM,
  BC_CTR,
  BC_LAM,
  BC_ELAM,
  BC_MAT,
  BC_MAT_CTR,
  BC_DUP,
  BC_SUP,
  BC_ERA,
  BC_BAD
};

enum {
  CK_NONE,
  CK_NUM,
  CK_VAR
};

struct Env {
  Val      *val;
  uintptr_t next_span;
};

typedef struct EnvBlock EnvBlock;
typedef struct ValBlock ValBlock;
typedef struct ItemBlock ItemBlock;
typedef struct CodeBlock CodeBlock;
typedef struct LamDupBlock LamDupBlock;

struct Val {
  u8  tag;
  u8  arity;
  u16 pad;
  u32 ext;
  union {
    Val **item;
    struct {
      Val *fst;
      Val *snd;
    };
    struct {
      Code *code;
      Env  *env;
    };
  };
};

struct Code {
  u8    op;
  u8    aux;
  u8    sup_has;
  u8    pad;
  u32   ext;
  u32   arity;
  u32   sup_lab;
  u32   gid;
  Term  term;
  Cases *cases;
  Code *sub;
  Code *next;
  Code **kid;
  void *jump;
};

struct Case {
  u8    tag;
  u32   ext;
  Code *body;
  Case *next;
};

struct Cases {
  Case *head;
  Code *ctr;
  Code *num0;
  Code *num1;
  Code *dft;
  u32   ctr_ext;
};

struct Arg {
  Val  *val;
  Code *code;
  Env  *env;
  u32   gap;
};

struct LamDup {
  Val *lam;
  Val *box[2];
  Val *proj[2];
  u32  lab;
};

struct EnvBlock {
  EnvBlock *next;
  u32       used;
  Env       item[65536];
};

struct ValBlock {
  ValBlock *next;
  u32       used;
  Val       item[65536];
};

struct ItemBlock {
  ItemBlock *next;
  u32        used;
  Val       *item[1 << 20];
};

struct CodeBlock {
  CodeBlock *next;
  u32        used;
  Code       item[65536];
};

struct LamDupBlock {
  LamDupBlock *next;
  u32          used;
  LamDup       item[65536];
};

static EnvBlock    *NEO_ENV_BLOCK = NULL;
static ValBlock    *NEO_VAL_BLOCK = NULL;
static ItemBlock   *NEO_ITEM_BLOCK = NULL;
static CodeBlock   *NEO_CODE_BLOCK = NULL;
static LamDupBlock *NEO_LAMDUP_BLOCK = NULL;
static Val         *NEO_VAL_FREE = NULL;
static Code       **NEO_DEFS = NULL;
static Val        **NEO_REF_CACHE = NULL;
static Code       **GRAPH_CODE_INDEX = NULL;
static u32          NEO_DEF_CAP = 0;
static u32          GRAPH_CODE_LEN = 0;
static u32          GRAPH_CODE_CAP = 0;
static u64          NEO_ITRS = 0;
static int          NEO_FAILED = 0;
static int          NEO_THREADED = 0;
static Val          NEO_NUM_CACHE[2] = {
  {.tag = V_NUM, .ext = 0, .arity = 0, .env = NULL, .code = NULL},
  {.tag = V_NUM, .ext = 1, .arity = 0, .env = NULL, .code = NULL},
};

static void neo_disable(void) {
  NEO_FAILED = 1;
}

static void neo_fail(void) {
  NEO_FAILED = 1;
}

static void neo_die(const char *msg) {
  fprintf(stderr, "RUNTIME_ERROR: %s\n", msg);
  exit(1);
}

NEO_INLINE void neo_itr(u64 n) {
  NEO_ITRS += n;
}

NEO_INLINE Val *val_new(u8 tag) {
  Val *v;
  if (__builtin_expect(NEO_VAL_FREE != NULL, 1)) {
    v = NEO_VAL_FREE;
    NEO_VAL_FREE = NEO_VAL_FREE->fst;
  } else if (__builtin_expect(NEO_VAL_BLOCK == NULL || NEO_VAL_BLOCK->used >= 65536, 0)) {
    ValBlock *block = (ValBlock*)malloc(sizeof(ValBlock));
    if (block == NULL) neo_die("out of memory");
    block->next = NEO_VAL_BLOCK;
    block->used = 0;
    NEO_VAL_BLOCK = block;
    v = &NEO_VAL_BLOCK->item[NEO_VAL_BLOCK->used++];
  } else {
    v = &NEO_VAL_BLOCK->item[NEO_VAL_BLOCK->used++];
  }
  v->tag = tag;
  return v;
}

NEO_INLINE void val_free(Val *v) {
  v->fst = NEO_VAL_FREE;
  NEO_VAL_FREE = v;
}

NEO_INLINE Val *share_value(Val *v);

NEO_INLINE void val_free_ctr(Val *v) {
  if (v->tag != V_CTR) return;
  if (v->pad == NEO_VAL_FROZEN) return;
  u16 refs = v->pad & NEO_VAL_REFS_MASK;
  if (__builtin_expect(refs == 0, 1)) {
    val_free(v);
    return;
  }
  v->pad = (v->pad & NEO_VAL_BORROWED) | (refs - 1);
  if (v->arity == 2) {
    share_value(v->fst);
    share_value(v->snd);
  } else if (v->arity == 1) {
    share_value(v->fst);
  } else if (v->arity > 2) {
    for (u32 i = 0; i < v->arity; i++) {
      share_value(v->item[i]);
    }
  }
  v->pad = 0;
}

NEO_INLINE Val *share_value(Val *v) {
  if (v->tag == V_CTR || v->tag == V_LTHUNK || v->tag == V_THUNK) {
    if (v->pad != NEO_VAL_FROZEN) {
      u16 refs = v->pad & NEO_VAL_REFS_MASK;
      if (refs < NEO_VAL_REFS_MASK) {
        v->pad = (v->pad & NEO_VAL_BORROWED) | (refs + 1);
      }
    }
  }
  return v;
}

NEO_INLINE int is_lam(Val *v) {
  return v->tag == V_LAM || v->tag == V_ELAM || v->tag == V_SEL || v->tag == V_PSEL;
}

static Code *code_new(u8 op) {
  if (NEO_CODE_BLOCK == NULL || NEO_CODE_BLOCK->used >= 65536) {
    CodeBlock *block = (CodeBlock*)malloc(sizeof(CodeBlock));
    if (block == NULL) neo_die("out of memory");
    block->next = NEO_CODE_BLOCK;
    block->used = 0;
    NEO_CODE_BLOCK = block;
  }
  Code *c = &NEO_CODE_BLOCK->item[NEO_CODE_BLOCK->used++];
  memset(c, 0, sizeof(Code));
  c->op = op;
  c->sup_lab = UINT32_MAX;
  if (__builtin_expect(GRAPH_CODE_LEN == GRAPH_CODE_CAP, 0)) {
    u32 next_cap = GRAPH_CODE_CAP == 0 ? 65536 : GRAPH_CODE_CAP * 2;
    Code **next = (Code**)realloc(GRAPH_CODE_INDEX, (size_t)next_cap * sizeof(Code*));
    if (next == NULL) neo_die("out of memory");
    GRAPH_CODE_INDEX = next;
    GRAPH_CODE_CAP = next_cap;
  }
  c->gid = GRAPH_CODE_LEN;
  GRAPH_CODE_INDEX[GRAPH_CODE_LEN++] = c;
  return c;
}

static Code **code_kids_new(u32 len) {
  if (len == 0) return NULL;
  Code **kids = (Code**)calloc(len, sizeof(Code*));
  if (kids == NULL) neo_die("out of memory");
  return kids;
}

static Cases *cases_new(void) {
  Cases *cases = (Cases*)calloc(1, sizeof(Cases));
  if (cases == NULL) neo_die("out of memory");
  cases->ctr_ext = UINT32_MAX;
  return cases;
}

static Case *case_new(u8 tag, u32 ext, Code *body) {
  Case *c = (Case*)malloc(sizeof(Case));
  if (c == NULL) neo_die("out of memory");
  c->tag = tag;
  c->ext = ext;
  c->body = body;
  c->next = NULL;
  return c;
}

static Val **items_new(u32 len) {
  if (len == 0) return NULL;
  if (len > (1u << 20)) neo_die("constructor too wide");
  if (NEO_ITEM_BLOCK == NULL || NEO_ITEM_BLOCK->used + len > (1u << 20)) {
    ItemBlock *block = (ItemBlock*)malloc(sizeof(ItemBlock));
    if (block == NULL) neo_die("out of memory");
    block->next = NEO_ITEM_BLOCK;
    block->used = 0;
    NEO_ITEM_BLOCK = block;
  }
  Val **items = &NEO_ITEM_BLOCK->item[NEO_ITEM_BLOCK->used];
  NEO_ITEM_BLOCK->used += len;
  return items;
}

static LamDup *lamdup_new(void) {
  if (NEO_LAMDUP_BLOCK == NULL || NEO_LAMDUP_BLOCK->used >= 65536) {
    LamDupBlock *block = (LamDupBlock*)malloc(sizeof(LamDupBlock));
    if (block == NULL) neo_die("out of memory");
    block->next = NEO_LAMDUP_BLOCK;
    block->used = 0;
    NEO_LAMDUP_BLOCK = block;
  }
  return &NEO_LAMDUP_BLOCK->item[NEO_LAMDUP_BLOCK->used++];
}

NEO_INLINE Env *env_cell(Val *val, Env *next, u32 span) {
  if (__builtin_expect(NEO_ENV_BLOCK == NULL || NEO_ENV_BLOCK->used >= 65536, 0)) {
    EnvBlock *block = (EnvBlock*)malloc(sizeof(EnvBlock));
    if (block == NULL) neo_die("out of memory");
    block->next = NEO_ENV_BLOCK;
    block->used = 0;
    NEO_ENV_BLOCK = block;
  }
  Env *e = &NEO_ENV_BLOCK->item[NEO_ENV_BLOCK->used++];
  e->val = val;
  e->next_span = ((uintptr_t)next) | (uintptr_t)(span - 1);
  return e;
}

NEO_INLINE Env *env_cell1(Val *val, Env *next) {
  if (__builtin_expect(NEO_ENV_BLOCK == NULL || NEO_ENV_BLOCK->used >= 65536, 0)) {
    EnvBlock *block = (EnvBlock*)malloc(sizeof(EnvBlock));
    if (block == NULL) neo_die("out of memory");
    block->next = NEO_ENV_BLOCK;
    block->used = 0;
    NEO_ENV_BLOCK = block;
  }
  Env *e = &NEO_ENV_BLOCK->item[NEO_ENV_BLOCK->used++];
  e->val = val;
  e->next_span = (uintptr_t)next;
  return e;
}

NEO_INLINE Env *env_push(Val *val, Env *next, u32 span) {
  if (span == 0) neo_die("bad environment span");
  if (span <= 16) return env_cell(val, next, span);
  u32 rest = span - 16;
  Env *tail = next;
  while (rest > 0) {
    u32 chunk = rest > 16 ? 16 : rest;
    tail = env_cell(NULL, tail, chunk);
    rest -= chunk;
  }
  return env_cell(val, tail, 16);
}

NEO_INLINE Env *env_push_lam(Val *val, Env *next, u32 gap) {
  if (__builtin_expect(gap == 0, 1)) {
    return env_cell1(val, next);
  }
  return env_push(val, next, gap + 1);
}

NEO_INLINE Env *env_next(Env *env) {
  return (Env*)(env->next_span & ~(uintptr_t)15);
}

NEO_INLINE u32 env_span(Env *env) {
  return (u32)(env->next_span & (uintptr_t)15) + 1;
}

NEO_INLINE Env *env_at(Env *env, u32 lvl, u32 gap) {
  if (lvl == 0) neo_die("bad variable level");
  if (lvl <= gap) neo_die("erased variable reached");
  if (gap == 0 && lvl == 2 && env != NULL && env_span(env) == 1) {
    env = env_next(env);
    if (!env || !env->val) neo_die("unbound variable");
    return env;
  }
  lvl -= gap;
  if (lvl == 1) {
    if (!env || !env->val) neo_die("unbound variable");
    return env;
  }
  for (;;) {
    if (!env) neo_die("unbound variable");
    u32 span = env_span(env);
    if (lvl <= span) neo_die("erased variable reached");
    lvl -= span;
    env = env_next(env);
    if (lvl == 1) {
      if (!env || !env->val) neo_die("unbound variable");
      return env;
    }
  }
}

NEO_INLINE Val *env_get(Env *env, u32 lvl, u32 gap) {
  if (gap == 0) {
    if (lvl == 1) {
      if (!env || !env->val) neo_die("unbound variable");
      return env->val;
    }
    if (lvl == 2 && env != NULL && env_span(env) == 1) {
      env = env_next(env);
      if (!env || !env->val) neo_die("unbound variable");
      return env->val;
    }
    if (lvl == 3 && env != NULL && env_span(env) == 1) {
      env = env_next(env);
      if (env != NULL && env_span(env) == 1) {
        env = env_next(env);
        if (!env || !env->val) neo_die("unbound variable");
        return env->val;
      }
    }
    if (lvl == 4 && env != NULL && env_span(env) == 1) {
      env = env_next(env);
      if (env != NULL && env_span(env) == 1) {
        env = env_next(env);
        if (env != NULL && env_span(env) == 1) {
          env = env_next(env);
          if (!env || !env->val) neo_die("unbound variable");
          return env->val;
        }
      }
    }
  }
  return env_at(env, lvl, gap)->val;
}

static Code *compile_term(Term term);
static Code *compile_term_ctx(Term term, int tail, u32 depth);
static Code *compile_app(Term term, int tail, u32 depth);
static Val *eval_code_into(Code *pc, Env *env, u32 gap, Arg *args, u32 argc, Val *dst);
static void link_refs_code(Code *code, u32 depth);
static void thread_code(Code *code, void **dispatch, u32 depth);
static Val *clone_cached_value(Val *v);
static Val *frozen_field_value(Val *v);
static void freeze_cached_value(Val *v);

static inline Val *eval_code(Code *pc, Env *env, u32 gap, Arg *args, u32 argc) {
  return eval_code_into(pc, env, gap, args, argc, NULL);
}

NEO_INLINE Val *force(Val *v);
NEO_INLINE Val *mk_lthunk(Code *code, Env *env, u32 gap);
NEO_INLINE Val *mk_num(u32 n);
NEO_INLINE Val *mk_lam_tag(u8 tag, Code *code, Env *env, u32 gap);
static inline Val *mk_lam(Code *code, Env *env, u32 gap);
static inline Val *mk_sel(u8 pick);
static inline Val *mk_call2(Val *fst, Val *snd);
static inline Val *mk_mat(Code *code, Env *env, u32 gap);
static inline Val *make_ctr(Code *pc, Env *env, u32 gap);

static Code *compile_term(Term term) {
  return compile_term_ctx(term, 0, 0);
}

static u8 compile_ctr_field_kind(Code *code) {
  if (code->op == BC_NUM) return CK_NUM;
  if (code->op == BC_VAR) return CK_VAR;
  return CK_NONE;
}

static u32 compile_level(u32 depth, Term term) {
  u32 abs = (u32)term_val(term);
  if (abs == 0 || abs > depth) {
    neo_fail();
    return abs;
  }
  return depth - abs + 1;
}

static Code *compile_term_ctx(Term term, int tail, u32 depth) {
  (void)tail;
  u8 tag = term_tag(term);
  Code *c = NULL;
  switch (tag) {
    case APP:
      return compile_app(term, tail, depth);
    case BJV:
      c = code_new(BC_VAR);
      c->ext = compile_level(depth, term);
      break;
    case BJ0:
      c = code_new(BC_DP0);
      c->ext = compile_level(depth, term);
      c->arity = term_ext(term);
      break;
    case BJ1:
      c = code_new(BC_DP1);
      c->ext = compile_level(depth, term);
      c->arity = term_ext(term);
      break;
    case REF:
      c = code_new(BC_REF);
      c->ext = term_ext(term);
      break;
    case NUM:
      c = code_new(BC_NUM);
      c->ext = (u32)term_val(term);
      break;
    case C00 ... C16: {
      c = code_new(BC_CTR);
      c->ext = term_ext(term);
      c->arity = tag - C00;
      c->term = term;
      c->kid = code_kids_new(c->arity);
      u64 loc = term_val(term);
      for (u32 i = 0; i < c->arity; i++) {
        c->kid[i] = compile_term_ctx(heap_read(loc + i), 0, depth);
      }
      c->sub = c->arity > 0 ? c->kid[0] : NULL;
      c->next = c->arity > 1 ? c->kid[1] : NULL;
      if (c->arity == 2) {
        c->aux = compile_ctr_field_kind(c->sub) | (compile_ctr_field_kind(c->next) << 2);
      }
      break;
    }
    case LAM: {
      c = code_new((term_ext(term) & LAM_ERA_MASK) ? BC_ELAM : BC_LAM);
      c->sub = compile_term_ctx(heap_read(term_val(term)), 1, depth + 1);
      break;
    }
    case MAT:
    case SWI: {
      c = code_new(BC_MAT);
      c->term = term;
      c->cases = cases_new();
      Case **tail_case = &c->cases->head;
      Term cur = term;
      while (term_tag(cur) == MAT || term_tag(cur) == SWI) {
        u64 loc = term_val(cur);
        Code *body = compile_term_ctx(heap_read(loc + 0), 1, depth);
        Case *one = case_new(term_tag(cur), term_ext(cur), body);
        *tail_case = one;
        tail_case = &one->next;
        if (term_tag(cur) == SWI && term_ext(cur) == 0) {
          c->cases->num0 = body;
        } else if (term_tag(cur) == SWI && term_ext(cur) == 1) {
          c->cases->num1 = body;
        } else if (term_tag(cur) == MAT && c->cases->ctr == NULL) {
          c->cases->ctr_ext = term_ext(cur);
          c->cases->ctr = body;
        }
        cur = heap_read(loc + 1);
      }
      c->cases->dft = compile_term_ctx(cur, 1, depth);
      Term tail_term = heap_read(term_val(term) + 1);
      if (tag == MAT && c->cases->head != NULL && c->cases->head->next == NULL &&
          term_tag(tail_term) == NUM && term_val(tail_term) == 0) {
        c->op = BC_MAT_CTR;
        c->ext = term_ext(term);
        c->sub = c->cases->head->body;
      }
      break;
    }
    case DUP: {
      c = code_new(BC_DUP);
      c->ext = term_ext(term);
      c->sub = compile_term_ctx(heap_read(term_val(term) + 0), 0, depth);
      c->next = compile_term_ctx(heap_read(term_val(term) + 1), 1, depth + 1);
      break;
    }
    case SUP: {
      c = code_new(BC_SUP);
      c->ext = term_ext(term);
      c->arity = 2;
      c->kid = code_kids_new(2);
      c->kid[0] = compile_term_ctx(heap_read(term_val(term) + 0), 0, depth);
      c->kid[1] = compile_term_ctx(heap_read(term_val(term) + 1), 0, depth);
      break;
    }
    case ERA:
      c = code_new(BC_ERA);
      break;
    default:
      neo_fail();
      c = code_new(BC_BAD);
      break;
  }
  return c;
}

static Code *compile_app(Term term, int tail, u32 depth) {
  (void)tail;
  Term args[NEO_MAX_ARGS];
  u32 argc = 0;
  Term fun = term;
  while (term_tag(fun) == APP && argc < NEO_MAX_ARGS) {
    u64 loc = term_val(fun);
    args[argc++] = heap_read(loc + 1);
    fun = heap_read(loc + 0);
  }
  if (term_tag(fun) == APP) {
    neo_fail();
    return code_new(BC_BAD);
  }
  if (argc == 1) {
    if (term_tag(fun) == REF) {
      Code *c = code_new(BC_CALL_REF);
      c->ext = term_ext(fun);
      c->arity = 1;
      c->sub = compile_term_ctx(args[0], 0, depth);
      return c;
    }
    Code *c = code_new(BC_ARG);
    c->sub = compile_term_ctx(args[0], 0, depth);
    c->next = compile_term_ctx(fun, 0, depth);
    return c;
  }
  Code *c = code_new(BC_ARGS);
  c->arity = argc;
  c->kid = code_kids_new(argc);
  for (u32 i = 0; i < argc; i++) {
    c->kid[i] = compile_term_ctx(args[i], 0, depth);
  }
  c->next = compile_term_ctx(fun, 0, depth);
  return c;
}

static int code_lam2_selector(Code *code, u8 *pick) {
  if (code == NULL || (code->op != BC_LAM && code->op != BC_ELAM)) return 0;
  Code *inner = code->sub;
  if (inner == NULL || (inner->op != BC_LAM && inner->op != BC_ELAM) ||
      inner->sub == NULL || inner->sub->op != BC_VAR) {
    return 0;
  }
  if (inner->sub->ext == 2) {
    *pick = 0;
    return 1;
  }
  if (inner->sub->ext == 1) {
    *pick = 1;
    return 1;
  }
  return 0;
}

static Val *closed_code_value(Code *code) {
  if (code == NULL) return NULL;
  switch (code->op) {
    case BC_REF:
      if (code->ext < NEO_DEF_CAP) return NEO_REF_CACHE[code->ext];
      return NULL;
    case BC_NUM:
      return mk_num(code->ext);
    case BC_CTR:
      return make_ctr(code, NULL, 0);
    case BC_LAM: {
      u8 pick = 0;
      if (code_lam2_selector(code, &pick)) return mk_sel(pick);
      return mk_lam_tag(V_LAM, code->sub, NULL, 0);
    }
    case BC_ELAM:
      return mk_lam_tag(V_ELAM, code->sub, NULL, 0);
    case BC_MAT:
    case BC_MAT_CTR:
      return mk_mat(code, NULL, 0);
    default:
      return mk_lthunk(code, NULL, 0);
  }
}

static int code_lam_call2(Code *code, Val **fst, Val **snd) {
  if (code == NULL || code->op != BC_LAM || code->sub == NULL || code->sub->op != BC_ARGS) {
    return 0;
  }
  Code *body = code->sub;
  if (body->arity != 2 || body->next == NULL || body->next->op != BC_VAR || body->next->ext != 1) {
    return 0;
  }
  Val *a = closed_code_value(body->kid[1]);
  Val *b = closed_code_value(body->kid[0]);
  if (a == NULL || b == NULL) return 0;
  *fst = a;
  *snd = b;
  return 1;
}

static void compile_program_terms(void) {
  NEO_DEF_CAP = TABLE.len;
  NEO_DEFS = (Code**)calloc(NEO_DEF_CAP == 0 ? 1 : NEO_DEF_CAP, sizeof(Code*));
  NEO_REF_CACHE = (Val**)calloc(NEO_DEF_CAP == 0 ? 1 : NEO_DEF_CAP, sizeof(Val*));
  if (NEO_DEFS == NULL || NEO_REF_CACHE == NULL) neo_die("out of memory");
  for (u32 i = 0; i < NEO_DEF_CAP; i++) {
    if (BOOK[i] != 0) {
      NEO_DEFS[i] = compile_term_ctx(heap_read(BOOK[i]), 1, 0);
    }
  }
  for (u32 i = 0; i < NEO_DEF_CAP; i++) {
    if (NEO_DEFS[i] != NULL) {
      link_refs_code(NEO_DEFS[i], 0);
    }
  }
  for (u32 i = 0; i < NEO_DEF_CAP; i++) {
    if (NEO_DEFS[i] == NULL) continue;
    u8 op = NEO_DEFS[i]->op;
    if (op == BC_MAT || op == BC_MAT_CTR) {
      NEO_REF_CACHE[i] = mk_mat(NEO_DEFS[i], NULL, 0);
    } else if (op == BC_CTR) {
      NEO_REF_CACHE[i] = make_ctr(NEO_DEFS[i], NULL, 0);
    } else if (op == BC_LAM || op == BC_ELAM) {
      u8 pick = 0;
      if (code_lam2_selector(NEO_DEFS[i], &pick)) {
        NEO_REF_CACHE[i] = mk_sel(pick);
      } else {
        u8 tag = op == BC_ELAM ? V_ELAM : V_LAM;
        NEO_REF_CACHE[i] = mk_lam_tag(tag, NEO_DEFS[i]->sub, NULL, 0);
      }
    }
  }
  for (u32 i = 0; i < NEO_DEF_CAP; i++) {
    Val *fst = NULL;
    Val *snd = NULL;
    if (code_lam_call2(NEO_DEFS[i], &fst, &snd)) {
      NEO_REF_CACHE[i] = mk_call2(fst, snd);
    }
  }
}

static void link_refs_code(Code *code, u32 depth) {
  if (code == NULL || depth > 256) return;
  switch (code->op) {
    case BC_ARGS:
      link_refs_code(code->next, depth + 1);
      for (u32 i = 0; i < code->arity; i++) {
        link_refs_code(code->kid[i], depth + 1);
      }
      return;
    case BC_ARG:
    case BC_DUP:
      link_refs_code(code->sub, depth + 1);
      link_refs_code(code->next, depth + 1);
      return;
    case BC_CALL_REF:
      link_refs_code(code->sub, depth + 1);
      code->next = code->ext < NEO_DEF_CAP ? NEO_DEFS[code->ext] : NULL;
      return;
    case BC_CTR:
      for (u32 i = 0; i < code->arity; i++) {
        link_refs_code(code->kid[i], depth + 1);
      }
      return;
    case BC_LAM:
    case BC_ELAM:
      link_refs_code(code->sub, depth + 1);
      return;
    case BC_MAT:
    case BC_MAT_CTR:
      for (Case *it = code->cases ? code->cases->head : NULL; it != NULL; it = it->next) {
        link_refs_code(it->body, depth + 1);
      }
      if (code->cases != NULL) {
        link_refs_code(code->cases->dft, depth + 1);
      }
      return;
    case BC_REF:
      code->sub = code->ext < NEO_DEF_CAP ? NEO_DEFS[code->ext] : NULL;
      return;
    case BC_SUP:
      link_refs_code(code->kid[0], depth + 1);
      link_refs_code(code->kid[1], depth + 1);
      return;
    default:
      return;
  }
}

static void thread_code(Code *code, void **dispatch, u32 depth) {
  if (code == NULL || depth > 256 || code->jump != NULL) return;
  code->jump = dispatch[code->op];
  switch (code->op) {
    case BC_ARGS:
      thread_code(code->next, dispatch, depth + 1);
      for (u32 i = 0; i < code->arity; i++) {
        thread_code(code->kid[i], dispatch, depth + 1);
      }
      return;
    case BC_ARG:
    case BC_DUP:
    case BC_CALL_REF:
      thread_code(code->sub, dispatch, depth + 1);
      thread_code(code->next, dispatch, depth + 1);
      return;
    case BC_CTR:
      for (u32 i = 0; i < code->arity; i++) {
        thread_code(code->kid[i], dispatch, depth + 1);
      }
      return;
    case BC_LAM:
    case BC_ELAM:
      thread_code(code->sub, dispatch, depth + 1);
      return;
    case BC_MAT:
    case BC_MAT_CTR:
      for (Case *it = code->cases ? code->cases->head : NULL; it != NULL; it = it->next) {
        thread_code(it->body, dispatch, depth + 1);
      }
      if (code->cases != NULL) {
        thread_code(code->cases->dft, dispatch, depth + 1);
      }
      return;
    case BC_SUP:
      thread_code(code->kid[0], dispatch, depth + 1);
      thread_code(code->kid[1], dispatch, depth + 1);
      return;
    default:
      return;
  }
}

static int code_has_sup_label(Code *code, u32 lab, u32 depth) {
  if (code == NULL || depth > 256) return 0;
  if (code->sup_lab == lab) return code->sup_has;
  int has = 0;
  switch (code->op) {
    case BC_ARGS:
      has = code_has_sup_label(code->next, lab, depth + 1);
      for (u32 i = 0; !has && i < code->arity; i++) {
        has = code_has_sup_label(code->kid[i], lab, depth + 1);
      }
      break;
    case BC_ARG:
    case BC_DUP:
      has = code_has_sup_label(code->sub, lab, depth + 1)
         || code_has_sup_label(code->next, lab, depth + 1);
      break;
    case BC_CTR:
      for (u32 i = 0; i < code->arity; i++) {
        if (code_has_sup_label(code->kid[i], lab, depth + 1)) {
          has = 1;
          break;
        }
      }
      break;
    case BC_LAM:
    case BC_ELAM:
      has = code_has_sup_label(code->sub, lab, depth + 1);
      break;
    case BC_MAT:
    case BC_MAT_CTR:
      for (Case *it = code->cases ? code->cases->head : NULL; !has && it != NULL; it = it->next) {
        has = code_has_sup_label(it->body, lab, depth + 1);
      }
      if (!has && code->cases != NULL) {
        has = code_has_sup_label(code->cases->dft, lab, depth + 1);
      }
      break;
    case BC_REF:
      has = code->sub != NULL ? code_has_sup_label(code->sub, lab, depth + 1) : 0;
      break;
    case BC_CALL_REF:
      has = code_has_sup_label(code->sub, lab, depth + 1)
         || (code->next != NULL ? code_has_sup_label(code->next, lab, depth + 1) : 0);
      break;
    case BC_SUP:
      has = code->ext == lab
         || code_has_sup_label(code->kid[0], lab, depth + 1)
         || code_has_sup_label(code->kid[1], lab, depth + 1);
      break;
    default:
      break;
  }
  code->sup_lab = lab;
  code->sup_has = (u8)has;
  return has;
}

typedef struct GraphEnv GraphEnv;

struct __attribute__((aligned(16))) GraphEnv {
  u64       loc;
  u32       id;
  uintptr_t next_span;
};

typedef struct GraphEnvBlock GraphEnvBlock;

struct GraphEnvBlock {
  GraphEnvBlock *next;
  u32            used;
  GraphEnv       item[65536];
};

static GraphEnvBlock *GRAPH_ENV_BLOCK = NULL;
static GraphEnv     **GRAPH_ENV_INDEX = NULL;
static u32            GRAPH_ENV_LEN = 1;
static u32            GRAPH_ENV_CAP = 0;
static int            GRAPH_EVAL_ENABLED = 0;

NEO_INLINE GraphEnv *graph_env_cell(u64 loc, GraphEnv *next, u32 span) {
  if (__builtin_expect(GRAPH_ENV_BLOCK == NULL || GRAPH_ENV_BLOCK->used >= 65536, 0)) {
    GraphEnvBlock *block = (GraphEnvBlock*)malloc(sizeof(GraphEnvBlock));
    if (block == NULL) neo_die("out of memory");
    block->next = GRAPH_ENV_BLOCK;
    block->used = 0;
    GRAPH_ENV_BLOCK = block;
  }
  GraphEnv *e = &GRAPH_ENV_BLOCK->item[GRAPH_ENV_BLOCK->used++];
  e->loc = loc;
  e->next_span = ((uintptr_t)next) | (uintptr_t)(span - 1);
  if (__builtin_expect(GRAPH_ENV_LEN >= GRAPH_ENV_CAP, 0)) {
    u32 next_cap = GRAPH_ENV_CAP == 0 ? 65536 : GRAPH_ENV_CAP * 2;
    GraphEnv **idx = (GraphEnv**)realloc(GRAPH_ENV_INDEX, (size_t)next_cap * sizeof(GraphEnv*));
    if (idx == NULL) neo_die("out of memory");
    GRAPH_ENV_INDEX = idx;
    GRAPH_ENV_CAP = next_cap;
  }
  e->id = GRAPH_ENV_LEN;
  GRAPH_ENV_INDEX[GRAPH_ENV_LEN++] = e;
  return e;
}

NEO_INLINE GraphEnv *graph_env_push(u64 loc, GraphEnv *next, u32 span) {
  if (span == 0) neo_die("bad graph environment span");
  if (span <= 16) return graph_env_cell(loc, next, span);
  u32 rest = span - 16;
  GraphEnv *tail = next;
  while (rest > 0) {
    u32 chunk = rest > 16 ? 16 : rest;
    tail = graph_env_cell(0, tail, chunk);
    rest -= chunk;
  }
  return graph_env_cell(loc, tail, 16);
}

NEO_INLINE GraphEnv *graph_env_next(GraphEnv *env) {
  return (GraphEnv*)(env->next_span & ~(uintptr_t)15);
}

NEO_INLINE u32 graph_env_span(GraphEnv *env) {
  return (u32)(env->next_span & (uintptr_t)15) + 1;
}

NEO_INLINE GraphEnv *graph_env_at(GraphEnv *env, u32 lvl, u32 gap) {
  if (lvl == 0) neo_die("bad graph variable level");
  if (lvl <= gap) neo_die("erased graph variable reached");
  lvl -= gap;
  if (lvl == 1) {
    if (!env || !env->loc) neo_die("unbound graph variable");
    return env;
  }
  for (;;) {
    if (!env) neo_die("unbound graph variable");
    u32 span = graph_env_span(env);
    if (lvl <= span) neo_die("erased graph variable reached");
    lvl -= span;
    env = graph_env_next(env);
    if (lvl == 1) {
      if (!env || !env->loc) neo_die("unbound graph variable");
      return env;
    }
  }
}

NEO_INLINE u64 graph_env_get(GraphEnv *env, u32 lvl, u32 gap) {
  return graph_env_at(env, lvl, gap)->loc;
}

static Term graph_build(Code *code, GraphEnv *env, u32 gap);

static Term graph_closure(Code *code, GraphEnv *env, u32 gap) {
  if (code == NULL) return term_new_era();
  if (__builtin_expect(code->gid > 0xFFFFu || gap > 0xFFu, 0)) {
    neo_fail();
    return term_new_era();
  }
  u32 ext = ((gap & 0xFFu) << 16) | (code->gid & 0xFFFFu);
  u32 val = env == NULL ? 0 : env->id;
  return term_new(0, CLO, ext, val);
}

fn Term graph_ref(u32 nam) {
  if (!GRAPH_EVAL_ENABLED || NEO_FAILED || nam >= NEO_DEF_CAP || NEO_DEFS == NULL || NEO_DEFS[nam] == NULL) {
    return 0;
  }
  return graph_closure(NEO_DEFS[nam], NULL, 0);
}

static Term graph_build_mat(Code *code, GraphEnv *env, u32 gap) {
  Term tail = term_new_num(0);
  if (code->cases != NULL && code->cases->dft != NULL) {
    tail = graph_closure(code->cases->dft, env, gap);
  }
  Case *cases[1024];
  u32 len = 0;
  for (Case *it = code->cases ? code->cases->head : NULL; it != NULL && len < 1024; it = it->next) {
    cases[len++] = it;
  }
  while (len > 0) {
    Case *it = cases[--len];
    Term body = graph_closure(it->body, env, gap);
    tail = it->tag == SWI ? term_new_swi(it->ext, body, tail) : term_new_mat(it->ext, body, tail);
  }
  return tail;
}

static Term graph_build_app(Code *fun, Code **kids, u32 argc, GraphEnv *env, u32 gap) {
  Term term = graph_closure(fun, env, gap);
  for (u32 i = argc; i > 0; i--) {
    term = term_new_app(term, graph_closure(kids[i - 1], env, gap));
  }
  return term;
}

static Term graph_build(Code *code, GraphEnv *env, u32 gap) {
  switch (code->op) {
    case BC_VAR:
      return term_new_var(graph_env_get(env, code->ext, gap));
    case BC_DP0:
      return term_new(0, DP0, code->arity, graph_env_get(env, code->ext, gap));
    case BC_DP1:
      return term_new(0, DP1, code->arity, graph_env_get(env, code->ext, gap));
    case BC_REF:
      return graph_ref(code->ext) ?: term_new_ref(code->ext);
    case BC_CALL_REF: {
      Term fun = graph_ref(code->ext);
      if (fun == 0) fun = term_new_ref(code->ext);
      return term_new_app(fun, graph_closure(code->sub, env, gap));
    }
    case BC_NUM:
      return term_new_num(code->ext);
    case BC_CTR: {
      Term args[16];
      for (u32 i = 0; i < code->arity; i++) {
        args[i] = graph_closure(code->kid[i], env, gap);
      }
      return term_new_ctr(code->ext, code->arity, args);
    }
    case BC_LAM: {
      u64 loc = heap_alloc(1);
      GraphEnv *body_env = graph_env_push(loc, env, gap + 1);
      heap_set(loc, graph_closure(code->sub, body_env, 0));
      return term_new(0, LAM, 0, loc);
    }
    case BC_ELAM: {
      u64 loc = heap_alloc(1);
      heap_set(loc, graph_closure(code->sub, env, gap + 1));
      return term_new(0, LAM, LAM_ERA_MASK, loc);
    }
    case BC_MAT:
    case BC_MAT_CTR:
      return graph_build_mat(code, env, gap);
    case BC_DUP: {
      u64 loc = heap_alloc(2);
      heap_set(loc + 0, graph_closure(code->sub, env, gap));
      GraphEnv *body_env = graph_env_push(loc, env, gap + 1);
      heap_set(loc + 1, graph_closure(code->next, body_env, 0));
      return term_new(0, DUP, code->ext, loc);
    }
    case BC_SUP: {
      return term_new_sup(code->ext, graph_closure(code->kid[0], env, gap), graph_closure(code->kid[1], env, gap));
    }
    case BC_ERA:
      return term_new_era();
    case BC_ARG:
      return graph_build_app(code->next, &code->sub, 1, env, gap);
    case BC_ARGS:
      return graph_build_app(code->next, code->kid, code->arity, env, gap);
    default:
      neo_fail();
      return term_new_era();
  }
}

fn Term graph_expand(Term clo) {
  u32 ext = term_ext(clo);
  u32 gid = ext & 0xFFFFu;
  u32 gap = ext >> 16;
  u32 eid = term_val(clo);
  Code *code = gid < GRAPH_CODE_LEN ? GRAPH_CODE_INDEX[gid] : NULL;
  GraphEnv *env = eid == 0 ? NULL : (eid < GRAPH_ENV_LEN ? GRAPH_ENV_INDEX[eid] : NULL);
  if (code == NULL) return term_new_era();
  return graph_build(code, env, gap);
}

NEO_INLINE Val *mk_thunk(Code *code, Env *env, u32 gap) {
  Val *v = val_new(V_THUNK);
  v->code = code;
  v->env = env;
  v->ext = gap;
  v->pad = 0;
  return v;
}

NEO_INLINE Val *mk_lthunk(Code *code, Env *env, u32 gap) {
  Val *v = val_new(V_LTHUNK);
  v->code = code;
  v->env = env;
  v->ext = gap;
  v->pad = 0;
  return v;
}

NEO_INLINE Val *mk_num(u32 n) {
  if (n < 2) return &NEO_NUM_CACHE[n];
  Val *v = val_new(V_NUM);
  v->ext = n;
  return v;
}

NEO_INLINE Val *mk_lam_tag(u8 tag, Code *code, Env *env, u32 gap) {
  Val *v = val_new(tag);
  v->code = code;
  v->env = env;
  v->ext = gap;
  v->arity = 0;
  v->pad = 0;
  return v;
}

static inline Val *mk_lam(Code *code, Env *env, u32 gap) {
  return mk_lam_tag(V_LAM, code, env, gap);
}

static inline Val *mk_sel(u8 pick) {
  Val *v = val_new(V_SEL);
  v->ext = pick;
  v->arity = 0;
  v->pad = 0;
  return v;
}

static inline Val *mk_psel(u8 pick, Val *arg) {
  Val *v = val_new(V_PSEL);
  v->ext = pick;
  v->arity = 0;
  v->pad = 0;
  v->fst = arg;
  return v;
}

static inline Val *mk_call2(Val *fst, Val *snd) {
  Val *v = val_new(V_CALL2);
  v->ext = 0;
  v->arity = 0;
  v->pad = 0;
  v->fst = fst;
  v->snd = snd;
  return v;
}

static inline Val *mk_mat(Code *code, Env *env, u32 gap) {
  Val *v = val_new(V_MAT);
  v->code = code;
  v->env = env;
  v->ext = gap;
  return v;
}

NEO_INLINE Val *mk_sup(u32 lab, Val *a, Val *b) {
  Val *v = val_new(V_SUP);
  v->ext = lab;
  v->fst = a;
  v->snd = b;
  return v;
}

static inline Val *mk_var(u32 idx) {
  Val *v = val_new(V_VAR);
  v->ext = idx;
  return v;
}

static inline Val *mk_app(Val *fun, Val *arg) {
  Val *v = val_new(V_APP);
  v->fst = fun;
  v->snd = arg;
  return v;
}

static inline Val *mk_prj(Val *fun, u32 lab, u8 side) {
  Val *v = val_new(V_PRJ);
  v->ext = lab;
  v->arity = side;
  v->fst = fun;
  return v;
}

static inline Val *mk_box(void) {
  Val *v = val_new(V_BOX);
  v->fst = NULL;
  return v;
}

static Val *mk_dlam(Val *lam, u32 lab) {
  LamDup *dup = lamdup_new();
  dup->lam = lam;
  dup->lab = lab;
  dup->box[0] = mk_box();
  dup->box[1] = mk_box();
  dup->proj[0] = NULL;
  dup->proj[1] = NULL;
  Val *v = val_new(V_DLAM);
  v->ext = lab;
  v->fst = lam;
  v->snd = (Val*)dup;
  return v;
}

static inline Val *mk_plam(Val *lam, u32 lab, u8 side) {
  Val *v = val_new(V_PLAM);
  v->ext = lab;
  v->arity = side;
  v->fst = lam;
  return v;
}

NEO_INLINE Val *force_fun_arg(Val *v) {
  if (v->tag != V_THUNK && v->tag != V_LTHUNK) return v;
  switch (v->code->op) {
    case BC_VAR:
    case BC_DP0:
    case BC_DP1:
    case BC_LAM:
    case BC_ELAM:
    case BC_MAT:
    case BC_MAT_CTR:
    case BC_REF:
    case BC_CALL_REF:
    case BC_ARG:
    case BC_ARGS:
    case BC_DUP:
      return force(v);
    default:
      return v;
  }
}

NEO_INLINE Arg arg_code(Code *code, Env *env, u32 gap) {
  return (Arg){.val = NULL, .code = code, .env = env, .gap = gap};
}

NEO_INLINE Arg arg_val(Val *val) {
  return (Arg){.val = val, .code = NULL, .env = NULL, .gap = 0};
}

static inline Val *make_ctr(Code *pc, Env *env, u32 gap);

NEO_INLINE Val *force_arg(Arg *arg) {
  if (arg->val != NULL) return force(arg->val);
  Arg none[NEO_MAX_ARGS];
  return eval_code(arg->code, arg->env, arg->gap, none, 0);
}

NEO_INLINE Val *bind_arg(Arg *arg) {
  if (arg->val != NULL) return force_fun_arg(arg->val);
  switch (arg->code->op) {
    case BC_NUM:
      return mk_num(arg->code->ext);
    case BC_VAR:
      return env_get(arg->env, arg->code->ext, arg->gap);
    case BC_CTR:
      return mk_lthunk(arg->code, arg->env, arg->gap);
    case BC_REF:
      if (arg->code->ext < NEO_DEF_CAP && NEO_REF_CACHE[arg->code->ext] != NULL) {
        Val *cached = NEO_REF_CACHE[arg->code->ext];
        return cached->tag == V_CTR ? clone_cached_value(cached) : cached;
      }
      return force_arg(arg);
    case BC_SUP:
    case BC_ERA:
      return mk_lthunk(arg->code, arg->env, arg->gap);
    default:
      return force_arg(arg);
  }
}

NEO_INLINE Val *project(Val *v, u32 lab, u8 side) {
  v = force(v);
  if (v->tag == V_SEL || v->tag == V_PSEL || v->tag == V_CALL2) {
    return v;
  }
  if (v->tag == V_DLAM) {
    neo_itr(1);
    LamDup *dup = (LamDup*)v->snd;
    if (dup->lab == lab) {
      Val *cached = dup->proj[side];
      if (cached != NULL) {
        return cached;
      }
      cached = mk_plam(v, lab, side);
      dup->proj[side] = cached;
      return cached;
    }
    return mk_plam(v, lab, side);
  }
  if (v->tag == V_SUP) {
    neo_itr(1);
    if (v->ext == lab) return side == 0 ? v->fst : v->snd;
    return mk_sup(v->ext, project(v->fst, lab, side), project(v->snd, lab, side));
  }
  if (v->tag == V_CTR) {
    neo_itr(1);
    Val *ctr = val_new(V_CTR);
    ctr->ext = v->ext;
    ctr->arity = v->arity;
    ctr->pad = 0;
    if (v->arity == 2) {
      ctr->fst = project(v->fst, lab, side);
      ctr->snd = project(v->snd, lab, side);
    } else if (v->arity == 1) {
      ctr->fst = project(v->fst, lab, side);
    } else if (v->arity > 2) {
      ctr->item = items_new(v->arity);
      for (u32 i = 0; i < v->arity; i++) {
        ctr->item[i] = project(v->item[i], lab, side);
      }
    }
    return ctr;
  }
  if (v->tag == V_MAT) {
    neo_itr(1);
    return mk_prj(v, lab, side);
  }
  if (is_lam(v)) {
    if (v->code->sup_lab == lab) {
      if (!v->code->sup_has) return v;
      neo_itr(1);
      return mk_plam(mk_dlam(v, lab), lab, side);
    }
    if (code_has_sup_label(v->code, lab, 0)) {
      neo_itr(1);
      return mk_plam(mk_dlam(v, lab), lab, side);
    }
    return v;
  }
  return v;
}

NEO_INLINE int mat_hits(Case *m, Val *arg) {
  return (m->tag == SWI && arg->tag == V_NUM && m->ext == arg->ext)
      || (m->tag == MAT && arg->tag == V_CTR && m->ext == arg->ext);
}

NEO_INLINE Val *ctr_get(Val *ctr, u32 idx) {
  if (idx == 0 && ctr->arity <= 2) return ctr->fst;
  if (idx == 1 && ctr->arity <= 2) return ctr->snd;
  return ctr->item[idx];
}

NEO_INLINE Code *mat_pick(Code *mat, Val *arg) {
  Cases *cases = mat->cases;
  if (arg->tag == V_NUM) {
    if (arg->ext == 0 && cases->num0) return cases->num0;
    if (arg->ext == 1 && cases->num1) return cases->num1;
  } else if (arg->tag == V_CTR && cases->ctr && arg->ext == cases->ctr_ext) {
    return cases->ctr;
  }
  for (Case *m = cases->head; m != NULL; m = m->next) {
    if (mat_hits(m, arg)) return m->body;
  }
  return NULL;
}

NEO_INLINE Code *mat_pick_code(Code *mat, Code *arg) {
  if (arg->op == BC_NUM) {
    if (arg->ext == 0 && mat->cases->num0) return mat->cases->num0;
    if (arg->ext == 1 && mat->cases->num1) return mat->cases->num1;
  } else if (arg->op == BC_CTR) {
    if (mat->cases->ctr && arg->ext == mat->cases->ctr_ext) return mat->cases->ctr;
  } else {
    return NULL;
  }
  for (Case *m = mat->cases->head; m != NULL; m = m->next) {
    if (m->tag == SWI && arg->op == BC_NUM && m->ext == arg->ext) return m->body;
    if (m->tag == MAT && arg->op == BC_CTR && m->ext == arg->ext) return m->body;
  }
  return NULL;
}

NEO_INLINE void push_ctr_code_args(Code *ctr, Env *env, u32 gap, Arg *args, u32 *argc) {
  if (ctr->arity == 2) {
    if (*argc + 2 > NEO_MAX_ARGS) neo_die("argument stack overflow");
    args[(*argc)++] = arg_code(ctr->kid[1], env, gap);
    args[(*argc)++] = arg_code(ctr->kid[0], env, gap);
    return;
  }
  if (ctr->arity == 1) {
    if (*argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
    args[(*argc)++] = arg_code(ctr->kid[0], env, gap);
    return;
  }
  for (u32 i = ctr->arity; i > 0; i--) {
    if (*argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
    args[(*argc)++] = arg_code(ctr->kid[i - 1], env, gap);
  }
}

NEO_INLINE void push_ctr_val_args(Val *ctr, Arg *args, u32 *argc) {
  if (ctr->arity == 2) {
    if (*argc + 2 > NEO_MAX_ARGS) neo_die("argument stack overflow");
    args[(*argc)++] = arg_val(ctr->snd);
    args[(*argc)++] = arg_val(ctr->fst);
    return;
  }
  if (ctr->arity == 1) {
    if (*argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
    args[(*argc)++] = arg_val(ctr->fst);
    return;
  }
  for (u32 i = ctr->arity; i > 0; i--) {
    if (*argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
    args[(*argc)++] = arg_val(ctr_get(ctr, i - 1));
  }
}

NEO_INLINE int is_lam_code(Code *code) {
  return code->op == BC_LAM || code->op == BC_ELAM;
}

NEO_INLINE Code *enter_one_lam(Code *lam, Arg *arg, Env **env, u32 *gap) {
  if (lam->op == BC_ELAM) {
    (*gap)++;
    return lam->sub;
  }
  *env = env_push(bind_arg(arg), *env, *gap + 1);
  *gap = 0;
  return lam->sub;
}

NEO_INLINE int enter_ctr2_code_lams(Code **body, Code *ctr, Env *ctr_env, u32 ctr_gap, Env **env, u32 *gap) {
  if (ctr->arity != 2 || !is_lam_code(*body) || !is_lam_code((*body)->sub)) return 0;
  neo_itr(2);
  Arg fst = arg_code(ctr->kid[0], ctr_env, ctr_gap);
  Code *next = enter_one_lam(*body, &fst, env, gap);
  Arg snd = arg_code(ctr->kid[1], ctr_env, ctr_gap);
  *body = enter_one_lam(next, &snd, env, gap);
  return 1;
}

NEO_INLINE int enter_ctr2_val_lams(Code **body, Val *ctr, Env **env, u32 *gap) {
  if (ctr->arity != 2 || !is_lam_code(*body) || !is_lam_code((*body)->sub)) return 0;
  neo_itr(2);
  Arg fst = arg_val(ctr->fst);
  Code *next = enter_one_lam(*body, &fst, env, gap);
  Arg snd = arg_val(ctr->snd);
  *body = enter_one_lam(next, &snd, env, gap);
  return 1;
}

NEO_INLINE int enter_ctr2_code_num_mat(Code **body, Code *ctr, Env *ctr_env, u32 ctr_gap, Arg *args, u32 *argc) {
  if (ctr->arity != 2 || (*body)->op != BC_MAT || ctr->kid[0]->op != BC_NUM) return 0;
  Code *next = mat_pick_code(*body, ctr->kid[0]);
  if (next == NULL) return 0;
  neo_itr(1);
  if (*argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
  args[(*argc)++] = arg_code(ctr->kid[1], ctr_env, ctr_gap);
  *body = next;
  return 1;
}

NEO_INLINE int enter_ctr2_val_num_mat(Code **body, Val *ctr, Arg *args, u32 *argc) {
  if (ctr->arity != 2 || (*body)->op != BC_MAT || ctr->fst->tag != V_NUM) return 0;
  Code *next = mat_pick(*body, ctr->fst);
  if (next == NULL) return 0;
  neo_itr(1);
  if (*argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
  args[(*argc)++] = arg_val(ctr->snd);
  *body = next;
  return 1;
}

static Val *apply_fun(Val *fun, Arg *arg);
static Val *apply_sup(Val *sup, Arg *arg);

NEO_INLINE Code *matchable_val_code(Val *v, Env **env, u32 *gap) {
  if ((v->tag == V_THUNK || v->tag == V_LTHUNK)
  &&  (v->code->op == BC_CTR || v->code->op == BC_NUM)) {
    *env = v->env;
    *gap = v->ext;
    return v->code;
  }
  return NULL;
}

NEO_INLINE Code *matchable_arg_code(Arg *arg, Env **env, u32 *gap, Val **seen) {
  *seen = NULL;
  if (arg->val != NULL) {
    *seen = arg->val;
    return matchable_val_code(arg->val, env, gap);
  }
  if (arg->code->op == BC_CTR || arg->code->op == BC_NUM) {
    *env = arg->env;
    *gap = arg->gap;
    return arg->code;
  }
  if (arg->code->op == BC_REF && arg->code->ext < NEO_DEF_CAP) {
    Code *def = NEO_DEFS[arg->code->ext];
    if (def != NULL && (def->op == BC_CTR || def->op == BC_NUM)) {
      *env = NULL;
      *gap = 0;
      return def;
    }
  }
  if (arg->code->op == BC_VAR) {
    Val *val = env_get(arg->env, arg->code->ext, arg->gap);
    *seen = val;
    return matchable_val_code(val, env, gap);
  }
  return NULL;
}

NEO_INLINE void consume_matchable_seen(Val *seen) {
  if (seen->tag == V_THUNK || seen->tag == V_LTHUNK) {
    if (seen->pad != 0) {
      seen->pad = 0;
    } else {
      val_free(seen);
    }
  }
}

static Val *apply_default(Code *dft, Env *env, u32 gap, Val *arg) {
  if (dft == NULL) return mk_num(0);
  Arg none[NEO_MAX_ARGS];
  Val *fun = eval_code(dft, env, gap, none, 0);
  switch (fun->tag) {
    case V_LAM:
    case V_ELAM:
    case V_SEL:
    case V_PSEL:
    case V_CALL2:
    case V_MAT:
    case V_SUP:
    case V_VAR:
    case V_APP: {
      Arg one = arg_val(arg);
      return apply_fun(fun, &one);
    }
    default:
      return fun;
  }
}

static inline Val *apply_lam(Val *lam, Arg *arg) {
  neo_itr(1);
  Env *env = lam->env;
  u32 gap = lam->ext;
  if (lam->tag == V_ELAM) {
    gap++;
  } else {
    env = env_push_lam(bind_arg(arg), env, gap);
    gap = 0;
  }
  Arg none[NEO_MAX_ARGS];
  return eval_code(lam->code, env, gap, none, 0);
}

static inline Val *apply_sel(Val *sel, Arg *arg) {
  neo_itr(1);
  if (sel->tag == V_SEL) {
    return mk_psel((u8)sel->ext, sel->ext == 0 ? bind_arg(arg) : NULL);
  }
  Val *got = bind_arg(arg);
  return sel->ext == 0 ? sel->fst : got;
}

static inline Val *apply_call2(Val *call, Arg *arg) {
  Val *fun = bind_arg(arg);
  if (__builtin_expect(fun->tag == V_SEL, 1)) {
    neo_itr(3);
    return fun->ext == 0 ? call->fst : call->snd;
  }
  if (__builtin_expect(fun->tag == V_PSEL, 0)) {
    neo_itr(2);
    return fun->ext == 0 ? fun->fst : call->snd;
  }
  neo_itr(1);
  Arg fst = arg_val(call->fst);
  Val *mid = apply_fun(fun, &fst);
  Arg snd = arg_val(call->snd);
  return apply_fun(mid, &snd);
}

static Val *apply_plam(Val *plam, Arg *arg) {
  neo_itr(1);
  Val *src = force(plam->fst);
  LamDup *dup = NULL;
  Val *lam = src;
  if (__builtin_expect(src->tag == V_DLAM, 1)) {
    dup = (LamDup*)src->snd;
    lam = dup->lam;
  }
  lam = force(lam);
  if (!is_lam(lam)) neo_die("projected non-lambda");
  Env *env = lam->env;
  u32 gap = lam->ext;
  if (lam->tag == V_ELAM) {
    gap++;
  } else {
    Val *got = bind_arg(arg);
    Val *var = dup ? dup->box[1 - plam->arity] : mk_var(0);
    if (dup) dup->box[plam->arity]->fst = got;
    Val *sup = plam->arity == 0 ? mk_sup(plam->ext, got, var) : mk_sup(plam->ext, var, got);
    env = env_push_lam(sup, env, gap);
    gap = 0;
  }
  Arg none[NEO_MAX_ARGS];
  Val *body = eval_code(lam->code, env, gap, none, 0);
  return project(body, plam->ext, plam->arity);
}

static inline Val *apply_mat(Code *mat, Env *env, u32 gap, Val *arg) {
  if (arg->tag == V_SUP) {
    Arg one[1];
    one[0] = arg_val(arg->fst);
    Val *fst = eval_code(mat, env, gap, one, 1);
    one[0] = arg_val(arg->snd);
    Val *snd = eval_code(mat, env, gap, one, 1);
    return mk_sup(arg->ext, fst, snd);
  }
  Code *body = mat_pick(mat, arg);
  if (!body) return apply_default(mat->cases->dft, env, gap, arg);
  Arg args[NEO_MAX_ARGS];
  u32 argc = 0;
  if (arg->tag == V_CTR) {
    if (!enter_ctr2_val_num_mat(&body, arg, args, &argc)
    &&  !enter_ctr2_val_lams(&body, arg, &env, &gap)) {
      push_ctr_val_args(arg, args, &argc);
    }
    val_free_ctr(arg);
  }
  return eval_code(body, env, gap, args, argc);
}

NEO_INLINE Val *apply_fun(Val *fun, Arg *arg) {
  fun = force(fun);
  switch (fun->tag) {
    case V_LAM:
    case V_ELAM:
      return apply_lam(fun, arg);
    case V_SEL:
    case V_PSEL:
      return apply_sel(fun, arg);
    case V_CALL2:
      return apply_call2(fun, arg);
    case V_MAT: {
      Env *arg_env;
      u32 arg_gap;
      Val *seen;
      Code *arg_code = matchable_arg_code(arg, &arg_env, &arg_gap, &seen);
      if (arg_code != NULL) {
        Code *body = mat_pick_code(fun->code, arg_code);
        if (body) {
          neo_itr(1);
          if (seen) consume_matchable_seen(seen);
          Arg args[NEO_MAX_ARGS];
          u32 argc = 0;
          Env *body_env = fun->env;
          u32 body_gap = fun->ext;
          if (arg_code->op == BC_CTR) {
            if (!enter_ctr2_code_num_mat(&body, arg_code, arg_env, arg_gap, args, &argc)
            &&  !enter_ctr2_code_lams(&body, arg_code, arg_env, arg_gap, &body_env, &body_gap)) {
              push_ctr_code_args(arg_code, arg_env, arg_gap, args, &argc);
            }
          }
          return eval_code(body, body_env, body_gap, args, argc);
        }
      }
      neo_itr(1);
      Val *val = seen ? force(seen) : force_arg(arg);
      return apply_mat(fun->code, fun->env, fun->ext, val);
    }
    case V_SUP: {
      Arg shared;
      if (arg->val == NULL) {
        shared = arg_val(mk_thunk(arg->code, arg->env, arg->gap));
        arg = &shared;
      }
      Val *fst = apply_fun(fun->fst, arg);
      Val *snd = apply_fun(fun->snd, arg);
      return mk_sup(fun->ext, fst, snd);
    }
    case V_VAR:
    case V_APP:
      return mk_app(fun, arg->val != NULL ? arg->val : mk_thunk(arg->code, arg->env, arg->gap));
    case V_PRJ: {
      Val *res = apply_fun(fun->fst, arg);
      return project(res, fun->ext, fun->arity);
    }
    case V_PLAM:
      return apply_plam(fun, arg);
    case V_BOX:
      if (fun->fst) return apply_fun(fun->fst, arg);
      return mk_app(fun, arg->val != NULL ? arg->val : mk_thunk(arg->code, arg->env, arg->gap));
    default:
      neo_die("cannot apply value");
  }
  return fun;
}

static __attribute__((noinline)) Val *apply_sup(Val *sup, Arg *arg) {
  if ((sup->fst->tag == V_THUNK || sup->fst->tag == V_LTHUNK)
  &&  (sup->snd->tag == V_THUNK || sup->snd->tag == V_LTHUNK)) {
    if (arg->val == NULL) *arg = arg_val(mk_thunk(arg->code, arg->env, arg->gap));
    Arg one[1] = {*arg};
    Val *fst = eval_code(sup->fst->code, sup->fst->env, sup->fst->ext, one, 1);
    one[0] = *arg;
    Val *snd = eval_code(sup->snd->code, sup->snd->env, sup->snd->ext, one, 1);
    return mk_sup(sup->ext, fst, snd);
  }
  if (is_lam(sup->fst) && is_lam(sup->snd)) {
    if (arg->val == NULL) *arg = arg_val(mk_thunk(arg->code, arg->env, arg->gap));
    Val *fst = apply_lam(sup->fst, arg);
    Val *snd = apply_lam(sup->snd, arg);
    return mk_sup(sup->ext, fst, snd);
  }
  return apply_fun(sup, arg);
}

NEO_INLINE Val *make_field(Code *kid, Env *env, u32 gap);

NEO_INLINE Val *make_atom_field(Code *kid, Env *env, u32 gap, u8 kind) {
  if (kind == CK_NUM) return mk_num(kid->ext);
  if (kind == CK_VAR) return env_get(env, kid->ext, gap);
  return make_field(kid, env, gap);
}

NEO_INLINE void init_ctr(Val *val, Code *pc, Env *env, u32 gap) {
  val->tag = V_CTR;
  val->ext = pc->ext;
  val->arity = pc->arity;
  val->pad = 0;
  if (val->arity == 2) {
    val->fst = make_atom_field(pc->kid[0], env, gap, pc->aux & 3);
    val->snd = make_atom_field(pc->kid[1], env, gap, pc->aux >> 2);
  } else if (val->arity == 1) {
    val->fst = make_field(pc->kid[0], env, gap);
  } else if (val->arity > 2) {
    val->item = items_new(val->arity);
    for (u32 i = 0; i < val->arity; i++) {
      val->item[i] = make_field(pc->kid[i], env, gap);
    }
  }
}

NEO_INLINE Val *make_ctr(Code *pc, Env *env, u32 gap) {
  Val *val = val_new(V_CTR);
  init_ctr(val, pc, env, gap);
  return val;
}

static Val *clone_cached_value(Val *v) {
  switch (v->tag) {
    case V_NUM:
      return v;
    case V_THUNK:
    case V_LTHUNK: {
      Val *out = val_new(v->tag);
      out->code = v->code;
      out->env = v->env;
      out->ext = v->ext;
      out->arity = v->arity;
      out->pad = 0;
      return out;
    }
    case V_CTR: {
      Val *out = val_new(V_CTR);
      out->ext = v->ext;
      out->arity = v->arity;
      out->pad = 0;
      if (v->arity == 2) {
        out->fst = clone_cached_value(v->fst);
        out->snd = clone_cached_value(v->snd);
      } else if (v->arity == 1) {
        out->fst = clone_cached_value(v->fst);
      } else if (v->arity > 2) {
        out->item = items_new(v->arity);
        for (u32 i = 0; i < v->arity; i++) {
          out->item[i] = clone_cached_value(v->item[i]);
        }
      }
      return out;
    }
    case V_SUP:
      return mk_sup(v->ext, clone_cached_value(v->fst), clone_cached_value(v->snd));
    default:
      return v;
  }
}

static Val *frozen_field_value(Val *v) {
  switch (v->tag) {
    case V_THUNK:
    case V_LTHUNK:
    case V_SUP:
      return clone_cached_value(v);
    default:
      return v;
  }
}

static void freeze_cached_value(Val *v) {
  switch (v->tag) {
    case V_CTR:
      v->pad = NEO_VAL_FROZEN;
      if (v->arity == 2) {
        freeze_cached_value(v->fst);
        freeze_cached_value(v->snd);
      } else if (v->arity == 1) {
        freeze_cached_value(v->fst);
      } else if (v->arity > 2) {
        for (u32 i = 0; i < v->arity; i++) {
          freeze_cached_value(v->item[i]);
        }
      }
      break;
    case V_SUP:
      freeze_cached_value(v->fst);
      freeze_cached_value(v->snd);
      break;
    default:
      break;
  }
}

NEO_INLINE Val *make_field(Code *kid, Env *env, u32 gap) {
  if (kid->op == BC_NUM) return mk_num(kid->ext);
  if (kid->op == BC_VAR) return env_get(env, kid->ext, gap);
  if (kid->op == BC_CTR) return make_ctr(kid, env, gap);
  return mk_lthunk(kid, env, gap);
}

static Val *eval_code_into(Code *pc, Env *env, u32 gap, Arg *args, u32 argc, Val *dst) {
  static void *dispatch[] = {
    &&do_arg, &&do_args, &&do_var, &&do_dp0, &&do_dp1, &&do_ref, &&do_call_ref, &&do_num,
    &&do_ctr, &&do_lam, &&do_elam, &&do_mat, &&do_mat_ctr, &&do_dup, &&do_sup, &&do_era,
    &&do_bad
  };
  if (!NEO_THREADED) {
    for (u32 i = 0; i < NEO_DEF_CAP; i++) {
      if (NEO_DEFS[i] != NULL) thread_code(NEO_DEFS[i], dispatch, 0);
    }
    NEO_THREADED = 1;
  }
  Val *val = NULL;
  goto *pc->jump;

do_arg:
  if (argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
  args[argc++] = arg_code(pc->sub, env, gap);
  pc = pc->next;
  goto *pc->jump;

do_args:
  if (argc + pc->arity > NEO_MAX_ARGS) neo_die("argument stack overflow");
  if (pc->arity == 4) {
    args[argc++] = arg_code(pc->kid[0], env, gap);
    args[argc++] = arg_code(pc->kid[1], env, gap);
    args[argc++] = arg_code(pc->kid[2], env, gap);
    args[argc++] = arg_code(pc->kid[3], env, gap);
  } else if (pc->arity == 3) {
    args[argc++] = arg_code(pc->kid[0], env, gap);
    args[argc++] = arg_code(pc->kid[1], env, gap);
    args[argc++] = arg_code(pc->kid[2], env, gap);
  } else if (pc->arity == 2) {
    args[argc++] = arg_code(pc->kid[0], env, gap);
    args[argc++] = arg_code(pc->kid[1], env, gap);
  } else if (pc->arity == 1) {
    args[argc++] = arg_code(pc->kid[0], env, gap);
  } else {
    for (u32 i = 0; i < pc->arity; i++) {
      args[argc++] = arg_code(pc->kid[i], env, gap);
    }
  }
  pc = pc->next;
  goto *pc->jump;

do_ref:
  if (pc->sub == NULL) neo_die("unknown reference");
  if (argc == 0 && pc->ext < NEO_DEF_CAP && NEO_REF_CACHE[pc->ext] != NULL) {
    Val *cached = NEO_REF_CACHE[pc->ext];
    val = cached->tag == V_CTR ? clone_cached_value(cached) : cached;
    goto apply_ready;
  }
  pc = pc->sub;
  env = NULL;
  gap = 0;
  goto *pc->jump;

do_call_ref:
  if (pc->next == NULL) neo_die("unknown reference");
  if (argc >= NEO_MAX_ARGS) neo_die("argument stack overflow");
  if (pc->next->op == BC_LAM || pc->next->op == BC_ELAM) {
    neo_itr(1);
    Arg arg = arg_code(pc->sub, env, gap);
    if (pc->next->op == BC_ELAM) {
      env = NULL;
      gap = 1;
    } else {
      env = env_cell(bind_arg(&arg), NULL, 1);
      gap = 0;
    }
    pc = pc->next->sub;
    goto *pc->jump;
  }
  if ((pc->next->op == BC_MAT || pc->next->op == BC_MAT_CTR) && (pc->sub->op == BC_CTR || pc->sub->op == BC_NUM)) {
    Code *arg_code = pc->sub;
    Code *body = mat_pick_code(pc->next, arg_code);
    if (body != NULL) {
      neo_itr(1);
      Env *body_env = NULL;
      u32 body_gap = 0;
      if (arg_code->op == BC_CTR) {
        if (!enter_ctr2_code_num_mat(&body, arg_code, env, gap, args, &argc)
        &&  !enter_ctr2_code_lams(&body, arg_code, env, gap, &body_env, &body_gap)) {
          push_ctr_code_args(arg_code, env, gap, args, &argc);
        }
      }
      pc = body;
      env = body_env;
      gap = body_gap;
      goto *pc->jump;
    }
  }
  args[argc++] = arg_code(pc->sub, env, gap);
  pc = pc->next;
  env = NULL;
  gap = 0;
  goto *pc->jump;

do_var:
  if (pc->ext == 1 && gap == 0) {
    if (!env || !env->val) neo_die("unbound variable");
    val = env->val;
  } else if (pc->ext == 2 && gap == 0 && env != NULL && env_span(env) == 1) {
    Env *next = env_next(env);
    if (!next || !next->val) neo_die("unbound variable");
    val = next->val;
  } else {
    val = env_get(env, pc->ext, gap);
  }
  if (__builtin_expect(val->tag == V_THUNK || val->tag == V_LTHUNK, 0)) goto apply_value;
  goto apply_ready;

do_dp0:
do_dp1:
  val = env_get(env, pc->ext, gap);
  if (pc->arity >= NEO_AUTO_LAB_BASE) {
    share_value(val);
  } else if (!is_lam(val) || val->code->sup_lab != pc->arity || val->code->sup_has) {
    val = project(val, pc->arity, pc->op == BC_DP0 ? 0 : 1);
  }
  goto apply_ready;

do_num:
  val = mk_num(pc->ext);
  goto apply_ready;

do_ctr:
  if (dst != NULL && argc == 0) {
    u16 refs = dst->pad;
    init_ctr(dst, pc, env, gap);
    dst->pad = refs;
    val = dst;
    dst = NULL;
  } else {
    val = make_ctr(pc, env, gap);
  }
  goto apply_ready;

do_lam:
  if (argc == 0) {
    val = mk_lam(pc->sub, env, gap);
    goto apply_ready;
  }
  neo_itr(1);
  {
    Arg *arg = &args[--argc];
    env = env_push_lam(bind_arg(arg), env, gap);
    gap = 0;
    pc = pc->sub;
    goto *pc->jump;
  }

do_elam:
  if (argc == 0) {
    val = mk_lam_tag(V_ELAM, pc->sub, env, gap);
    goto apply_ready;
  }
  neo_itr(1);
  argc--;
  gap++;
  pc = pc->sub;
  goto *pc->jump;

do_mat:
  if (__builtin_expect(argc == 0, 0)) {
    val = mk_mat(pc, env, gap);
    goto apply_ready;
  }
  {
    Arg *raw = &args[argc - 1];
    if (raw->val != NULL && raw->val->tag == V_NUM) {
      Code *body = mat_pick(pc, raw->val);
      if (body != NULL) {
        neo_itr(1);
        argc--;
        pc = body;
        goto *pc->jump;
      }
    }
    Env *arg_env;
    u32 arg_gap;
    Val *seen;
    Code *arg_code = matchable_arg_code(raw, &arg_env, &arg_gap, &seen);
    if (arg_code != NULL) {
      Code *body = mat_pick_code(pc, arg_code);
      if (body) {
        neo_itr(1);
        if (seen) consume_matchable_seen(seen);
        argc--;
        if (arg_code->op == BC_CTR) {
          if (!enter_ctr2_code_num_mat(&body, arg_code, arg_env, arg_gap, args, &argc)
          &&  !enter_ctr2_code_lams(&body, arg_code, arg_env, arg_gap, &env, &gap)) {
            push_ctr_code_args(arg_code, arg_env, arg_gap, args, &argc);
          }
        }
        pc = body;
        goto *pc->jump;
      }
    }
    neo_itr(1);
    argc--;
    Val *arg = seen ? force(seen) : force_arg(raw);
    if (arg->tag == V_SUP) {
      val = apply_mat(pc, env, gap, arg);
      goto apply_value;
    }
    Code *body = mat_pick(pc, arg);
    if (!body) {
      val = apply_default(pc->cases->dft, env, gap, arg);
      goto apply_value;
    }
    if (arg->tag == V_CTR) {
      if (!enter_ctr2_val_num_mat(&body, arg, args, &argc)
      &&  !enter_ctr2_val_lams(&body, arg, &env, &gap)) {
        push_ctr_val_args(arg, args, &argc);
      }
      val_free_ctr(arg);
    }
    pc = body;
    goto *pc->jump;
  }

do_mat_ctr:
  if (__builtin_expect(argc == 0, 0)) {
    val = mk_mat(pc, env, gap);
    goto apply_ready;
  }
  {
    Arg *raw = &args[argc - 1];
    Env *arg_env;
    u32 arg_gap;
    Val *seen;
    Code *arg_code = matchable_arg_code(raw, &arg_env, &arg_gap, &seen);
    if (arg_code != NULL && arg_code->op == BC_CTR && arg_code->ext == pc->ext) {
      neo_itr(1);
      if (seen) consume_matchable_seen(seen);
      argc--;
      Code *body = pc->sub;
      if (!enter_ctr2_code_num_mat(&body, arg_code, arg_env, arg_gap, args, &argc)
      &&  !enter_ctr2_code_lams(&body, arg_code, arg_env, arg_gap, &env, &gap)) {
        push_ctr_code_args(arg_code, arg_env, arg_gap, args, &argc);
      }
      pc = body;
      goto *pc->jump;
    }
    neo_itr(1);
    argc--;
    Val *arg = seen ? force(seen) : force_arg(raw);
    if (arg->tag == V_SUP) {
      val = apply_mat(pc, env, gap, arg);
      goto apply_value;
    }
    if (arg->tag == V_CTR && arg->ext == pc->ext) {
      Code *body = pc->sub;
      if (!enter_ctr2_val_num_mat(&body, arg, args, &argc)
      &&  !enter_ctr2_val_lams(&body, arg, &env, &gap)) {
        push_ctr_val_args(arg, args, &argc);
      }
      val_free_ctr(arg);
      pc = body;
      goto *pc->jump;
    }
    val = mk_num(0);
    goto apply_value;
  }

do_dup:
  {
    Val *v = force(mk_thunk(pc->sub, env, gap));
    if (pc->ext >= NEO_AUTO_LAB_BASE) {
      share_value(v);
    } else if (is_lam(v) && code_has_sup_label(v->code, pc->ext, 0)) {
      v = mk_dlam(v, pc->ext);
    }
    env = env_push_lam(v, env, gap);
    gap = 0;
    pc = pc->next;
    goto *pc->jump;
  }

do_sup:
  val = mk_sup(pc->ext, mk_thunk(pc->kid[0], env, gap), mk_thunk(pc->kid[1], env, gap));
  goto apply_ready;

do_era:
  val = val_new(V_ERA);
  goto apply_ready;

do_bad:
  neo_fail();
  return val_new(V_ERA);

apply_value:
  val = force(val);
apply_ready:
  if (argc == 0) return val;
  switch (val->tag) {
    case V_LAM: {
      neo_itr(1);
      Arg *arg = &args[--argc];
      env = val->env;
      gap = val->ext;
      env = env_push_lam(bind_arg(arg), env, gap);
      gap = 0;
      pc = val->code;
      goto *pc->jump;
    }
    case V_ELAM: {
      neo_itr(1);
      argc--;
      env = val->env;
      gap = val->ext + 1;
      pc = val->code;
      goto *pc->jump;
    }
    case V_SEL: {
      if (argc >= 2) {
        Arg *fst = &args[argc - 1];
        Arg *snd = &args[argc - 2];
        Arg *pick = val->ext == 0 ? fst : snd;
        argc -= 2;
        neo_itr(2);
        val = bind_arg(pick);
        if (__builtin_expect(val->tag == V_THUNK || val->tag == V_LTHUNK, 0)) goto apply_value;
        goto apply_ready;
      }
      Arg arg = args[--argc];
      val = apply_sel(val, &arg);
      goto apply_ready;
    }
    case V_PSEL: {
      Arg arg = args[--argc];
      val = apply_sel(val, &arg);
      if (__builtin_expect(val->tag == V_THUNK || val->tag == V_LTHUNK, 0)) goto apply_value;
      goto apply_ready;
    }
    case V_CALL2: {
      Arg arg = args[--argc];
      val = apply_call2(val, &arg);
      if (__builtin_expect(val->tag == V_THUNK || val->tag == V_LTHUNK, 0)) goto apply_value;
      goto apply_ready;
    }
    case V_MAT: {
      Arg arg = args[--argc];
      val = apply_fun(val, &arg);
      goto apply_value;
    }
    case V_SUP: {
      Arg arg = args[--argc];
      val = apply_sup(val, &arg);
      goto apply_value;
    }
    case V_PRJ: {
      Arg arg = args[--argc];
      val = apply_fun(val, &arg);
      goto apply_value;
    }
    case V_PLAM: {
      Arg arg = args[--argc];
      val = apply_plam(val, &arg);
      goto apply_value;
    }
    default:
      if (val->tag == V_VAR || val->tag == V_APP || val->tag == V_BOX) {
        Arg arg = args[--argc];
        val = mk_app(val, arg.val != NULL ? arg.val : mk_thunk(arg.code, arg.env, arg.gap));
        goto apply_value;
      }
      neo_die("cannot apply value");
  }
  return val;
}

NEO_INLINE Val *force(Val *v) {
  while (v->tag == V_THUNK || v->tag == V_LTHUNK) {
    if (v->tag == V_LTHUNK && v->code->op == BC_CTR) {
      u16 refs = v->pad;
      init_ctr(v, v->code, v->env, v->ext);
      v->pad = refs;
      return v;
    }
    u8 tag = v->tag;
    u16 refs = v->pad & NEO_VAL_REFS_MASK;
    Arg args[NEO_MAX_ARGS];
    Val *res = eval_code_into(v->code, v->env, v->ext, args, 0, tag == V_LTHUNK ? v : NULL);
    if (tag == V_LTHUNK && res != v) {
      u16 res_pad = res->pad;
      *v = *res;
      if (v->tag == V_CTR) {
        v->pad = refs | (res_pad == NEO_VAL_FROZEN ? NEO_VAL_BORROWED : 0);
        val_free_ctr(res);
      }
    } else {
      v = res;
    }
  }
  return v;
}

static void print_var_name(u32 idx) {
  if (idx < 26) {
    putchar((char)('a' + idx));
  } else {
    printf("x%u", idx);
  }
}

static void print_val_at(Val *v, u32 depth);
static void normalize_val_at(Val *v, u32 depth);
static void normalize_forced_val_at(Val *v, u32 depth);

static void print_val_at(Val *v, u32 depth) {
  v = force(v);
  switch (v->tag) {
    case V_NUM:
      printf("%u", v->ext);
      return;
    case V_CTR:
      fputc('#', stdout);
      print_sym_name(stdout, v->ext);
      if (v->arity > 0) {
        fputc('{', stdout);
        for (u32 i = 0; i < v->arity; i++) {
          if (i > 0) fputc(',', stdout);
          print_val_at(ctr_get(v, i), depth);
        }
        fputc('}', stdout);
      }
      return;
    case V_LAM:
    case V_ELAM: {
      fputs("λ", stdout);
      print_var_name(depth);
      fputc('.', stdout);
      Env *env = v->env;
      u32 gap = v->ext;
      if (v->tag == V_ELAM) {
        gap++;
      } else {
        env = env_push(mk_var(depth), env, gap + 1);
        gap = 0;
      }
      Arg none[NEO_MAX_ARGS];
      Val *body = eval_code(v->code, env, gap, none, 0);
      print_val_at(body, depth + 1);
      return;
    }
    case V_SEL:
      fputs("λ", stdout);
      print_var_name(depth);
      fputs(".λ", stdout);
      print_var_name(depth + 1);
      fputc('.', stdout);
      print_var_name(v->ext == 0 ? depth : depth + 1);
      return;
    case V_PSEL:
      fputs("λ", stdout);
      print_var_name(depth);
      fputc('.', stdout);
      if (v->ext == 0) {
        print_val_at(v->fst, depth + 1);
      } else {
        print_var_name(depth);
      }
      return;
    case V_CALL2:
      fputs("λ", stdout);
      print_var_name(depth);
      fputc('.', stdout);
      print_var_name(depth);
      fputc('(', stdout);
      print_val_at(v->fst, depth);
      fputc(',', stdout);
      print_val_at(v->snd, depth);
      fputc(')', stdout);
      return;
    case V_SUP:
      fputc('&', stdout);
      print_name(stdout, v->ext);
      fputc('{', stdout);
      print_val_at(v->fst, depth);
      fputc(',', stdout);
      print_val_at(v->snd, depth);
      fputc('}', stdout);
      return;
    case V_VAR:
      print_var_name(v->ext);
      return;
    case V_APP:
      print_val_at(v->fst, depth);
      fputc('(', stdout);
      print_val_at(v->snd, depth);
      fputc(')', stdout);
      return;
    case V_BOX:
      if (v->fst) {
        print_val_at(v->fst, depth);
      } else {
        fputc('_', stdout);
      }
      return;
    case V_ERA:
      fputs("&{}", stdout);
      return;
    default:
      fputc('_', stdout);
      return;
  }
}

static void print_val(Val *v) {
  print_val_at(v, 0);
}

static void normalize_mat_val(Val *v, u32 depth) {
  for (Case *m = v->code->cases ? v->code->cases->head : NULL; m != NULL; m = m->next) {
    Arg none[NEO_MAX_ARGS];
    Val *body = eval_code(m->body, v->env, v->ext, none, 0);
    normalize_val_at(body, depth);
  }
  if (v->code->cases && v->code->cases->dft) {
    Arg none[NEO_MAX_ARGS];
    Val *body = eval_code(v->code->cases->dft, v->env, v->ext, none, 0);
    normalize_val_at(body, depth);
  }
}

static void normalize_val_at(Val *v, u32 depth) {
  v = force(v);
  normalize_forced_val_at(v, depth);
}

static void normalize_forced_val_at(Val *v, u32 depth) {
  switch (v->tag) {
    case V_CTR:
      if (v->arity == 2) {
        normalize_val_at(v->fst, depth);
        normalize_val_at(v->snd, depth);
        return;
      }
      for (u32 i = 0; i < v->arity; i++) {
        normalize_val_at(ctr_get(v, i), depth);
      }
      return;
    case V_LAM:
    case V_ELAM: {
      Env *env = v->env;
      u32 gap = v->ext;
      if (v->tag == V_ELAM) {
        gap++;
      } else {
        env = env_push(mk_var(depth), env, gap + 1);
        gap = 0;
      }
      Arg none[NEO_MAX_ARGS];
      Val *body = eval_code(v->code, env, gap, none, 0);
      normalize_val_at(body, depth + 1);
      return;
    }
    case V_MAT:
      normalize_mat_val(v, depth);
      return;
    case V_SEL:
      return;
    case V_PSEL:
      if (v->ext == 0 && v->fst != NULL) normalize_val_at(v->fst, depth);
      return;
    case V_CALL2:
      normalize_val_at(v->fst, depth);
      normalize_val_at(v->snd, depth);
      return;
    case V_SUP:
      normalize_val_at(v->fst, depth);
      normalize_val_at(v->snd, depth);
      return;
    case V_APP:
      normalize_val_at(v->fst, depth);
      normalize_val_at(v->snd, depth);
      return;
    case V_PRJ:
    case V_PLAM:
      normalize_val_at(v->fst, depth);
      return;
    default:
      return;
  }
}

static void normalize_val(Val *v) {
  normalize_val_at(v, 0);
}

static int neo_eval_main(u32 main_id, int silent) {
  NEO_THREADED = 0;
  NEO_ITRS = 0;
  if (NEO_FAILED || main_id >= NEO_DEF_CAP || NEO_DEFS[main_id] == NULL) {
    return 0;
  }
  Arg args[NEO_MAX_ARGS];
  Val *res = force(eval_code(NEO_DEFS[main_id], NULL, 0, args, 0));
  if (NEO_FAILED) {
    return 0;
  }
  normalize_val(res);
  if (!silent) {
    print_val(res);
    putchar('\n');
  }
  if (ITRS_ENABLED) {
    ITRS += NEO_ITRS;
  }
  return 1;
}

// Nano VM
// =======
// Compact evaluator for the pure first-order case-tree subset used by direct
// NanoHVM ports. It compiles HVM's static book terms to a tiny stack VM whose
// primitives correspond to Nano's GET/LAM/DEL/MAT/TUP/CALL operations. Programs
// outside this subset are declined and evaluated by the general evaluator.

#define NV_NUM_BIT   0x80000000u
#define NV_LOC_MASK  0x7FFFFFFFu
#define NV_CODE_CAP  (1u << 20)
#define NV_HEAP_CAP  (1u << 20)
#define NV_STACK_CAP (1u << 20)
#define NV_CTX_CAP   (1u << 20)
#define NV_CALL_CAP  65536u
#define NV_BIND_CAP  256u

typedef u32 NvTerm;

enum {
  NV_NUM,
  NV_TUP,
  NV_VAR,
  NV_GET,
  NV_LAM,
  NV_DEL,
  NV_MAT,
  NV_DUP,
  NV_GET_MAT,
  NV_TUP_L_NUM,
  NV_TUP_R_NUM,
  NV_TUP_L_VAR,
  NV_TUP_R_VAR,
  NV_REC_FN,
  NV_TAI_FN,
  NV_END
};

typedef struct {
  u8  op;
  u8  pad;
  u16 idx;
  u32 ext;
} NvOp;

typedef struct {
  u32 pc;
  u32 frm;
  u32 cur;
  u32 fun;
} NvFrame;

typedef struct {
  u8  tag;
  u32 ext;
  u32 lvl;
  u32 idx;
} NvBind;

typedef struct {
  NvBind bind[NV_BIND_CAP];
  u32    len;
  u32    slots;
  u32    level;
} NvScope;

typedef struct {
  NvOp    *code;
  NvTerm  *heap_l;
  NvTerm  *heap_r;
  u16     *heap_refs;
  NvTerm  *vals;
  NvTerm  *terms;
  NvTerm  *collect;
  NvTerm  *ctx;
  NvFrame *calls;
  u32     *fn_start;
  u32     *fn_name;
  int     *fn_of_name;
  u32      code_len;
  u32      heap_cur;
  u32      fn_count;
  u32      table_len;
  u32      main_start;
  u32      compile_name;
  u32      pair_ext;
  int      pair_ext_set;
  u64      itrs;
  const char *fail_reason;
  u32      fail_tag;
  u32      trace_pc;
  u8       trace_op;
  int      failed;
} NvRun;

static NvRun NV_PREPARED_RUN = {0};
static int   NV_PREPARED = 0;
static u32   NV_PREPARED_MAIN = 0;

fn NvTerm nv_num(u32 val) {
  return NV_NUM_BIT | val;
}

fn int nv_is_num(NvTerm term) {
  return (term & NV_NUM_BIT) != 0;
}

fn u32 nv_num_val(NvTerm term) {
  return term & NV_LOC_MASK;
}

fn void nv_fail(NvRun *run) {
  run->failed = 1;
}

fn void nv_fail_because(NvRun *run, const char *reason, u32 tag) {
  if (run->fail_reason == NULL) {
    run->fail_reason = reason;
    run->fail_tag = tag;
  }
  nv_fail(run);
}

fn int nv_is_func_term(Term term) {
  switch (term_tag(term)) {
    case LAM:
    case MAT:
    case SWI:
      return 1;
    default:
      return 0;
  }
}

fn int nv_emit(NvRun *run, u8 op, u32 ext, u32 *pos) {
  if (run->code_len >= NV_CODE_CAP) {
    nv_fail(run);
    return 0;
  }
  if (pos != NULL) {
    *pos = run->code_len;
  }
  run->code[run->code_len++] = (NvOp){.op = op, .ext = ext};
  return 1;
}

fn int nv_scope_add(NvRun *run, NvScope *scope, u8 tag, u32 ext, u32 lvl) {
  if (scope->len >= NV_BIND_CAP) {
    nv_fail(run);
    return 0;
  }
  scope->bind[scope->len++] = (NvBind){tag, ext, lvl, scope->slots++};
  return 1;
}

fn int nv_scope_find(NvScope *scope, u8 tag, u32 ext, u32 lvl, u32 *idx) {
  for (int i = (int)scope->len - 1; i >= 0; i--) {
    NvBind *bind = &scope->bind[i];
    if (bind->tag == tag && bind->lvl == lvl && (tag == BJV || bind->ext == ext)) {
      *idx = bind->idx;
      return 1;
    }
  }
  return 0;
}

fn int nv_pair_ext(NvRun *run, u32 ext) {
  if (!run->pair_ext_set) {
    run->pair_ext = ext;
    run->pair_ext_set = 1;
    return 1;
  }
  return run->pair_ext == ext;
}

fn int nv_collect_app(u64 loc, u64 *args, u32 *argc, Term *fun) {
  *argc = 0;
  Term term = heap_read(loc);
  while (term_tag(term) == APP) {
    if (*argc >= 16) {
      return 0;
    }
    u64 app_loc = term_val(term);
    args[(*argc)++] = app_loc + 1;
    term = heap_read(app_loc + 0);
  }
  *fun = term;
  return 1;
}

fn int nv_compile_expr(NvRun *run, u64 loc, NvScope scope, int tail, u32 cur_fn);
fn int nv_compile_func(NvRun *run, u64 loc, NvScope scope, u32 cur_fn);

fn int nv_term_var_idx(Term term, NvScope *scope, u32 *idx) {
  u8 tag = term_tag(term);
  if (tag != BJV && tag != BJ0 && tag != BJ1) {
    return 0;
  }
  return nv_scope_find(scope, tag, term_ext(term), term_val(term), idx);
}

fn void nv_find_dup_levels_go(Term term, u32 lab, u32 *lvl0, u32 *lvl1) {
  u8 tag = term_tag(term);
  if (tag == BJ0 && term_ext(term) == lab && *lvl0 == 0) {
    *lvl0 = term_val(term);
    return;
  }
  if (tag == BJ1 && term_ext(term) == lab && *lvl1 == 0) {
    *lvl1 = term_val(term);
    return;
  }
  u32 ari = term_arity(term);
  u64 loc = term_val(term);
  for (u32 i = 0; i < ari; i++) {
    nv_find_dup_levels_go(heap_read(loc + i), lab, lvl0, lvl1);
  }
}

fn void nv_find_dup_levels(Term body, u32 lab, u32 fallback, u32 *lvl0, u32 *lvl1) {
  *lvl0 = 0;
  *lvl1 = 0;
  nv_find_dup_levels_go(body, lab, lvl0, lvl1);
  if (*lvl0 == 0) *lvl0 = fallback;
  if (*lvl1 == 0) *lvl1 = fallback;
}

fn int nv_compile_expr(NvRun *run, u64 loc, NvScope scope, int tail, u32 cur_fn) {
  if (run->failed) {
    return 0;
  }
  Term term = heap_read(loc);
  switch (term_tag(term)) {
    case NUM: {
      if (!nv_emit(run, NV_NUM, (u32)term_val(term), NULL)) return 0;
      if (tail && !nv_emit(run, NV_END, 0, NULL)) return 0;
      return 1;
    }
    case C02: {
      if (!nv_pair_ext(run, term_ext(term))) {
        nv_fail_because(run, "pair-ext", term_ext(term));
        return 0;
      }
      u64 ctr_loc = term_val(term);
      Term lhs = heap_read(ctr_loc + 0);
      Term rhs = heap_read(ctr_loc + 1);
      u32 idx = 0;
      if (term_tag(lhs) == NUM) {
        if (!nv_compile_expr(run, ctr_loc + 1, scope, 0, cur_fn)) return 0;
        if (!nv_emit(run, NV_TUP_L_NUM, (u32)term_val(lhs), NULL)) return 0;
      } else if (term_tag(rhs) == NUM) {
        if (!nv_compile_expr(run, ctr_loc + 0, scope, 0, cur_fn)) return 0;
        if (!nv_emit(run, NV_TUP_R_NUM, (u32)term_val(rhs), NULL)) return 0;
      } else if (nv_term_var_idx(lhs, &scope, &idx)) {
        if (!nv_compile_expr(run, ctr_loc + 1, scope, 0, cur_fn)) return 0;
        if (!nv_emit(run, NV_TUP_L_VAR, idx, NULL)) return 0;
      } else if (nv_term_var_idx(rhs, &scope, &idx)) {
        if (!nv_compile_expr(run, ctr_loc + 0, scope, 0, cur_fn)) return 0;
        if (!nv_emit(run, NV_TUP_R_VAR, idx, NULL)) return 0;
      } else {
        if (!nv_compile_expr(run, ctr_loc + 0, scope, 0, cur_fn)) return 0;
        if (!nv_compile_expr(run, ctr_loc + 1, scope, 0, cur_fn)) return 0;
        if (!nv_emit(run, NV_TUP, 0, NULL)) return 0;
      }
      if (tail && !nv_emit(run, NV_END, 0, NULL)) return 0;
      return 1;
    }
    case BJV:
    case BJ0:
    case BJ1: {
      u32 idx = 0;
      if (!nv_scope_find(&scope, term_tag(term), term_ext(term), term_val(term), &idx)) {
        nv_fail_because(run, "scope", term_tag(term));
        return 0;
      }
      if (!nv_emit(run, NV_VAR, idx, NULL)) return 0;
      if (tail && !nv_emit(run, NV_END, 0, NULL)) return 0;
      return 1;
    }
    case REF: {
      u32 nam = term_ext(term);
      if (nam >= run->table_len || BOOK[nam] == 0 || run->fn_of_name[nam] >= 0) {
        nv_fail_because(run, "ref", nam);
        return 0;
      }
      return nv_compile_expr(run, BOOK[nam], scope, tail, cur_fn);
    }
    case APP: {
      u64 args[16];
      u32 argc = 0;
      Term fun = 0;
      if (!nv_collect_app(loc, args, &argc, &fun) || argc != 1 || term_tag(fun) != REF) {
        nv_fail_because(run, "app", term_tag(fun));
        return 0;
      }
      u32 nam = term_ext(fun);
      if (nam >= run->table_len || run->fn_of_name[nam] < 0) {
        nv_fail_because(run, "call-ref", nam);
        return 0;
      }
      if (!nv_compile_expr(run, args[0], scope, 0, cur_fn)) return 0;
      if (!nv_emit(run, tail ? NV_TAI_FN : NV_REC_FN, (u32)run->fn_of_name[nam], NULL)) return 0;
      return 1;
    }
    default: {
      nv_fail_because(run, "expr-tag", term_tag(term));
      return 0;
    }
  }
}

fn int nv_compile_func(NvRun *run, u64 loc, NvScope scope, u32 cur_fn) {
  if (run->failed) {
    return 0;
  }
  Term term = heap_read(loc);
  switch (term_tag(term)) {
    case LAM: {
      scope.level++;
      if (term_ext(term) & LAM_ERA_MASK) {
        if (!nv_emit(run, NV_DEL, 0, NULL)) return 0;
      } else {
        if (!nv_emit(run, NV_LAM, 0, NULL)) return 0;
        if (!nv_scope_add(run, &scope, BJV, 0, scope.level)) return 0;
      }
      return nv_compile_func(run, term_val(term), scope, cur_fn);
    }
    case MAT: {
      u64 mat_loc = term_val(term);
      Term dft = heap_read(mat_loc + 1);
      if (term_tag(dft) != NUM || term_val(dft) != 0) {
        nv_fail_because(run, "mat-default", term_tag(dft));
        return 0;
      }
      if (!nv_pair_ext(run, term_ext(term))) {
        nv_fail_because(run, "mat-pair-ext", term_ext(term));
        return 0;
      }
      Term body = heap_read(mat_loc + 0);
      if (term_tag(body) == SWI && term_ext(body) == 0) {
        u64 body_loc = term_val(body);
        Term one = heap_read(body_loc + 1);
        if (term_tag(one) == SWI && term_ext(one) == 1) {
          u64 one_loc = term_val(one);
          u32 pos = 0;
          if (!nv_emit(run, NV_GET_MAT, 0, &pos)) return 0;
          if (!nv_compile_func(run, body_loc + 0, scope, cur_fn)) return 0;
          run->code[pos].ext = run->code_len;
          return nv_compile_func(run, one_loc + 0, scope, cur_fn);
        }
      }
      if (!nv_emit(run, NV_GET, 0, NULL)) return 0;
      return nv_compile_func(run, mat_loc + 0, scope, cur_fn);
    }
    case DUP: {
      u64 dup_loc = term_val(term);
      u32 lab = term_ext(term);
      u32 lvl0 = 0;
      u32 lvl1 = 0;
      u32 dup_level = scope.level + 1;
      nv_find_dup_levels(heap_read(dup_loc + 1), lab, dup_level, &lvl0, &lvl1);
      if (!nv_compile_expr(run, dup_loc + 0, scope, 0, cur_fn)) return 0;
      if (!nv_emit(run, NV_DUP, 0, NULL)) return 0;
      scope.level = dup_level;
      if (lvl0 > scope.level) scope.level = lvl0;
      if (lvl1 > scope.level) scope.level = lvl1;
      if (!nv_scope_add(run, &scope, BJ0, lab, lvl0)) return 0;
      if (!nv_scope_add(run, &scope, BJ1, lab, lvl1)) return 0;
      return nv_compile_func(run, dup_loc + 1, scope, cur_fn);
    }
    case SWI: {
      u64 swi_loc = term_val(term);
      Term one = heap_read(swi_loc + 1);
      if (term_ext(term) != 0 || term_tag(one) != SWI || term_ext(one) != 1) {
        nv_fail_because(run, "swi-shape", term_ext(term));
        return 0;
      }
      u64 one_loc = term_val(one);
      u32 pos = 0;
      if (!nv_emit(run, NV_MAT, 0, &pos)) return 0;
      if (!nv_compile_func(run, swi_loc + 0, scope, cur_fn)) return 0;
      run->code[pos].ext = run->code_len;
      return nv_compile_func(run, one_loc + 0, scope, cur_fn);
    }
    default: {
      return nv_compile_expr(run, loc, scope, 1, cur_fn);
    }
  }
}

fn int nv_compile_program(NvRun *run, u32 main_id) {
  run->table_len = TABLE.len;
  run->fn_of_name = (int*)malloc(sizeof(int) * run->table_len);
  if (run->fn_of_name == NULL) {
    nv_fail(run);
    return 0;
  }
  for (u32 i = 0; i < run->table_len; i++) {
    run->fn_of_name[i] = -1;
  }
  for (u32 i = 0; i < run->table_len; i++) {
    if (BOOK[i] != 0 && nv_is_func_term(heap_read(BOOK[i]))) {
      run->fn_of_name[i] = (int)run->fn_count++;
    }
  }
  run->fn_start = (u32*)calloc(run->fn_count == 0 ? 1 : run->fn_count, sizeof(u32));
  run->fn_name = (u32*)calloc(run->fn_count == 0 ? 1 : run->fn_count, sizeof(u32));
  if (run->fn_start == NULL || run->fn_name == NULL) {
    nv_fail(run);
    return 0;
  }
  for (u32 i = 0; i < run->table_len; i++) {
    int fidx = run->fn_of_name[i];
    if (fidx >= 0) {
      run->fn_name[fidx] = i;
    }
  }
  for (u32 fidx = 0; fidx < run->fn_count; fidx++) {
    u32 nam = run->fn_name[fidx];
    run->compile_name = nam;
    run->fn_start[fidx] = run->code_len;
    NvScope scope = {0};
    if (!nv_compile_func(run, BOOK[nam], scope, fidx)) {
      return 0;
    }
  }
  run->main_start = run->code_len;
  run->compile_name = main_id;
  NvScope scope = {0};
  return nv_compile_expr(run, BOOK[main_id], scope, 1, run->fn_count);
}

fn u32 nv_alloc(NvRun *run) {
  u32 loc = run->heap_cur;
  for (;;) {
    loc++;
    if (loc >= NV_HEAP_CAP) loc = 1;
    if (run->heap_l[loc] == 0 && run->heap_r[loc] == 0) {
      run->heap_cur = loc;
      return loc;
    }
    if (loc == run->heap_cur) {
      nv_fail(run);
      return 0;
    }
  }
}

fn void nv_collect(NvRun *run, NvTerm term) {
  if (nv_is_num(term) || term == 0) {
    return;
  }
  if (run->heap_refs[term] > 0) {
    run->heap_refs[term]--;
    return;
  }
  NvTerm *stack = run->collect;
  u32 sp = 0;
  for (;;) {
    if (run->heap_refs[term] > 0) {
      run->heap_refs[term]--;
      if (sp == 0) return;
      term = stack[--sp];
      continue;
    }
    NvTerm l = run->heap_l[term];
    NvTerm r = run->heap_r[term];
    run->heap_l[term] = 0;
    run->heap_r[term] = 0;
    if (!nv_is_num(r) && r != 0) stack[sp++] = r;
    if (!nv_is_num(l) && l != 0) {
      term = l;
      continue;
    }
    if (sp == 0) return;
    term = stack[--sp];
  }
}

fn void nv_retain(NvRun *run, NvTerm term) {
  if (!nv_is_num(term) && term != 0 && run->heap_refs[term] != UINT16_MAX) {
    run->heap_refs[term]++;
  }
}

fn NvTerm nv_run(NvRun *run) {
  static void *dispatch[] = {
    &&do_num, &&do_tup, &&do_var, &&do_get, &&do_lam, &&do_del, &&do_mat,
    &&do_dup, &&do_get_mat, &&do_tup_l_num, &&do_tup_r_num, &&do_tup_l_var, &&do_tup_r_var,
    &&do_rec_fn, &&do_tai_fn, &&do_end
  };
  NvTerm *vals = run->vals;
  NvTerm *terms = run->terms;
  NvTerm *ctx = run->ctx;
  NvFrame *calls = run->calls;
  u32 vsp = 0;
  u32 tsp = 0;
  u32 csp = 0;
  u32 cur = 0;
  u32 frm = 0;
  u32 pc = run->main_start;
  u32 fun = run->fn_count;
  NvTerm term = 0;

  #define NV_NEXT() do { goto *dispatch[run->code[pc++].op]; } while (0)
  #define NV_DEFOREST_PAIR(lv, rv) do { \
    NvOp *call_ = &run->code[pc]; \
    if ((call_->op == NV_TAI_FN || call_->op == NV_REC_FN) && \
        call_->ext < run->fn_count && \
        (run->code[run->fn_start[call_->ext]].op == NV_GET || \
         run->code[run->fn_start[call_->ext]].op == NV_GET_MAT)) { \
      pc++; \
      u32 start_ = run->fn_start[call_->ext]; \
      u8 start_op_ = run->code[start_].op; \
      run->itrs += start_op_ == NV_GET_MAT ? 3 : 2; \
      if (call_->op == NV_REC_FN) { \
        if (csp >= NV_CALL_CAP) { nv_fail_because(run, "call-stack", 0); return 0; } \
        calls[csp++] = (NvFrame){pc, frm, cur, fun}; \
        frm = cur; \
      } else { \
        cur = frm; \
      } \
      fun = call_->ext; \
      if (start_op_ == NV_GET_MAT) { \
        if (!nv_is_num((lv))) { nv_fail_because(run, "def-mat", (lv)); return 0; } \
        term = (rv); \
        tsp = 0; \
        pc = nv_num_val((lv)) == 1 ? run->code[start_].ext : start_ + 1; \
      } else { \
        terms[0] = (rv); \
        tsp = 1; \
        term = (lv); \
        pc = start_ + 1; \
      } \
      NV_NEXT(); \
    } \
  } while (0)

  NV_NEXT();

do_num: {
    NvOp *op = &run->code[pc - 1];
    if (vsp >= NV_STACK_CAP) { nv_fail(run); return 0; }
    vals[vsp++] = nv_num(op->ext);
    NV_NEXT();
  }

do_tup: {
    if (vsp < 2) { nv_fail(run); return 0; }
    NvTerm r = vals[--vsp];
    NvTerm l = vals[--vsp];
    NV_DEFOREST_PAIR(l, r);
    NvOp *call = &run->code[pc];
    if ((call->op == NV_TAI_FN || call->op == NV_REC_FN) &&
        call->ext < run->fn_count &&
        (run->code[run->fn_start[call->ext]].op == NV_GET ||
         run->code[run->fn_start[call->ext]].op == NV_GET_MAT)) {
      pc++;
      u32 start = run->fn_start[call->ext];
      u8 start_op = run->code[start].op;
      run->itrs += start_op == NV_GET_MAT ? 3 : 2;
      if (call->op == NV_REC_FN) {
        if (csp >= NV_CALL_CAP) { nv_fail_because(run, "call-stack", 0); return 0; }
        calls[csp++] = (NvFrame){pc, frm, cur, fun};
        frm = cur;
      } else {
        cur = frm;
      }
      fun = call->ext;
      if (start_op == NV_GET_MAT) {
        if (!nv_is_num(l)) { nv_fail_because(run, "def-mat", l); return 0; }
        term = r;
        tsp = 0;
        pc = nv_num_val(l) == 1 ? run->code[start].ext : start + 1;
      } else {
        terms[0] = r;
        tsp = 1;
        term = l;
        pc = start + 1;
      }
      NV_NEXT();
    }
    if (run->code[pc].op == NV_TUP) {
      NvOp *call2 = &run->code[pc + 1];
      if ((call2->op == NV_TAI_FN || call2->op == NV_REC_FN) &&
          call2->ext < run->fn_count &&
          (run->code[run->fn_start[call2->ext]].op == NV_GET ||
           run->code[run->fn_start[call2->ext]].op == NV_GET_MAT) &&
          vsp > 0) {
        u32 inner = nv_alloc(run);
        if (run->failed) return 0;
        run->heap_l[inner] = l;
        run->heap_r[inner] = r;
        NvTerm outer_l = vals[--vsp];
        pc += 2;
        u32 start = run->fn_start[call2->ext];
        u8 start_op = run->code[start].op;
        run->itrs += start_op == NV_GET_MAT ? 3 : 2;
        if (call2->op == NV_REC_FN) {
          if (csp >= NV_CALL_CAP) { nv_fail_because(run, "call-stack", 0); return 0; }
          calls[csp++] = (NvFrame){pc, frm, cur, fun};
          frm = cur;
        } else {
          cur = frm;
        }
        fun = call2->ext;
        if (start_op == NV_GET_MAT) {
          if (!nv_is_num(outer_l)) { nv_fail_because(run, "def-mat", outer_l); return 0; }
          term = inner;
          tsp = 0;
          pc = nv_num_val(outer_l) == 1 ? run->code[start].ext : start + 1;
        } else {
          terms[0] = inner;
          tsp = 1;
          term = outer_l;
          pc = start + 1;
        }
        NV_NEXT();
      }
    }
    u32 loc = nv_alloc(run);
    if (run->failed) return 0;
    run->heap_l[loc] = l;
    run->heap_r[loc] = r;
    vals[vsp++] = loc;
    NV_NEXT();
  }

do_var: {
    NvOp *op = &run->code[pc - 1];
    if (vsp >= NV_STACK_CAP) { nv_fail(run); return 0; }
    vals[vsp++] = ctx[frm + op->ext];
    NV_NEXT();
  }

do_get: {
    if (nv_is_num(term) || term == 0 || tsp >= NV_STACK_CAP) { nv_fail_because(run, "get", term); return 0; }
    run->itrs++;
    NvTerm l = run->heap_l[term];
    NvTerm r = run->heap_r[term];
    if (run->heap_refs[term] > 0) {
      run->heap_refs[term]--;
      nv_retain(run, l);
      nv_retain(run, r);
    } else {
      run->heap_l[term] = 0;
      run->heap_r[term] = 0;
    }
    terms[tsp++] = r;
    term = l;
    NV_NEXT();
  }

do_lam: {
    if (cur >= NV_CTX_CAP) { nv_fail_because(run, "lam", tsp); return 0; }
    run->itrs++;
    ctx[cur++] = term;
    term = tsp > 0 ? terms[--tsp] : 0;
    NV_NEXT();
  }

do_del: {
    run->itrs++;
    nv_collect(run, term);
    term = tsp > 0 ? terms[--tsp] : 0;
    NV_NEXT();
  }

do_mat: {
    NvOp *op = &run->code[pc - 1];
    if (!nv_is_num(term)) { nv_fail_because(run, "mat", term); return 0; }
    run->itrs++;
    u32 pick = nv_num_val(term);
    term = tsp > 0 ? terms[--tsp] : 0;
    if (pick == 1) pc = op->ext;
    NV_NEXT();
  }

do_dup: {
    if (vsp == 0 || cur + 2 >= NV_CTX_CAP) { nv_fail(run); return 0; }
    run->itrs++;
    NvTerm val = vals[--vsp];
    nv_retain(run, val);
    ctx[cur++] = val;
    ctx[cur++] = val;
    NV_NEXT();
  }

do_get_mat: {
    NvOp *op = &run->code[pc - 1];
    if (nv_is_num(term) || term == 0) { nv_fail_because(run, "get-mat", term); return 0; }
    NvTerm l = run->heap_l[term];
    NvTerm r = run->heap_r[term];
    if (!nv_is_num(l)) { nv_fail_because(run, "get-mat-tag", l); return 0; }
    run->itrs += 2;
    if (run->heap_refs[term] > 0) {
      run->heap_refs[term]--;
      nv_retain(run, r);
    } else {
      run->heap_l[term] = 0;
      run->heap_r[term] = 0;
    }
    term = r;
    if (nv_num_val(l) == 1) pc = op->ext;
    NV_NEXT();
  }

do_tup_l_num: {
    NvOp *op = &run->code[pc - 1];
    if (vsp < 1) { nv_fail_because(run, "tup-l-num", 0); return 0; }
    NvTerm r = vals[--vsp];
    NV_DEFOREST_PAIR(nv_num(op->ext), r);
    u32 loc = nv_alloc(run);
    if (run->failed) return 0;
    run->heap_l[loc] = nv_num(op->ext);
    run->heap_r[loc] = r;
    vals[vsp++] = loc;
    NV_NEXT();
  }

do_tup_r_num: {
    NvOp *op = &run->code[pc - 1];
    if (vsp < 1) { nv_fail_because(run, "tup-r-num", 0); return 0; }
    NvTerm l = vals[--vsp];
    NV_DEFOREST_PAIR(l, nv_num(op->ext));
    u32 loc = nv_alloc(run);
    if (run->failed) return 0;
    run->heap_l[loc] = l;
    run->heap_r[loc] = nv_num(op->ext);
    vals[vsp++] = loc;
    NV_NEXT();
  }

do_tup_l_var: {
    NvOp *op = &run->code[pc - 1];
    if (vsp < 1) { nv_fail_because(run, "tup-l-var", 0); return 0; }
    NvTerm r = vals[--vsp];
    NvTerm l = ctx[frm + op->ext];
    NV_DEFOREST_PAIR(l, r);
    u32 loc = nv_alloc(run);
    if (run->failed) return 0;
    run->heap_l[loc] = l;
    run->heap_r[loc] = r;
    vals[vsp++] = loc;
    NV_NEXT();
  }

do_tup_r_var: {
    NvOp *op = &run->code[pc - 1];
    if (vsp < 1) { nv_fail_because(run, "tup-r-var", 0); return 0; }
    NvTerm l = vals[--vsp];
    NvTerm r = ctx[frm + op->ext];
    NV_DEFOREST_PAIR(l, r);
    u32 loc = nv_alloc(run);
    if (run->failed) return 0;
    run->heap_l[loc] = l;
    run->heap_r[loc] = r;
    vals[vsp++] = loc;
    NV_NEXT();
  }

do_rec_fn: {
    NvOp *op = &run->code[pc - 1];
    if (vsp == 0 || csp >= NV_CALL_CAP || op->ext >= run->fn_count) { nv_fail(run); return 0; }
    run->itrs++;
    calls[csp++] = (NvFrame){pc, frm, cur, fun};
    term = vals[--vsp];
    fun = op->ext;
    pc = run->fn_start[fun];
    frm = cur;
    tsp = 0;
    NV_NEXT();
  }

do_tai_fn: {
    NvOp *op = &run->code[pc - 1];
    if (vsp == 0 || op->ext >= run->fn_count) { nv_fail(run); return 0; }
    run->itrs++;
    term = vals[--vsp];
    fun = op->ext;
    pc = run->fn_start[fun];
    cur = frm;
    tsp = 0;
    NV_NEXT();
  }

do_end: {
    if (vsp == 0) { nv_fail(run); return 0; }
    NvTerm val = vals[--vsp];
    if (csp == 0) return val;
    NvFrame frame = calls[--csp];
    pc = frame.pc;
    frm = frame.frm;
    cur = frame.cur;
    fun = frame.fun;
    vals[vsp++] = val;
    NV_NEXT();
  }
  #undef NV_NEXT
  #undef NV_DEFOREST_PAIR
}

fn void nv_print(NvRun *run, NvTerm term) {
  if (nv_is_num(term)) {
    printf("%u", nv_num_val(term));
    return;
  }
  fputc('#', stdout);
  print_sym_name(stdout, run->pair_ext);
  fputc('{', stdout);
  nv_print(run, run->heap_l[term]);
  fputc(',', stdout);
  nv_print(run, run->heap_r[term]);
  fputc('}', stdout);
}

fn void nv_free(NvRun *run) {
  free(run->code);
  free(run->heap_l);
  free(run->heap_r);
  free(run->heap_refs);
  free(run->vals);
  free(run->terms);
  free(run->collect);
  free(run->ctx);
  free(run->calls);
  free(run->fn_start);
  free(run->fn_name);
  free(run->fn_of_name);
}

fn int nv_alloc_storage(NvRun *run) {
  run->code = (NvOp*)malloc(sizeof(NvOp) * NV_CODE_CAP);
  run->heap_l = (NvTerm*)calloc(NV_HEAP_CAP, sizeof(NvTerm));
  run->heap_r = (NvTerm*)calloc(NV_HEAP_CAP, sizeof(NvTerm));
  run->heap_refs = (u16*)calloc(NV_HEAP_CAP, sizeof(u16));
  run->vals = (NvTerm*)malloc(sizeof(NvTerm) * NV_STACK_CAP);
  run->terms = (NvTerm*)malloc(sizeof(NvTerm) * NV_STACK_CAP);
  run->collect = (NvTerm*)malloc(sizeof(NvTerm) * NV_STACK_CAP);
  run->ctx = (NvTerm*)calloc(NV_CTX_CAP, sizeof(NvTerm));
  run->calls = (NvFrame*)malloc(sizeof(NvFrame) * NV_CALL_CAP);
  run->heap_cur = 1;
  if (run->code == NULL || run->heap_l == NULL || run->heap_r == NULL || run->heap_refs == NULL || run->vals == NULL ||
      run->terms == NULL || run->collect == NULL || run->ctx == NULL || run->calls == NULL) {
    nv_free(run);
    memset(run, 0, sizeof(*run));
    return 0;
  }
  return 1;
}

static int nv_prepare_main(u32 main_id) {
  if (NV_PREPARED) {
    nv_free(&NV_PREPARED_RUN);
    memset(&NV_PREPARED_RUN, 0, sizeof(NV_PREPARED_RUN));
    NV_PREPARED = 0;
  }
  NvRun run = {0};
  if (!nv_alloc_storage(&run)) {
    return 0;
  }
  if (!nv_compile_program(&run, main_id)) {
    nv_free(&run);
    return 0;
  }
  NV_PREPARED_RUN = run;
  NV_PREPARED_MAIN = main_id;
  NV_PREPARED = 1;
  return 1;
}

static int nv_eval_main(u32 main_id, int silent) {
  if (!NV_PREPARED || main_id != NV_PREPARED_MAIN) {
    if (getenv("HVM_NV_TRACE") != NULL) {
      char *name = table_get(main_id);
      fprintf(stderr, "[nv] compile fallback: def=%s reason=not-prepared tag=%u\n",
        name != NULL ? name : "?",
        main_id);
    }
    return 0;
  }
  NvRun run = NV_PREPARED_RUN;
  memset(&NV_PREPARED_RUN, 0, sizeof(NV_PREPARED_RUN));
  NV_PREPARED = 0;
  run.itrs = 0;
  run.failed = 0;
  run.fail_reason = NULL;
  run.fail_tag = 0;
  run.trace_pc = 0;
  run.trace_op = 0;
  NvTerm res = nv_run(&run);
  if (run.failed) {
    if (getenv("HVM_NV_TRACE") != NULL) {
      fprintf(stderr, "[nv] run fallback: reason=%s tag=%u pc=%u op=%u itrs=%llu\n",
        run.fail_reason != NULL ? run.fail_reason : "?",
        run.fail_tag,
        run.trace_pc,
        run.trace_op,
        (unsigned long long)run.itrs);
    }
    nv_free(&run);
    return 0;
  }
  if (!silent) {
    nv_print(&run, res);
    putchar('\n');
  }
  if (ITRS_ENABLED) {
    ITRS += run.itrs;
  }
  nv_free(&run);
  return 1;
}

// Runtime Main Evaluator
// ======================
// Runs one top-level entrypoint using shared CLI evaluation behavior.

// Forward declarations
// --------------------

fn Term eval_normalize(Term term);
fn void eval_collapse(Term root, int limit, int stats, int silent);
fn void wnf_set_itrs_enabled(int enabled);

// Runtime Eval Main
// -----------------

// Evaluates one entrypoint and prints output/stats according to cfg.
fn void runtime_eval_main(u32 main_id, const RuntimeEvalCfg *cfg) {
  RuntimeEvalCfg run = {
    .do_collapse   = 0,
    .collapse_limit = -1,
    .stats         = 0,
    .silent        = 0,
    .step_by_step  = 0,
  };

  if (cfg != NULL) {
    run = *cfg;
  }

  int enable_itrs = run.stats || run.silent || run.step_by_step;
  wnf_set_itrs_enabled(enable_itrs);

  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  Term main_ref = term_new_ref(main_id);
  if (run.do_collapse) {
    eval_collapse(main_ref, run.collapse_limit, run.stats, run.silent);
  } else {
    int done = 0;
    if (!run.step_by_step && run.silent) {
      done = nv_eval_main(main_id, run.silent);
      if (!done) {
        done = neo_eval_main(main_id, run.silent);
      }
    }
    if (!done) {
      Term result;
      result = eval_normalize(main_ref);
      if (!run.silent && !run.step_by_step) {
        print_term(result);
        printf("\n");
      }
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &end);

  u64 total_itrs = wnf_itrs_total();
  if (run.stats) {
    double dt = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double ips = total_itrs / dt;
    u64 total_heap = heap_alloc_total();

    printf("- Itrs: %llu interactions\n", (unsigned long long)total_itrs);
    printf("- Heap: %llu nodes\n", (unsigned long long)total_heap);
    printf("- Time: %.3f seconds\n", dt);
    printf("- Perf: %.2f M interactions/s\n", ips / 1e6);
  } else if (run.silent) {
    printf("- Itrs: %llu interactions\n", (unsigned long long)total_itrs);
  }
}

#ifdef HVM_PROFILE
static const char *PROFILE_TAG_NAMES[TAG_MASK + 1] = {
  [APP] = "APP", [VAR] = "VAR", [LAM] = "LAM", [DP0] = "DP0", [DP1] = "DP1",
  [SUP] = "SUP", [DUP] = "DUP", [ALO] = "ALO", [REF] = "REF", [NAM] = "NAM",
  [DRY] = "DRY", [ERA] = "ERA", [MAT] = "MAT", [C00] = "C00", [C01] = "C01",
  [C02] = "C02", [C03] = "C03", [C04] = "C04", [C05] = "C05", [C06] = "C06",
  [C07] = "C07", [C08] = "C08", [C09] = "C09", [C10] = "C10", [C11] = "C11",
  [C12] = "C12", [C13] = "C13", [C14] = "C14", [C15] = "C15", [C16] = "C16",
  [NUM] = "NUM", [SWI] = "SWI", [USE] = "USE", [OP2] = "OP2", [DSU] = "DSU",
  [DDU] = "DDU", [EQL] = "EQL", [AND] = "AND", [OR] = "OR", [UNS] = "UNS",
  [ANY] = "ANY", [INC] = "INC", [BJV] = "BJV", [BJ0] = "BJ0", [BJ1] = "BJ1",
  [F_OP2_NUM] = "F_OP2_NUM", [F_EQL_L] = "F_EQL_L", [F_EQL_R] = "F_EQL_R",
  [F_ALO_MAT] = "F_ALO_MAT",
};

fn const char *profile_tag_name(u32 tag) {
  const char *name = tag <= TAG_MASK ? PROFILE_TAG_NAMES[tag] : NULL;
  return name != NULL ? name : "?";
}

fn void profile_print_counts(const char *name, u64 *counts) {
  fprintf(stderr, "[profile] %s", name);
  for (u32 i = 0; i <= TAG_MASK; i++) {
    if (counts[i] != 0) {
      fprintf(stderr, " %s=%llu", profile_tag_name(i), (unsigned long long)counts[i]);
    }
  }
  fprintf(stderr, "\n");
}

fn void profile_print(void) {
  profile_print_counts("enter", PROF_ENTER);
  profile_print_counts("frame", PROF_FRAME);
  profile_print_counts("alo_book", PROF_ALO_BOOK);
  profile_print_counts("alo_closed", PROF_ALO_BOOK_CLOSED);
  profile_print_counts("alo_open", PROF_ALO_BOOK_OPEN);
  profile_print_counts("app_whnf", PROF_APP_WHNF);
  profile_print_counts("mat_whnf", PROF_MAT_WHNF);
  fprintf(stderr, "[profile] alloc");
  for (u32 i = 0; i <= HEAP_FREE_MAX; i++) {
    if (PROF_ALLOC[i] != 0) {
      fprintf(stderr, " %u=%llu", i, (unsigned long long)PROF_ALLOC[i]);
    }
  }
  fprintf(stderr, "\n[profile] free");
  for (u32 i = 0; i <= HEAP_FREE_MAX; i++) {
    if (PROF_FREE[i] != 0) {
      fprintf(stderr, " %u=%llu", i, (unsigned long long)PROF_FREE[i]);
    }
  }
  fprintf(stderr, "\n");
}
#endif

// Data
// ====

// Bitset for non-zero u64 keys.
//
// Context
// - Used by normalization to track visited heap locations.
// - One bit per heap location.

typedef struct {
  u64 *words;
  u64 word_count;
} Uset;

// Initialize bitset storage.
fn void uset_init(Uset *set) {
  set->word_count = HEAP_CAP >> 6;
  set->words      = NULL;
  if (set->word_count == 0 || set->word_count > ((u64)SIZE_MAX / sizeof(u64))) {
    fprintf(stderr, "uset: allocation failed\n");
    exit(1);
  }
  size_t bytes = (size_t)(set->word_count * sizeof(u64));
  void  *map   = sys_mmap_anon(bytes);
  if (map == NULL) {
    fprintf(stderr, "uset: allocation failed\n");
    exit(1);
  }
  set->words = (u64 *)map;
}

// Release storage and reset state.
fn void uset_free(Uset *set) {
  if (set->words) {
    size_t bytes = (size_t)(set->word_count * sizeof(u64));
    sys_munmap_anon((void *)set->words, bytes);
  }
  *set = (Uset){0};
}

// Insert key if missing; returns 1 if inserted, 0 if already present.
fn u8 uset_add(Uset *set, u64 key) {
  u64 word_idx = key >> 6;
  if (__builtin_expect(word_idx >= set->word_count, 0)) {
    return 0;
  }
  u64 bit_mask = 1ull << (key & 63u);
  u64 prev = set->words[word_idx];
  set->words[word_idx] = prev | bit_mask;
  return (prev & bit_mask) == 0;
}

// Clear all visited bits.
fn void uset_clear(Uset *set) {
  memset(set->words, 0, (size_t)(set->word_count * sizeof(u64)));
}

// CNF
// ===

// CNF (collapsed normal form) step.
// Reduces to WNF, then lifts the first SUP to the top.
// Output is either SUP/ERA/INC at the root with arbitrary fields, or a term
// with no SUP/ERA/INC at any position. ERA propagates upward.

fn Term cnf_at(Term term, u32 depth) {
  term = wnf(term);

  switch (term_tag(term)) {
    case ERA:
    case REF:
    case NUM:
    case NAM:
    case BJV:
    case BJ0:
    case BJ1: {
      return term;
    }

    case SUP:
    case INC: {
      return term;
    }

    case LAM: {
      u64  lam_loc = term_val(term);
      Term body    = heap_read(lam_loc);
      u32  level   = depth + 1;
      heap_subst_var(lam_loc, term_new(0, BJV, 0, level));
      Term body_collapsed = cnf_at(body, level);
      u64  body_loc = heap_alloc(1);
      heap_set(body_loc, body_collapsed);
      Term lam = term_new(0, LAM, level, body_loc);

      u8 body_tag = term_tag(body_collapsed);
      if (body_tag == ERA) {
        return term_new_era();
      }

      if (body_tag == INC) {
        u64 inc_loc = term_val(body_collapsed);
        heap_set(body_loc, heap_read(inc_loc));
        return term_new_inc(lam);
      }

      if (body_tag != SUP) {
        return lam;
      }

      u32  lab     = term_ext(body_collapsed);
      u64  sup_loc = term_val(body_collapsed);
      Term sup_a   = heap_read(sup_loc + 0);
      Term sup_b   = heap_read(sup_loc + 1);

      u64 loc0 = heap_alloc(1);
      u64 loc1 = heap_alloc(1);
      heap_set(loc0, sup_a);
      heap_set(loc1, sup_b);

      Term lam0 = term_new(0, LAM, level, loc0);
      Term lam1 = term_new(0, LAM, level, loc1);

      return term_new_sup(lab, lam0, lam1);
    }

    case DUP:
    case APP:
    case DRY:
    case MAT:
    case SWI:
    case USE:
    case OP2:
    case DSU:
    case DDU:
    case EQL:
    case AND:
    case OR:
    case UNS:
    case C01 ... C16: {
      u32 ari = term_arity(term);
      u64 loc = term_val(term);

      int  sup_idx = -1;
      Term children[16];

      for (u32 i = 0; i < ari; i++) {
        Term child = heap_read(loc + i);
        children[i] = cnf_at(child, depth);
        if (children[i] != child) {
          heap_set(loc + i, children[i]);
        }

        if (term_tag(children[i]) == ERA) {
          return term_new_era();
        }

        if (sup_idx < 0 && term_tag(children[i]) == SUP) {
          sup_idx = (int)i;
        }
      }

      if (sup_idx < 0) {
        return term;
      }

      Term sup     = children[sup_idx];
      u32  lab     = term_ext(sup);
      u64  sup_loc = term_val(sup);
      Term sup_a   = heap_read(sup_loc + 0);
      Term sup_b   = heap_read(sup_loc + 1);

      Term args0[16];
      Term args1[16];

      for (u32 i = 0; i < ari; i++) {
        if ((int)i == sup_idx) {
          args0[i] = sup_a;
          args1[i] = sup_b;
        } else {
          Copy c = term_clone(lab, children[i]);
          args0[i] = c.k0;
          args1[i] = c.k1;
        }
      }

      Term node0 = term_new_at(loc, term_tag(term), term_ext(term), ari, args0);
      Term node1 = term_new_(term_tag(term), term_ext(term), ari, args1);

      return term_new_sup(lab, node0, node1);
    }

    default: {
      return term;
    }
  }
}

fn Term cnf(Term term) {
  return cnf_at(term, 0);
}

// Eval
// ====

// Sequential normalization (SNF) traversal.
// WNFs every reachable heap node, using a visited set to avoid revisiting
// shared nodes and an explicit stack to avoid recursive traversal.

#ifndef EVAL_NORMALIZE_STACK_INIT
#define EVAL_NORMALIZE_STACK_INIT 4096u
#endif

typedef struct {
  u64 *data;
  u64  len;
  u64  cap;
} EvalNormalizeStack;

fn void eval_normalize_stack_init(EvalNormalizeStack *stack) {
  stack->cap  = EVAL_NORMALIZE_STACK_INIT;
  stack->len  = 0;
  stack->data = malloc(stack->cap * sizeof(u64));
  if (stack->data == NULL) {
    fprintf(stderr, "eval_normalize: stack allocation failed\n");
    exit(1);
  }
}

fn void eval_normalize_stack_free(EvalNormalizeStack *stack) {
  free(stack->data);
  *stack = (EvalNormalizeStack){0};
}

fn void eval_normalize_stack_push(EvalNormalizeStack *stack, u64 loc) {
  if (loc == 0) {
    return;
  }
  if (stack->len == stack->cap) {
    u64 next_cap = stack->cap * 2;
    u64 *next = realloc(stack->data, next_cap * sizeof(u64));
    if (next == NULL) {
      fprintf(stderr, "eval_normalize: stack growth failed\n");
      exit(1);
    }
    stack->data = next;
    stack->cap  = next_cap;
  }
  stack->data[stack->len++] = loc;
}

fn int eval_normalize_stack_pop(EvalNormalizeStack *stack, u64 *loc) {
  if (stack->len == 0) {
    return 0;
  }
  *loc = stack->data[--stack->len];
  return 1;
}

fn void eval_normalize_go(Uset *seen, EvalNormalizeStack *stack, u64 loc) {
  for (;;) {
    if (loc == 0 || !uset_add(seen, loc)) {
      return;
    }

    Term term = __builtin_expect(STEPS_ENABLE, 0) ? wnf_steps_at(loc) : wnf_at(loc);

    u64 tloc = term_val(term);
    u8  tag  = term_tag(term);
    if (tag == DP0 || tag == DP1) {
      loc = tloc;
      continue;
    }

    u32 ari = term_arity(term);
    if (ari == 0) {
      return;
    }

    for (u32 i = ari; i > 1; i--) {
      eval_normalize_stack_push(stack, tloc + (i - 1));
    }
    loc = tloc;
  }
}

fn Term eval_normalize(Term term) {
  u64 root_loc = heap_alloc(1);
  heap_set(root_loc, term);

  if (STEPS_ENABLE) {
    STEPS_ROOT_LOC = root_loc;
    if (!SILENT) {
      print_term(heap_read(root_loc));
      printf("\n");
    }
  }

  Uset seen;
  EvalNormalizeStack stack;
  uset_init(&seen);
  eval_normalize_stack_init(&stack);
  uset_clear(&seen);
  eval_normalize_stack_push(&stack, root_loc);

  u64 loc = 0;
  while (eval_normalize_stack_pop(&stack, &loc)) {
    eval_normalize_go(&seen, &stack, loc);
  }

  eval_normalize_stack_free(&stack);
  uset_free(&seen);

  if (STEPS_ENABLE) {
    STEPS_ROOT_LOC = 0;
  }

  return heap_read(root_loc);
}

// Sequential collapse (CNF flattening).
// Lower numeric keys are popped first. INC decreases key, SUP increases key.

#ifndef EVAL_COLLAPSE_QUEUE_INIT
#define EVAL_COLLAPSE_QUEUE_INIT 1024u
#endif

typedef struct {
  u32 key;
  u64 seq;
  u64 loc;
} EvalCollapseTask;

typedef struct {
  EvalCollapseTask *data;
  u64 len;
  u64 cap;
  u64 next_seq;
} EvalCollapseQueue;

fn int eval_collapse_task_lt(EvalCollapseTask a, EvalCollapseTask b) {
  if (a.key != b.key) {
    return a.key < b.key;
  }
  return a.seq < b.seq;
}

fn void eval_collapse_queue_init(EvalCollapseQueue *queue) {
  queue->cap      = EVAL_COLLAPSE_QUEUE_INIT;
  queue->len      = 0;
  queue->next_seq = 0;
  queue->data     = malloc(queue->cap * sizeof(EvalCollapseTask));
  if (queue->data == NULL) {
    fprintf(stderr, "eval_collapse: queue allocation failed\n");
    exit(1);
  }
}

fn void eval_collapse_queue_free(EvalCollapseQueue *queue) {
  free(queue->data);
  *queue = (EvalCollapseQueue){0};
}

fn void eval_collapse_queue_push(EvalCollapseQueue *queue, u32 key, u64 loc) {
  if (loc == 0) {
    return;
  }
  if (queue->len == queue->cap) {
    u64 next_cap = queue->cap * 2;
    EvalCollapseTask *next = realloc(queue->data, next_cap * sizeof(EvalCollapseTask));
    if (next == NULL) {
      fprintf(stderr, "eval_collapse: queue growth failed\n");
      exit(1);
    }
    queue->data = next;
    queue->cap  = next_cap;
  }

  u64 idx = queue->len++;
  queue->data[idx] = (EvalCollapseTask){
    .key = key,
    .seq = queue->next_seq++,
    .loc = loc,
  };

  while (idx > 0) {
    u64 parent = (idx - 1) / 2;
    if (!eval_collapse_task_lt(queue->data[idx], queue->data[parent])) {
      break;
    }
    EvalCollapseTask tmp = queue->data[idx];
    queue->data[idx] = queue->data[parent];
    queue->data[parent] = tmp;
    idx = parent;
  }
}

fn int eval_collapse_queue_pop(EvalCollapseQueue *queue, EvalCollapseTask *task) {
  if (queue->len == 0) {
    return 0;
  }

  *task = queue->data[0];
  EvalCollapseTask last = queue->data[--queue->len];
  if (queue->len == 0) {
    return 1;
  }

  queue->data[0] = last;
  u64 idx = 0;
  for (;;) {
    u64 left = idx * 2 + 1;
    if (left >= queue->len) {
      break;
    }
    u64 right = left + 1;
    u64 best = left;
    if (right < queue->len && eval_collapse_task_lt(queue->data[right], queue->data[left])) {
      best = right;
    }
    if (!eval_collapse_task_lt(queue->data[best], queue->data[idx])) {
      break;
    }
    EvalCollapseTask tmp = queue->data[idx];
    queue->data[idx] = queue->data[best];
    queue->data[best] = tmp;
    idx = best;
  }

  return 1;
}

fn void eval_collapse_process(EvalCollapseQueue *queue, EvalCollapseTask task, u64 *printed, u64 limit, int show_itrs, int silent) {
  Term before = heap_read(task.loc);
  u32  key    = task.key;

  for (;;) {
    Term t = cnf(before);

    switch (term_tag(t)) {
      case INC: {
        u64 inc_loc = term_val(t);
        before = heap_read(inc_loc);
        if (key > 0) {
          key -= 1;
        }
        continue;
      }

      case SUP: {
        u64 sup_loc = term_val(t);
        eval_collapse_queue_push(queue, key + 1, sup_loc + 0);
        eval_collapse_queue_push(queue, key + 1, sup_loc + 1);
        return;
      }

      case ERA: {
        return;
      }

      default: {
        *printed += 1;
        if (*printed <= limit && !silent) {
          print_term_quoted(t);
          if (show_itrs) {
            printf(" \033[2m#%llu\033[0m", (unsigned long long)wnf_itrs_total());
          }
          printf("\n");
        }
        return;
      }
    }
  }
}

fn void eval_collapse(Term term, int limit, int show_itrs, int silent) {
  if (limit == 0) {
    return;
  }

  u64 max_lines = UINT64_MAX;
  if (limit >= 0) {
    max_lines = (u64)limit;
  }

  u64 root_loc = heap_alloc(1);
  heap_set(root_loc, term);

  EvalCollapseQueue queue;
  eval_collapse_queue_init(&queue);
  eval_collapse_queue_push(&queue, 0, root_loc);

  u64 printed = 0;
  EvalCollapseTask task;
  while (printed < max_lines && eval_collapse_queue_pop(&queue, &task)) {
    eval_collapse_process(&queue, task, &printed, max_lines, show_itrs, silent);
  }

  eval_collapse_queue_free(&queue);
}

// CLI
// ===

// Usage: hvm <file.hvm> [options]
//   -s, --stats: Show statistics
//   -S, --silent: Silent output
//   -D, --step-by-step: Step-by-step reduction trace
//   -d, --debug: Debug mode
//   -C[N], --collapse[=N]: Collapse and flatten, optionally limiting output

typedef struct {
  int stats;
  int silent;
  int do_collapse;
  int collapse_limit;
  int debug;
  int step_by_step;
  int help;
  int version;
  char *file;
} CliOpts;

fn const char *cli_prog_name(const char *argv0) {
  if (argv0 == NULL || argv0[0] == '\0') {
    return "hvm";
  }

  const char *slash = strrchr(argv0, '/');
  if (slash == NULL) {
    return argv0;
  }

  return slash + 1;
}

fn int cli_is_uint(const char *text) {
  if (text == NULL || text[0] == '\0') {
    return 0;
  }

  for (u32 i = 0; text[i] != '\0'; i++) {
    if (text[i] < '0' || text[i] > '9') {
      return 0;
    }
  }

  return 1;
}

fn void cli_print_help_opt(const char *sht, const char *lng, const char *desc) {
  char col[64];

  if (sht == NULL || sht[0] == '\0') {
    snprintf(col, sizeof(col), "      %s", lng);
  } else {
    snprintf(col, sizeof(col), "  %s, %s", sht, lng);
  }

  fprintf(stdout, "%-26s %s\n", col, desc);
}

fn void cli_print_help(const char *argv0) {
  const char *prog = cli_prog_name(argv0);

  fprintf(stdout, "HVM 4.0\n\n");
  fprintf(stdout, "Usage: %s <file.hvm> [options]\n\n", prog);
  fprintf(stdout, "A sequential Interaction Calculus runtime.\n\n");

  fprintf(stdout, "Options:\n");
  cli_print_help_opt("-s", "--stats",        "Show statistics after evaluation");
  cli_print_help_opt("-S", "--silent",       "Suppress result output");
  cli_print_help_opt("-C", "--collapse[=N]", "Collapse superpositions (limit N)");
  cli_print_help_opt("-D", "--step-by-step", "Trace each reduction step");
  cli_print_help_opt("-d", "--debug",        "Enable debug mode");
  cli_print_help_opt("-v", "--version",      "Print version");
  cli_print_help_opt("-h", "--help",         "Show this help message");

  fprintf(stdout, "\nExamples:\n");
  fprintf(stdout, "  %s devs/test/fib.hvm -s\n", prog);
  fprintf(stdout, "  %s devs/test/enum_nat.hvm -C10\n", prog);
}

fn CliOpts parse_opts(int argc, char **argv) {
  CliOpts opts = {
    .stats = 0,
    .silent = 0,
    .do_collapse = 0,
    .collapse_limit = -1,
    .debug = 0,
    .step_by_step = 0,
    .help = 0,
    .version = 0,
    .file = NULL
  };

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      opts.help = 1;
    } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      opts.version = 1;
    } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stats") == 0) {
      opts.stats = 1;
    } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--silent") == 0) {
      opts.silent = 1;
    } else if (strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--step-by-step") == 0) {
      opts.step_by_step = 1;
    } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
      opts.debug = 1;
    } else if (strncmp(argv[i], "-C", 2) == 0) {
      opts.do_collapse = 1;
      if (argv[i][2] != '\0') {
        opts.collapse_limit = atoi(&argv[i][2]);
      }
    } else if (strcmp(argv[i], "--collapse") == 0 || strncmp(argv[i], "--collapse=", 11) == 0) {
      opts.do_collapse = 1;
      if (argv[i][10] == '=') {
        const char *num = argv[i] + 11;
        if (!cli_is_uint(num)) {
          fprintf(stderr, "Error: invalid collapse limit '%s'\n", num);
          exit(1);
        }
        opts.collapse_limit = atoi(num);
      } else if (i + 1 < argc && cli_is_uint(argv[i + 1])) {
        opts.collapse_limit = atoi(argv[++i]);
      }
    } else if (argv[i][0] != '-') {
      if (opts.file == NULL) {
        opts.file = argv[i];
      } else {
        fprintf(stderr, "Error: multiple input files specified\n");
        exit(1);
      }
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      exit(1);
    }
  }

  return opts;
}

int main(int argc, char **argv) {
  if (argc <= 1) {
    cli_print_help(argv[0]);
    return 0;
  }

  CliOpts opts = parse_opts(argc, argv);

  if (opts.version) {
    fprintf(stdout, "4.0\n");
    return 0;
  }

  if (opts.help) {
    cli_print_help(argv[0]);
    return 0;
  }

  if (opts.file == NULL) {
    fprintf(stderr, "Error: missing input file\n");
    fprintf(stderr, "Run '%s --help' for usage.\n", cli_prog_name(argv[0]));
    return 1;
  }

  if (opts.step_by_step && opts.do_collapse) {
    fprintf(stderr, "Error: -D is not supported with -C\n");
    return 1;
  }

  runtime_init(opts.debug, opts.silent, opts.step_by_step);

  char *src = sys_file_read(opts.file);
  if (!src) {
    fprintf(stderr, "Error: could not open '%s'\n", opts.file);
    runtime_free();
    return 1;
  }

  char *abs_path = realpath(opts.file, NULL);
  const char *src_path = abs_path ? abs_path : opts.file;
  u32 main_id = 0;
  if (!runtime_prepare(&main_id, src_path, src)) {
    free(src);
    free(abs_path);
    runtime_free();
    return 1;
  }

  free(src);

  RuntimeEvalCfg eval_cfg = {
    .do_collapse   = opts.do_collapse,
    .collapse_limit = opts.collapse_limit,
    .stats         = opts.stats,
    .silent        = opts.silent,
    .step_by_step  = opts.step_by_step,
  };
  runtime_eval_main(main_id, &eval_cfg);
#ifdef HVM_PROFILE
  profile_print();
#endif

  free(abs_path);
  runtime_free();

  return 0;
}
