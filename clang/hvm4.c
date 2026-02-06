#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sched.h>
#include <stdatomic.h>
#include <pthread.h>
#include <assert.h>

// Busy-wait hint
#if defined(__aarch64__)
#define cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__x86_64__)
#define cpu_relax() __asm__ __volatile__("pause")
#else
#define cpu_relax() ((void)0)
#endif

// Types
// =====

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef const char *str;

#define CACHE_L1 128
typedef struct {
  _Alignas(CACHE_L1) _Atomic u64 v;
  char _pad[CACHE_L1 - sizeof(_Atomic u64)];
} CachePaddedAtomic;

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
#define RED 36  // Red(f, g): guarded reduction, f ~> g
#define EQL 37  // Eql(a, b): structural equality, strict on a, then b
#define AND 38  // And(a, b): short-circuit AND, strict on a only
#define OR  39  // Or(a, b): short-circuit OR, strict on a only
#define UNS 40  // Unscoped(xf, xv): binds an unscoped lambda/var pair to xf and xv
#define ANY 41  // Any: wildcard that duplicates itself and equals anything
#define INC 42  // Inc(x): priority wrapper for collapse ordering - decreases priority
#define BJV 43  // Bjv(n): quoted lambda-bound variable (de Bruijn level)
#define BJ0 44  // Bj0(n): quoted dup-bound variable (side 0, de Bruijn level)
#define BJ1 45  // Bj1(n): quoted dup-bound variable (side 1, de Bruijn level)
#define PRI 46  // Pri(p): primitive (native) function

// LAM Ext Flags
// =============
#define LAM_ERA_MASK 0x800000  // binder unused in lambda body

// Stack frame tags (0x40+) - internal to WNF, encode reduction state
// Note: regular term tags (APP, MAT, USE, DP0, DP1, OP2, DSU, DDU) also used as frames
// These frames reuse existing heap nodes to avoid allocation
#define F_APP_RED     0x40  // ((f ~> □) x): val=app_loc. RED at HEAP[app_loc], arg at HEAP[app_loc+1]
#define F_RED_MAT     0x41  // ((f ~> mat) □): val=app_loc. After reducing g, mat stored at HEAP[red_loc+1]
#define F_RED_USE     0x42  // ((f ~> use) □): val=app_loc. After reducing g, use stored at HEAP[red_loc+1]
#define F_OP2_NUM     0x43  // (x op □): ext=opr, val=x_num_val
#define F_EQL_L       0x44  // (□ === b): val=eql_loc, b at HEAP[eql_loc+1]
#define F_EQL_R       0x45  // (a === □): val=eql_loc, a stored in ext as heap loc

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

// Nick-encoded primitive names
#define NAM_ADD 4356
#define NAM_SUB 79170
#define NAM_MUL 54604
#define NAM_DIV 16982
#define NAM_MOD 54212
#define NAM_AND 4996
#define NAM_OR  978
#define NAM_XOR 99282
#define NAM_LSH 50376
#define NAM_RSH 74952
#define NAM_NOT 58324
#define NAM_EQ  337
#define NAM_NE  901
#define NAM_LT  788
#define NAM_LE  773
#define NAM_GT  468
#define NAM_GE  453
#define NAM_DUP 17744
#define NAM_SUP 79184

// Bit Layout
// ==========

#define SUB_BITS 1
#define TAG_BITS 7
#define EXT_BITS 24
#define VAL_BITS 32

#define SUB_SHIFT 63
#define TAG_SHIFT 56
#define EXT_SHIFT 32
#define VAL_SHIFT 0

#define SUB_MASK 0x1
#define TAG_MASK 0x7F
#define EXT_MASK 0xFFFFFF
#define VAL_MASK 0xFFFFFFFF

// Capacities
// ==========

#define HEAP_CAP (1ULL << 32)
#define BOOK_CAP (1ULL << 24)
#define WNF_CAP  (1ULL << 32)
#define MAX_THREADS 64

// Thread Globals
// ==============

static u32 THREAD_COUNT = 1;

#include "thread/get_count.c"
#include "thread/set_count.c"

// Heap Globals
// ============

static Term    *HEAP;
#define HEAP_STRIDE 32
static u64      HEAP_NEXT[MAX_THREADS * HEAP_STRIDE] __attribute__((aligned(256))) = {0};
static u64      HEAP_END[MAX_THREADS * HEAP_STRIDE] __attribute__((aligned(256))) = {0};
#define HEAP_NEXT_AT(t) HEAP_NEXT[(t) * HEAP_STRIDE]
#define HEAP_END_AT(t)  HEAP_END[(t) * HEAP_STRIDE]

// Book Globals
// ============

static u32 *BOOK;

// WNF Globals
// ===========

typedef struct __attribute__((aligned(256))) {
  Term *stack;
  u64   stack_bytes;
  u32   s_pos;
  u8    stack_mmap;
} WnfBank;

static WnfBank WNF_BANKS[MAX_THREADS] = {{0}};

typedef struct __attribute__((aligned(256))) {
  u64 itrs;
  u8  _pad[256 - sizeof(u64)];
} WnfItrsBank;

static WnfItrsBank WNF_ITRS_BANKS[MAX_THREADS] = {{0}};
static _Thread_local WnfBank *WNF_BANK = NULL;
static _Thread_local u64 *WNF_ITRS_PTR = NULL;
static _Thread_local u32 WNF_TID = 0;
#define WNF_STACK (WNF_BANK->stack)
#define WNF_S_POS (WNF_BANK->s_pos)
#define ITRS (*WNF_ITRS_PTR)
#define ITRS_INC(name) \
  do { \
    if (__builtin_expect(STEPS_ITRS_LIM != 0, 0)) { \
      STEPS_LAST_ITR = (name); \
    } \
    ITRS++; \
  } while (0)
static u32 FRESH = 1;

static int DEBUG          = 0;
static int SILENT         = 0;
static int STEPS_ENABLE   = 0;
static u64 STEPS_ITRS_LIM = 0;
static u32 STEPS_ROOT_LOC = 0;
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
} PBind;

// Parser Globals
// ==============

static char  *PARSE_SEEN_FILES[1024];
static u32    PARSE_SEEN_FILES_LEN = 0;
static PBind  PARSE_BINDS[16384];
static u32    PARSE_BINDS_LEN = 0;
static u32    PARSE_FRESH_LAB = 0x800000; // start at 2^23 to avoid collision with user labels
static int    PARSE_FORK_SIDE = -1;      // -1 = off, 0 = left branch (DP0), 1 = right branch (DP1)

// Term
// ====
#include "term/new.c"
#include "term/sub/get.c"
#include "term/sub/set.c"
#include "term/tag.c"
#include "term/ext.c"
#include "term/val.c"
#include "term/arity.c"

// Heap
// ====

#include "heap/alloc.c"
#include "heap/read.c"
#include "heap/take.c"
#include "heap/set.c"
#include "heap/init_slices.c"

// Term Constructors
// =================

#include "term/new/_.c"
#include "term/new/nam.c"
#include "term/new/dry.c"
#include "term/new/var.c"
#include "term/new/ref.c"
#include "term/new/pri.c"
#include "term/new/era.c"
#include "term/new/any.c"
#include "term/new/dp0.c"
#include "term/new/dp1.c"
#include "term/new/lam.c"
#include "term/new/app.c"
#include "term/new/sup.c"
#include "term/new/dup.c"
#include "term/new/mat.c"
#include "term/new/swi.c"
#include "term/new/use.c"
#include "term/new/ctr.c"
#include "term/new/op2.c"
#include "term/new/dsu.c"
#include "term/new/ddu.c"
#include "term/new/red.c"
#include "term/new/eql.c"
#include "term/new/and.c"
#include "term/new/or.c"
#include "term/new/uns.c"
#include "term/new/inc.c"
#include "term/new/num.c"
#include "term/clone.c"

// Heap Substitution
// =================

#include "heap/subst_var.c"
#include "heap/subst_cop.c"

// Nick
// ====

#include "nick/letter_to_b64.c"
#include "nick/b64_to_letter.c"
#include "nick/to_str.c"
#include "nick/is_init.c"
#include "nick/is_char.c"
#include "nick/from_str.c"
#include "nick/names.c"

// System
// ======

#include "sys/error.c"
#include "sys/runtime_error.c"
#include "sys/path_join.c"
#include "sys/file_read.c"

// Table
// =====

#include "table/_.c"
#include "table/find.c"
#include "table/get.c"

// Print
// =====

#include "print/name.c"
#include "print/utf8.c"
#include "prim/register.c"
#include "prim/fn/log.c"
#include "prim/fn/log_go_0.c"
#include "prim/fn/log_go_1.c"
#include "prim/fn/log_go_2.c"
#include "prim/init.c"
#include "print/term.c"

// FFI
// ===

#include "ffi/api.c"
#include "ffi/load.c"
#include "ffi/load_dir.c"

// Parse
// =====

#include "parse/error.c"
#include "parse/error_var.c"
#include "parse/error_affine.c"
#include "parse/at_end.c"
#include "parse/peek_at.c"
#include "parse/peek.c"
#include "parse/advance.c"
#include "parse/starts_with.c"
#include "parse/match.c"
#include "parse/is_space.c"
#include "parse/skip_comment.c"
#include "parse/skip.c"
#include "parse/consume.c"
#include "parse/bind_push.c"
#include "parse/bind_pop.c"
#include "parse/bind_lookup.c"
#include "parse/count_uses.c"
#include "parse/auto_dup.c"
#include "parse/name.c"
#include "parse/utf8.c"
#include "parse/term/lam.c"
#include "parse/term/dup.c"
#include "parse/term/fork.c"
#include "parse/term/sup.c"
#include "parse/term/ctr.c"
#include "parse/term/ref.c"
#include "parse/term/pri.c"
#include "parse/term/nam.c"
#include "parse/term/par.c"
#include "parse/term/num.c"
#include "parse/term/nat.c"
#include "parse/term/chr.c"
#include "parse/term/str.c"
#include "parse/term/lst.c"
#include "parse/term/var.c"
#include "parse/term/any.c"
#include "parse/term/args.c"
#include "parse/term/opr.c"
#include "parse/term/app.c"
#include "parse/term/inc.c"
#include "parse/term/_.c"
#include "parse/include.c"
#include "parse/def.c"

// WNF
// ===

#include "wnf/set_tid.c"
#include "wnf/stack_init.c"
#include "wnf/stack_free.c"
#include "wnf/itrs_total.c"
#include "wnf/itrs_thread.c"
#include "wnf/app_era.c"
#include "wnf/app_nam.c"
#include "wnf/app_dry.c"
#include "wnf/app_lam.c"
#include "wnf/app_sup.c"
#include "wnf/app_inc.c"
#include "wnf/app_mat_sup.c"
#include "wnf/app_mat_ctr.c"
#include "wnf/app_mat_num.c"
#include "wnf/mat_inc.c"
#include "wnf/dup_nam.c"
#include "wnf/dup_dry.c"
#include "wnf/dup_red.c"
#include "wnf/dup_lam.c"
#include "wnf/dup_sup.c"
#include "wnf/dup_nod.c"
#include "wnf/alo_var.c"
#include "wnf/alo_cop.c"
#include "wnf/alo_nam.c"
#include "wnf/alo_dry.c"
#include "wnf/alo_lam.c"
#include "wnf/alo_dup.c"
#include "wnf/alo_nod.c"
#include "wnf/op2_era.c"
#include "wnf/op2_sup.c"
#include "wnf/op2_num_era.c"
#include "wnf/op2_num_num.c"
#include "wnf/op2_num_sup.c"
#include "wnf/op2_inc.c"
#include "wnf/dsu_era.c"
#include "wnf/dsu_num.c"
#include "wnf/dsu_sup.c"
#include "wnf/dsu_inc.c"
#include "wnf/ddu_era.c"
#include "wnf/ddu_num.c"
#include "wnf/ddu_sup.c"
#include "wnf/ddu_inc.c"
#include "wnf/use_era.c"
#include "wnf/use_sup.c"
#include "wnf/use_val.c"
#include "wnf/use_inc.c"
#include "wnf/app_red_era.c"
#include "wnf/app_red_sup.c"
#include "wnf/app_red_inc.c"
#include "wnf/app_red_lam.c"
#include "wnf/app_red_red.c"
#include "wnf/app_red_nam.c"
#include "wnf/app_red_dry.c"
#include "wnf/app_red_ctr.c"
#include "wnf/app_red_mat_era.c"
#include "wnf/app_red_mat_sup.c"
#include "wnf/app_red_mat_inc.c"
#include "wnf/app_red_mat_ctr.c"
#include "wnf/app_red_mat_num.c"
#include "wnf/app_red_use_era.c"
#include "wnf/app_red_use_sup.c"
#include "wnf/app_red_use_inc.c"
#include "wnf/app_red_use_val.c"
#include "wnf/eql_era.c"
#include "wnf/eql_any.c"
#include "wnf/eql_sup.c"
#include "wnf/eql_num.c"
#include "wnf/eql_lam.c"
#include "wnf/eql_ctr.c"
#include "wnf/eql_mat.c"
#include "wnf/eql_use.c"
#include "wnf/eql_nam.c"
#include "wnf/eql_dry.c"
#include "wnf/eql_inc.c"
#include "wnf/and_era.c"
#include "wnf/and_sup.c"
#include "wnf/and_num.c"
#include "wnf/and_inc.c"
#include "wnf/or_era.c"
#include "wnf/or_sup.c"
#include "wnf/or_num.c"
#include "wnf/or_inc.c"
#include "wnf/uns.c"
#include "wnf/pri.c"
#include "wnf/_.c"

// Data
// ====

#include "data/uset.c"
#include "data/wsq.c"
#include "data/wspq.c"

// CNF
// ===

#include "cnf/_.c"

// Eval
// ====

#include "eval/normalize.c"
#include "eval/collapse.c"
