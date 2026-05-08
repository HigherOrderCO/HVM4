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

// LAM Ext Flags
// =============
#define LAM_ERA_MASK 0x800000u  // binder unused in lambda body

// Stack frame tags (0x40+) - internal to WNF, encode reduction state
// Note: regular term tags (APP, MAT, USE, DP0, DP1, OP2, DSU, DDU) also used as frames
// These frames reuse existing heap nodes to avoid allocation
#define F_OP2_NUM     0x43  // (x op □): ext=opr, val=x_num_val
#define F_EQL_L       0x44  // (□ === b): val=eql_loc, b at HEAP[eql_loc+1]
#define F_EQL_R       0x45  // (a === □): val=eql_loc, a stored at HEAP[eql_loc]

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

// Heap Globals
// ============

static Term *HEAP;
static u64   HEAP_NEXT = 1;

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
    if (ITRS_ENABLED != 0) { \
      if (__builtin_expect(STEPS_ITRS_LIM != 0, 0)) { \
        STEPS_LAST_ITR = (name); \
      } \
      ITRS++; \
    } \
  } while (0)
static u32 FRESH = 1;

static int DEBUG          = 0;
static int SILENT         = 0;
static int STEPS_ENABLE   = 0;
static u64 STEPS_ITRS_LIM = 0;
static u64 STEPS_ROOT_LOC = 0;
static str STEPS_LAST_ITR = NULL;

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
};

fn u32 term_arity(Term t) {
  u8 tag = term_tag(t);
  return TERM_ARITY[tag];
}

// Heap
// ====

fn u64 heap_alloc(u64 size) {
  u64 at   = HEAP_NEXT;
  u64 next = at + size;
  if (__builtin_expect(next <= HEAP_CAP && next >= at, 1)) {
    HEAP_NEXT = next;
    return at;
  }
  fprintf(stderr, "Out of heap memory (need %llu words)\n", (unsigned long long)size);
  exit(1);
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
fn Term term_new_dsu(Term lab, Term a, Term b) {
  u64 loc = heap_alloc(3);
  heap_set(loc + 0, lab);
  heap_set(loc + 1, a);
  heap_set(loc + 2, b);
  return term_new(0, DSU, 0, loc);
}

// DynDup(lab, val, bod): dynamic DUP binder, strict on lab
// Layout: heap_read(loc+0) = lab, heap_read(loc+1) = val, heap_read(loc+2) = bod
fn Term term_new_ddu(Term lab, Term val, Term bod) {
  u64 loc = heap_alloc(3);
  heap_set(loc + 0, lab);
  heap_set(loc + 1, val);
  heap_set(loc + 2, bod);
  return term_new(0, DDU, 0, loc);
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
  u32 ts_cap = 1024;
  Term *ts = (Term*)malloc(sizeof(Term) * ts_cap);
  u32 ts_idx = 0;
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
      if (ts_idx == ts_cap) {
        ts_cap *= 2;
        ts = (Term*)realloc(ts, sizeof(Term) * ts_cap);
      }
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
    if (ari == 0) {
      return res;
    }
    u64 ctr_loc = term_val(ctr);
    // Reuse MAT node storage for the first APP in the chain.
    Term arg0 = heap_read(ctr_loc + 0);
    res = term_new_app_at(mat_loc, res, arg0);
    if (ari == 1) {
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
    return res;
  } else {
    ITRS_INC("APP-MAT-CTR-MIS");
    Term g = heap_read(mat_loc + 1);
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
    return heap_read(mat_loc + 0);
  } else {
    ITRS_INC("APP-MAT-NUM-MIS");
    Term g = heap_read(mat_loc + 1);
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
    return book;
  }
  args[0] = term_new_alo_at(alo_loc, ls_loc, len, loc + 0);
  for (u32 i = 1; i < ari; i++) {
    args[i] = term_new_alo(ls_loc, len, loc + i);
  }
  return term_new_(tag, ext, ari, args);
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
fn Term wnf_op2_inc_x(u32 opr, Term inc, Term y) {
  ITRS_INC("OP2-INC-X");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term op      = term_new_op2(opr, x, y);
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
fn Term wnf_dsu_num(Term lab_num, Term a, Term b) {
  ITRS_INC("DSU-NUM");
  u32 lab = term_val(lab_num);
  return term_new_sup(lab, a, b);
}

// &(&L{x,y}){a, b}
// -------------------------- DSU-SUP
// ! A &L = a
// ! B &L = b
// &L{&(x){A₀,B₀}, &(y){A₁,B₁}}
fn Term wnf_dsu_sup(Term lab_sup, Term a, Term b) {
  ITRS_INC("DSU-SUP");
  u32  lab     = term_ext(lab_sup);
  u64  sup_loc = term_val(lab_sup);
  Copy A;
  Copy B;
  term_clone2(lab, a, b, &A, &B);
  Term ds0     = term_new_dsu(heap_read(sup_loc + 0), A.k0, B.k0);
  Term ds1     = term_new_dsu(heap_read(sup_loc + 1), A.k1, B.k1);
  return term_new_sup_at(sup_loc, lab, ds0, ds1);
}

// &(↑x){a, b}
// ------------ DSU-INC
// ↑(&(x){a, b})
fn Term wnf_dsu_inc(Term inc, Term a, Term b) {
  ITRS_INC("DSU-INC");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term new_dsu = term_new_dsu(x, a, b);
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
fn Term wnf_ddu_num(Term lab_num, Term val, Term bod) {
  ITRS_INC("DDU-NUM");
  u32 lab   = term_val(lab_num);
  Copy V    = term_clone(lab, val);
  Term app0 = term_new_app(bod, V.k0);
  Term app1 = term_new_app(app0, V.k1);
  return app1;
}

// ! X &(&L{x,y}) = v; b
// ------------------------------ DDU-SUP
// ! V &L = v
// ! B &L = b
// &L{! X &(x) = V₀; B₀, ! X &(y) = V₁; B₁}
fn Term wnf_ddu_sup(Term lab_sup, Term val, Term bod) {
  ITRS_INC("DDU-SUP");
  u32  lab     = term_ext(lab_sup);
  u64  sup_loc = term_val(lab_sup);
  Copy V;
  Copy B;
  term_clone2(lab, val, bod, &V, &B);
  Term dd0     = term_new_ddu(heap_read(sup_loc + 0), V.k0, B.k0);
  Term dd1     = term_new_ddu(heap_read(sup_loc + 1), V.k1, B.k1);
  return term_new_sup_at(sup_loc, lab, dd0, dd1);
}

// ! X &(↑x) = v; b
// ---------------- DDU-INC
// ↑(! X &(x) = v; b)
fn Term wnf_ddu_inc(Term inc, Term val, Term bod) {
  ITRS_INC("DDU-INC");
  u64  inc_loc = term_val(inc);
  Term x       = heap_read(inc_loc);
  Term new_ddu = term_new_ddu(x, val, bod);
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
fn Term wnf_eql_lam(Term a, Term b) {
  ITRS_INC("EQL-LAM");
  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);
  Term af    = heap_read(a_loc);
  Term bf    = heap_read(b_loc);
  // Generate fresh name for substitution
  u32 fresh = FRESH++;
  Term nam = term_new_nam(fresh);
  // Substitute both variable locations with the same name
  heap_subst_var(a_loc, nam);
  heap_subst_var(b_loc, nam);
  return term_new_eql(af, bf);
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
    return term_new_num(0);
  }

  u32 arity = a_tag - C00;

  // Arity 0: equal
  if (arity == 0) {
    return term_new_num(1);
  }

  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);

  // SUC (1n+): recursive natural - wrap in INC for priority
  if (a_ext == SYM_SUC && arity == 1) {
    Term eq = term_new_eql_at(eql_loc, heap_read(a_loc), heap_read(b_loc));
    return term_new_inc(eq);
  }

  // CON (<>): recursive list - wrap tail and whole in INC
  if (a_ext == SYM_CON && arity == 2) {
    Term eq_h = term_new_eql_at(eql_loc, heap_read(a_loc), heap_read(b_loc));
    Term eq_t = term_new_inc(term_new_eql(heap_read(a_loc + 1), heap_read(b_loc + 1)));
    return term_new_inc(term_new_and(eq_h, eq_t));
  }

  // Other constructors: no INC, just AND chain
  Term result = term_new_eql_at(eql_loc, heap_read(a_loc), heap_read(b_loc));
  for (u32 i = 1; i < arity; i++) {
    Term eq_i = term_new_eql(heap_read(a_loc + i), heap_read(b_loc + i));
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
    return term_new_num(0);
  }

  u64  a_loc = term_val(a);
  u64  b_loc = term_val(b);
  Term ah    = heap_read(a_loc + 0);
  Term am    = heap_read(a_loc + 1);
  Term bh    = heap_read(b_loc + 0);
  Term bm    = heap_read(b_loc + 1);

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

    switch (term_tag(next)) {
      case VAR: {
        u64 loc = term_val(next);
        Term cell = heap_read(loc);
        if (term_sub_get(cell)) {
          next = term_sub_set(cell, 0);
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
        next = body;
        goto enter;
      }

      case UNS: {
        next = wnf_uns(next);
        goto enter;
      }

      case REF: {
        u32 nam = term_ext(next);
        if (BOOK[nam] != 0) {
          next = term_new_alo(0, 0, BOOK[nam]);
          goto enter;
        }
        whnf = next;
        goto apply;
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

        switch (term_tag(book)) {
          case VAR:
          case BJV: {
            next = wnf_alo_var(ls_loc, len, book);
            goto enter;
          }
          case DP0:
          case DP1:
          case BJ0:
          case BJ1: {
            next = wnf_alo_cop(ls_loc, len, book);
            goto enter;
          }
          case LAM: {
            next = wnf_alo_lam(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case DUP: {
            next = wnf_alo_dup(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case APP:
          case DRY:
          case SUP:
          case MAT:
          case SWI:
          case USE:
          case UNS:
          case INC:
          case C00 ... C16:
          case OP2:
          case EQL:
          case AND:
          case OR:
          case DSU:
          case DDU: {
            next = wnf_alo_nod(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case NAM:
          case NUM:
          case REF:
          case ERA:
          case ANY: {
            next = book;
            goto enter;
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

      switch (term_tag(frame)) {
        // -----------------------------------------------------------------------
        // APP frame: (□ x) - we reduced func, now dispatch
        // -----------------------------------------------------------------------
        case APP: {
          u64  app_loc = term_val(frame);
          Term arg     = heap_read(app_loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
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
              stack[s_pos++] = whnf;
              next = arg;
              goto enter;
            }
            case USE: {
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

        // -----------------------------------------------------------------------
        // MAT/SWI frame: (mat □) - we reduced arg, dispatch mat interaction
        // -----------------------------------------------------------------------
        case MAT:
        case SWI: {
          Term mat = frame;
          switch (term_tag(whnf)) {
            case ERA: {
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
              whnf = wnf_op2_era();
              continue;
            }
            case NUM: {
              u8 y_tag = term_tag(y);
              if (y_tag == NUM) {
                whnf = wnf_op2_num_num_raw(opr, (u32)term_val(whnf), (u32)term_val(y));
                continue;
              }
              // x is NUM, now reduce y: push F_OP2_NUM frame
              stack[s_pos++] = term_new(0, F_OP2_NUM, opr, term_val(whnf));
              next = y;
              goto enter;
            }
            case SUP: {
              whnf = wnf_op2_sup(loc, opr, whnf, y);
              continue;
            }
            case INC: {
              whnf = wnf_op2_inc_x(opr, whnf, y);
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
              whnf = wnf_eql_era_l();
              continue;
            }
            case ANY: {
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
              whnf = wnf_eql_era_r();
              continue;
            }
            case ANY: {
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
                whnf = wnf_eql_any_r();
                continue;
              }
              // NUM === NUM
              if (a_tag == NUM && b_tag == NUM) {
                whnf = wnf_eql_num(a, whnf);
                continue;
              }
              // LAM === LAM
              if (a_tag == LAM && b_tag == LAM) {
                next = wnf_eql_lam(a, whnf);
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
              whnf = wnf_dsu_era();
              continue;
            }
            case NUM: {
              whnf = wnf_dsu_num(whnf, a, b);
              continue;
            }
            case SUP: {
              whnf = wnf_dsu_sup(whnf, a, b);
              continue;
            }
            case INC: {
              whnf = wnf_dsu_inc(whnf, a, b);
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
              whnf = wnf_ddu_era();
              continue;
            }
            case NUM: {
              next = wnf_ddu_num(whnf, val, bod);
              goto enter;
            }
            case SUP: {
              whnf = wnf_ddu_sup(whnf, val, bod);
              continue;
            }
            case INC: {
              whnf = wnf_ddu_inc(whnf, val, bod);
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
    Term result = eval_normalize(main_ref);
    if (!run.silent && !run.step_by_step) {
      print_term(result);
      printf("\n");
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

  free(abs_path);
  runtime_free();

  return 0;
}
