// HVM4 CUDA Runtime
// ==================
// GPU evaluator for the Interaction Calculus runtime.
// Shares term operations and interaction rules with the CPU runtime (clang/).
//
// Build:
//   cd cuda
//   clang -O2 -I../clang -o dump dump.c -lpthread
//   nvcc -O2 -arch=sm_89 -I../clang -w -o hvm_cuda hvm.cu
//
// Run:
//   ./dump <file.hvm> prog.bin
//   ./hvm_cuda prog.bin

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cuda_runtime.h>

// ============================================================
// CUDA Compatibility Layer
// ============================================================

#define fn __device__ __forceinline__

#define ITRS_INC(name) do { d_itrs++; } while(0)

// Map GCC atomic used in eql_lam.c to CUDA atomic
#define __ATOMIC_RELAXED 0
#define __atomic_fetch_add(ptr, val, order) atomicAdd(ptr, val)

// ============================================================
// Types
// ============================================================

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

// ============================================================
// Tags
// ============================================================

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
#define SWI 31
#define USE 32
#define OP2 33
#define DSU 34
#define DDU 35
#define EQL 36
#define AND 37
#define OR  38
#define UNS 39
#define ANY 40
#define INC 41
#define BJV 42
#define BJ0 43
#define BJ1 44
#define PRI 45

// Bitmask of tags already in weak head normal form (single-instruction check)
#define WHNF_MASK ( \
  (1ULL << LAM) | (1ULL << SUP) | (1ULL << NAM) | (1ULL << DRY) | \
  (1ULL << ERA) | (1ULL << MAT) | (1ULL << NUM) | (1ULL << SWI) | \
  (1ULL << USE) | (1ULL << INC) | (1ULL << ANY) | \
  (1ULL << BJV) | (1ULL << BJ0) | (1ULL << BJ1) | \
  ((1ULL << (C16 + 1)) - (1ULL << C00)) )

// LAM Ext Flags
#define LAM_ERA_MASK 0x20000

// Stack frame tags (internal to WNF)
#define F_OP2_NUM     0x43
#define F_EQL_L       0x44
#define F_EQL_R       0x45

// Operation codes
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
#define OP_NOT 10
#define OP_EQ  11
#define OP_NE  12
#define OP_LT  13
#define OP_LE  14
#define OP_GT  15
#define OP_GE  16

// ============================================================
// Bit Layout
// ============================================================

#define SUB_BITS 1
#define TAG_BITS 7
#define EXT_BITS 18
#define VAL_BITS 38
#define SUB_SHIFT 63
#define TAG_SHIFT 56
#define EXT_SHIFT 38
#define VAL_SHIFT 0

#define SUB_MASK 0x1
#define TAG_MASK 0x7F
#define EXT_MASK 0x3FFFF
#define VAL_MASK 0x3FFFFFFFFFULL

#define ALO_TM_BITS 24
#define ALO_LS_BITS 38
#define ALO_TM_MASK 0xFFFFFFULL
#define ALO_LS_MASK 0x3FFFFFFFFFULL

// ============================================================
// Device Globals
// ============================================================

__device__ u64 *HEAP;
__device__ u64 *BOOK;
__device__ u32  FRESH = 1;

// Symbol IDs for eql_ctr special cases (set to 0 = unused)
__device__ u32 SYM_SUC = 0;
__device__ u32 SYM_CON = 0;

// Hot per-thread state lives in shared memory (5 cycles vs 200+ for global RMW).
// Layout: 4 banks of 256 u64 slots each.  Block size must be <= 256.
extern __shared__ u64 s_hot[];
#define S_ITRS      0     // per-thread (256 max)
#define S_MAX_DEPTH 256   // per-thread (256 max)
#define S_WARP_HEAP 512   // per-warp (8 max): bump next
#define S_WARP_END  520   // per-warp (8 max): region end
#define S_WARP_BASE 528   // per-warp (8 max): region start (for circular wrap)
#define S_HOT_WORDS 536
#define S_HOT_BYTES (S_HOT_WORDS * sizeof(u64))

#define d_itrs (s_hot[S_ITRS + threadIdx.x])

// Tag → arity lookup table (replaces 40-case switch, hits L1/constant cache)
__device__ const u8 c_arity[48] = {
  2, 0, 1, 0, 0, 2, 2, 0, 0, 0, 2, 0, 2,
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  0, 2, 1, 2, 3, 3, 2, 2, 2, 1, 0, 1, 0, 0, 0, 0, 0, 0
};

// ============================================================
// Term Operations (shared with CPU -- pure bit manipulation)
// ============================================================

#include "term/new.c"
#include "term/tag.c"
#include "term/ext.c"
#include "term/val.c"
#include "term/sub/get.c"
#include "term/sub/set.c"
#include "term/op2_u32.c"

// ============================================================
// Heap Operations (CUDA-specific)
// ============================================================

fn u32 gpu_tid() {
  return threadIdx.x + blockIdx.x * blockDim.x;
}

__device__ __noinline__ u64 heap_alloc_coop(u64 size) {
  unsigned mask = __activemask();
  u32 warp = threadIdx.x >> 5;
  u32 lane = threadIdx.x & 31;
  u32 leader = __ffs(mask) - 1;
  u32 count = __popc(mask);
  u32 rank = __popc(mask & ((1u << lane) - 1));
  u64 leader_size = __shfl_sync(mask, size, leader);
  if (__all_sync(mask, size == leader_size)) {
    u64 base;
    if (lane == leader) {
      base = s_hot[S_WARP_HEAP + warp];
      u64 need = (u64)count * size;
#ifdef CIRCULAR_HEAP
      if (base + need > s_hot[S_WARP_END + warp]) {
        base = s_hot[S_WARP_BASE + warp];
      }
#endif
      s_hot[S_WARP_HEAP + warp] = base + need;
    }
    base = __shfl_sync(mask, base, leader);
    return base + (u64)rank * size;
  }
  return (u64)atomicAdd((unsigned long long*)&s_hot[S_WARP_HEAP + warp],
                        (unsigned long long)size);
}

fn u64 heap_alloc(u64 size) {
  unsigned mask = __activemask();
  if (__popc(mask) == 1) {
    u32 warp = threadIdx.x >> 5;
    u64 at = s_hot[S_WARP_HEAP + warp];
#ifdef CIRCULAR_HEAP
    if (at + size > s_hot[S_WARP_END + warp]) {
      at = s_hot[S_WARP_BASE + warp];
    }
#endif
    s_hot[S_WARP_HEAP + warp] = at + size;
    return at;
  }
  return heap_alloc_coop(size);
}

fn Term heap_read(u64 loc) {
  return HEAP[loc];
}

fn void heap_set(u64 loc, Term val) {
  HEAP[loc] = val;
}

fn Term heap_take(u64 loc) {
  for (;;) {
    Term prev = (Term)atomicExch((unsigned long long*)&HEAP[loc], 0ULL);
    if (prev != 0) return prev;
  }
}

fn void heap_set_rel(u64 loc, Term val) {
  *(volatile u64*)&HEAP[loc] = val;
}

// ============================================================
// Term Constructors
// ============================================================
// term_new_at and term_new_ from clang/ (no compound literals, take Term*)

#include "term/new/_.c"

#include "term/new/num.c"
#include "term/new/era.c"
#include "term/new/any.c"
#include "term/new/var.c"
#include "term/new/ref.c"
#include "term/new/nam.c"
#include "term/new/dp0.c"
#include "term/new/dp1.c"
#include "term/new/alo.c"
#include "term/new/inc.c"
#include "term/new/op2.c"
#include "term/new/dsu.c"
#include "term/new/ddu.c"
#include "term/new/eql.c"
#include "term/new/and.c"
#include "term/new/or.c"
#include "term/new/use.c"
#include "term/new/uns.c"
#include "term/new/swi.c"
#include "term/new/ctr.c"
#include "term/new/pri.c"
#include "term/new/app.c"
#include "term/new/lam.c"
#include "term/new/sup.c"
#include "term/new/dry.c"
#include "term/new/dup.c"
#include "term/new/mat.c"

// ============================================================
// Term Arity (CUDA-specific, replaces designated-initializer version)
// ============================================================

fn u32 term_arity(Term t) {
  return c_arity[term_tag(t)];
}

// ============================================================
// Term Clone & Heap Substitution (shared with CPU)
// ============================================================

#include "term/clone.c"
#include "heap/subst_var.c"
#include "heap/subst_var_dup.c"
#include "heap/subst_cop.c"

// ============================================================
// WNF Interaction Rules (shared with CPU)
// ============================================================
// Hot rules (used in spin/tree hot path) stay __forceinline__.
// Cold rules are __noinline__ to keep wnf() code small for icache.

#include "wnf/app_era.c"
#include "wnf/app_lam.c"
#include "wnf/app_mat_num.c"
#include "wnf/alo_var.c"
#include "wnf/alo_lam.c"
#include "wnf/alo_nod.c"
#include "wnf/op2_num_num.c"
#include "wnf/dup_sup.c"
#include "wnf/dup_nod.c"

#undef fn
#define fn __device__ __noinline__

#include "wnf/app_nam.c"
#include "wnf/app_dry.c"
#include "wnf/app_sup.c"
#include "wnf/app_inc.c"
#include "wnf/app_mat_sup.c"
#include "wnf/app_mat_ctr.c"
#include "wnf/mat_inc.c"
#include "wnf/dup_nam.c"
#include "wnf/dup_lam.c"
#include "wnf/alo_cop.c"
#include "wnf/alo_dup.c"
#include "wnf/op2_era.c"
#include "wnf/op2_sup.c"
#include "wnf/op2_num_era.c"
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

#undef fn
#define fn __device__ __forceinline__

// ============================================================
// WNF Evaluator (CUDA-adapted from clang/wnf/_.c)
// ============================================================

#define WNF_STACK_CAP (1 << 27) // 128M entries per thread (1GB)

__device__ Term *d_wnf_stacks;
__device__ u64   d_wnf_stride;

fn int aot_try_call(u32 id, u32 *s_pos, u32 base, Term *out) {
  return 0;
}

__device__ __noinline__ Term wnf(Term term) {
  u32 tid = gpu_tid();
  u64 nt  = d_wnf_stride;
  u32  s_pos = 0;
  u32  base  = 0;
  u32  max_d = 0;
  Term next  = term;
  Term whnf;

  #define PUSH(v) do { d_wnf_stacks[(u64)s_pos * nt + tid] = (v); s_pos++; } while(0)
  #define POP()   (--s_pos, d_wnf_stacks[(u64)s_pos * nt + tid])

  enter: {
    switch (term_tag(next)) {
      case VAR: {
        u64 loc = term_val(next);
        Term cell = heap_read(loc);
        if (term_sub_get(cell)) {
          next = term_sub_set(cell, 0);
          if ((WHNF_MASK >> term_tag(next)) & 1) { whnf = next; goto apply; }
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
          if ((WHNF_MASK >> term_tag(next)) & 1) { whnf = next; goto apply; }
          goto enter;
        }
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
        next = cell;
        goto enter;
      }

      case APP: {
        u64  loc = term_val(next);
        Term fun = heap_read(loc);
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
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
        Term aot_out;
        if (aot_try_call(nam, &s_pos, base, &aot_out)) {
          next = aot_out;
          goto enter;
        }
        if (BOOK != NULL) {
          u64 bv = __ldg(&BOOK[nam]);
          if (bv != 0) {
            next = term_new_alo(0, 0, bv);
            goto enter;
          }
        }
        whnf = next;
        goto apply;
      }

      case PRI: {
        whnf = next;
        goto apply;
      }

      case ALO: {
        u32 len = term_ext(next);
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
        Term book = heap_read(tm_loc);

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
          case PRI:
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
          default: {
            if (term_tag(book) >= C00 && term_tag(book) <= C16) {
              next = wnf_alo_nod(alo_loc, ls_loc, len, book);
              goto enter;
            }
            next = book;
            goto enter;
          }
        }
      }

      case OP2: {
        u64  loc = term_val(next);
        Term x   = heap_read(loc + 0);
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
        next = x;
        goto enter;
      }

      case EQL: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
        next = a;
        goto enter;
      }

      case AND: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
        next = a;
        goto enter;
      }

      case OR: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
        next = a;
        goto enter;
      }

      case DSU: {
        u64  loc = term_val(next);
        Term lab = heap_read(loc + 0);
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
        next = lab;
        goto enter;
      }

      case DDU: {
        u64  loc = term_val(next);
        Term lab = heap_read(loc + 0);
        PUSH(next);
        if (s_pos > max_d) max_d = s_pos;
        next = lab;
        goto enter;
      }

      default: {
        whnf = next;
        goto apply;
      }
    }
  }

  apply: {
    while (s_pos > base) {
      Term frame = POP();

      switch (term_tag(frame)) {
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
              PUSH(whnf);
              if (s_pos > max_d) max_d = s_pos;
              next = arg;
              goto enter;
            }
            case USE: {
              PUSH(whnf);
              if (s_pos > max_d) max_d = s_pos;
              next = arg;
              goto enter;
            }
            case NUM: {
              if ((u64)max_d > s_hot[S_MAX_DEPTH + threadIdx.x])
                s_hot[S_MAX_DEPTH + threadIdx.x] = (u64)max_d;
              return whnf;
            }
            default: {
              if (term_tag(whnf) >= C00 && term_tag(whnf) <= C16) {
                if ((u64)max_d > s_hot[S_MAX_DEPTH + threadIdx.x])
                  s_hot[S_MAX_DEPTH + threadIdx.x] = (u64)max_d;
                return whnf;
              }
              heap_set(app_loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

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
            case NUM: {
              next = wnf_app_mat_num(mat, whnf);
              goto enter;
            }
            case NAM:
            case BJV:
            case BJ0:
            case BJ1:
            case DRY: {
              whnf = term_new_dry(mat, whnf);
              continue;
            }
            default: {
              if (term_tag(whnf) >= C00 && term_tag(whnf) <= C16) {
                next = wnf_app_mat_ctr(mat, whnf);
                goto enter;
              }
              whnf = term_new_app(mat, whnf);
              continue;
            }
          }
        }

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
            case PRI:
            case NUM: {
              whnf = wnf_dup_nod(lab, loc, side, whnf);
              continue;
            }
            case DRY:
            case MAT:
            case SWI:
            case USE:
            case INC:
            case OP2:
            case DSU:
            case DDU: {
              next = wnf_dup_nod(lab, loc, side, whnf);
              goto enter;
            }
            default: {
              if (term_tag(whnf) >= C00 && term_tag(whnf) <= C16) {
                next = wnf_dup_nod(lab, loc, side, whnf);
                goto enter;
              }
              heap_set(loc, whnf);
              whnf = frame;
              continue;
            }
          }
        }

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
              PUSH(term_new(0, F_OP2_NUM, opr, term_val(whnf)));
              if (s_pos > max_d) max_d = s_pos;
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
              Term x = term_new_num(x_val);
              whnf = term_new_op2(opr, x, whnf);
              continue;
            }
          }
        }

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
              heap_set(loc + 0, whnf);
              PUSH(term_new(0, F_EQL_R, 0, loc));
              if (s_pos > max_d) max_d = s_pos;
              next = b;
              goto enter;
            }
          }
        }

        case F_EQL_R: {
          u64  loc = term_val(frame);
          Term a   = heap_read(loc + 0);

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
              u8 a_tag = term_tag(a);
              u8 b_tag = term_tag(whnf);

              if (a_tag == ANY || b_tag == ANY) {
                whnf = wnf_eql_any_r();
                continue;
              }
              if (a_tag == NUM && b_tag == NUM) {
                whnf = wnf_eql_num(a, whnf);
                continue;
              }
              if (a_tag == LAM && b_tag == LAM) {
                next = wnf_eql_lam(a, whnf);
                goto enter;
              }
              if (a_tag >= C00 && a_tag <= C16 && b_tag >= C00 && b_tag <= C16) {
                next = wnf_eql_ctr(loc, a, whnf);
                goto enter;
              }
              if ((a_tag == MAT || a_tag == SWI) && (b_tag == MAT || b_tag == SWI)) {
                next = wnf_eql_mat(loc, a, whnf);
                goto enter;
              }
              if (a_tag == USE && b_tag == USE) {
                next = wnf_eql_use(loc, a, whnf);
                goto enter;
              }
              if ((a_tag == NAM || a_tag == BJV || a_tag == BJ0 || a_tag == BJ1) &&
                  (b_tag == NAM || b_tag == BJV || b_tag == BJ0 || b_tag == BJ1)) {
                whnf = wnf_eql_nam(a, whnf);
                continue;
              }
              if (a_tag == DRY && b_tag == DRY) {
                next = wnf_eql_dry(loc, a, whnf);
                goto enter;
              }
              whnf = term_new_num(0);
              continue;
            }
          }
        }

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

  if ((u64)max_d > s_hot[S_MAX_DEPTH + threadIdx.x])
    s_hot[S_MAX_DEPTH + threadIdx.x] = (u64)max_d;
  return whnf;

  #undef PUSH
  #undef POP
}

fn bool is_whnf_tag(u8 tag) {
  return (WHNF_MASK >> tag) & 1;
}

fn Term wnf_at(u64 loc) {
  Term cur = heap_read(loc);
  if (is_whnf_tag(term_tag(cur))) return cur;
  Term res = wnf(cur);
  if (res != cur) heap_set(loc, res);
  return res;
}

// ============================================================
// Eval Kernel (run a parsed program)
// ============================================================

__global__ void eval_kernel(u64 *heap, u64 *book, Term *wnf_stacks,
                            u32 main_id, u64 heap_start, u64 heap_end_val, u64 *results) {
  HEAP = heap;
  BOOK = book;
  d_wnf_stacks = wnf_stacks;
  d_wnf_stride = 1;
  d_itrs = 0;
  s_hot[S_WARP_BASE + 0] = heap_start;
  s_hot[S_WARP_HEAP + 0] = heap_start;
  s_hot[S_WARP_END  + 0] = heap_end_val;
  s_hot[S_MAX_DEPTH + threadIdx.x] = 0;

  Term ref = term_new_ref(main_id);
  Term res = wnf(ref);

  results[0] = res;
  results[1] = (u64)term_tag(res);
  results[2] = term_val(res);
  results[3] = d_itrs;
  results[4] = s_hot[S_MAX_DEPTH + threadIdx.x];
  results[5] = s_hot[S_WARP_HEAP + 0];
}

// ============================================================
// Parallel SNF Kernel (static path mapping)
// ============================================================

fn Term spin_until_whnf(u64 loc) {
  for (u32 iter = 0;; iter++) {
    Term t = *(volatile u64*)&HEAP[loc];
    if (is_whnf_tag(term_tag(t))) return t;
    if (iter >= 64) __nanosleep(100);
  }
}

fn void seq_snf(u64 loc) {
  u64 work[32];
  u32 wp = 0;
  work[wp++] = loc;
  while (wp > 0) {
    u64 l = work[--wp];
    Term t = wnf_at(l);
    u8 tag = term_tag(t);
    if (tag == DP0 || tag == DP1) {
      work[wp++] = term_val(t);
      continue;
    }
    u32 ari = term_arity(t);
    u64 tloc = term_val(t);
    for (u32 i = 0; i < ari; i++) {
      work[wp++] = tloc + i;
    }
  }
}

__global__ void par_eval_kernel(u64 *heap, u64 *book, Term *wnf_stacks,
                     u64 root_loc, u64 heap_start, u64 heap_end_val,
                     u32 tree_depth, u32 split_depth, u64 max_region, u64 *results) {
  u32 tid = threadIdx.x + blockIdx.x * blockDim.x;
  u32 n_threads = blockDim.x * gridDim.x;

  HEAP = heap;
  BOOK = book;
  d_wnf_stacks = wnf_stacks;
  d_wnf_stride = n_threads;

  u64 heap_size = heap_end_val - heap_start;
  u32 tpr = (blockDim.x >= 32) ? 32 : 1;
  u32 n_regions = n_threads / tpr;
  u64 region_chunk = heap_size / (u64)n_regions;
  if (max_region > 0 && region_chunk > max_region) region_chunk = max_region;
  u32 region_global = tid / tpr;
  u32 region_local  = threadIdx.x / tpr;
  if (threadIdx.x % tpr == 0) {
    u64 base = heap_start + (u64)region_global * region_chunk;
    s_hot[S_WARP_BASE + region_local] = base;
    s_hot[S_WARP_HEAP + region_local] = base;
    s_hot[S_WARP_END  + region_local] = base + region_chunk;
  }
  d_itrs = 0;
  s_hot[S_MAX_DEPTH + threadIdx.x] = 0;
  __syncthreads();

  u64 loc = root_loc;

  for (u32 d = 0; d < split_depth; d++) {
    u32 shift = split_depth - d;
    u32 owner_mask = (1u << shift) - 1;

    Term t;
    if ((tid & owner_mask) == 0) {
      t = wnf_at(loc);
      __threadfence();
    } else {
      t = spin_until_whnf(loc);
    }

    u32 ari = term_arity(t);
    if (ari < 2) break;

    u32 bit = (tid >> (shift - 1)) & 1;
    loc = term_val(t) + bit;
  }

  __syncwarp();
  seq_snf(loc);

  atomicAdd((unsigned long long*)&results[0], (unsigned long long)d_itrs);
  atomicMax((unsigned int*)&results[1], (unsigned int)s_hot[S_MAX_DEPTH + threadIdx.x]);
}

// ============================================================
// Undo device macros for host code
// ============================================================

#undef fn
#undef ITRS_INC
#undef __ATOMIC_RELAXED
#undef __atomic_fetch_add

// ============================================================
// Host Main
// ============================================================

#define CUDA_CHECK(call) do {                                          \
  cudaError_t err = (call);                                            \
  if (err != cudaSuccess) {                                            \
    fprintf(stderr, "CUDA error at %s:%d: %s\n",                      \
            __FILE__, __LINE__, cudaGetErrorString(err));              \
    exit(1);                                                           \
  }                                                                    \
} while(0)

static u64 g_region_words = 0;

static const char *tag_name(u8 tag) {
  switch (tag) {
    case APP: return "APP"; case VAR: return "VAR"; case LAM: return "LAM";
    case SUP: return "SUP"; case NUM: return "NUM"; case ERA: return "ERA";
    case DRY: return "DRY"; case NAM: return "NAM"; case REF: return "REF";
    default:  return "???";
  }
}

static int run_eval(const char *dump_path) {
  FILE *f = fopen(dump_path, "rb");
  if (!f) {
    fprintf(stderr, "Error: could not open '%s'\n", dump_path);
    return 1;
  }

  u64 heap_used;
  u32 main_id, book_count;
  fread(&heap_used,   sizeof(u64), 1, f);
  fread(&main_id,     sizeof(u32), 1, f);
  fread(&book_count,  sizeof(u32), 1, f);

  printf("Loading dump: heap_used=%llu main_id=%u book_entries=%u\n",
         (unsigned long long)heap_used, main_id, book_count);

  u64 *h_heap = (u64 *)calloc(heap_used, sizeof(u64));
  fread(h_heap, sizeof(u64), heap_used, f);

  size_t book_cap = 1ULL << 24;
  u64 *h_book = (u64 *)calloc(book_cap, sizeof(u64));
  for (u32 i = 0; i < book_count; i++) {
    u32 id;
    u64 val;
    fread(&id,  sizeof(u32), 1, f);
    fread(&val, sizeof(u64), 1, f);
    h_book[id] = val;
  }
  fclose(f);

  size_t heap_words = 768ULL * 1024 * 1024;
  size_t heap_bytes = heap_words * sizeof(u64);
  size_t book_bytes = book_cap * sizeof(u64);

  u64 *d_heap, *d_book, *d_results;
  CUDA_CHECK(cudaMalloc(&d_heap, heap_bytes));
  CUDA_CHECK(cudaMemset(d_heap, 0, heap_bytes));
  CUDA_CHECK(cudaMemcpy(d_heap, h_heap, heap_used * sizeof(u64), cudaMemcpyHostToDevice));

  CUDA_CHECK(cudaMalloc(&d_book, book_bytes));
  CUDA_CHECK(cudaMemcpy(d_book, h_book, book_bytes, cudaMemcpyHostToDevice));

  Term *d_stacks;
  CUDA_CHECK(cudaMalloc(&d_stacks, (size_t)WNF_STACK_CAP * sizeof(Term)));

  CUDA_CHECK(cudaMalloc(&d_results, 16 * sizeof(u64)));

  free(h_heap);
  free(h_book);

  printf("Heap: %zu MB on device, starting alloc at word %llu\n",
         heap_bytes / (1024 * 1024), (unsigned long long)heap_used);
  printf("Evaluating @main (id=%u) on 1 GPU thread...\n\n", main_id);

  cudaEvent_t t0, t1;
  CUDA_CHECK(cudaEventCreate(&t0));
  CUDA_CHECK(cudaEventCreate(&t1));

  CUDA_CHECK(cudaEventRecord(t0));
  eval_kernel<<<1, 1, S_HOT_BYTES>>>(d_heap, d_book, d_stacks, main_id, heap_used, heap_words, d_results);
  CUDA_CHECK(cudaEventRecord(t1));
  CUDA_CHECK(cudaEventSynchronize(t1));

  float ms = 0;
  CUDA_CHECK(cudaEventElapsedTime(&ms, t0, t1));

  u64 h_results[16];
  CUDA_CHECK(cudaMemcpy(h_results, d_results, 16 * sizeof(u64), cudaMemcpyDeviceToHost));

  u64 result_term = h_results[0];
  u8  result_tag  = (u8)h_results[1];
  u64 result_val  = h_results[2];
  u64 itrs        = h_results[3];

  double secs = ms / 1000.0;
  double mips = (double)itrs / secs / 1e6;

  u64 max_depth  = h_results[4];
  u64 heap_final = h_results[5];

  printf("Result: %s (tag=%u val=%llu)\n", tag_name(result_tag), result_tag, (unsigned long long)result_val);
  printf("- Itrs: %llu interactions\n", (unsigned long long)itrs);
  printf("- Heap: %llu nodes\n", (unsigned long long)heap_final);
  printf("- Stck: %llu max depth\n", (unsigned long long)max_depth);
  printf("- Time: %.3f seconds\n", secs);
  printf("- Perf: %.2f M interactions/s\n", mips);

  CUDA_CHECK(cudaEventDestroy(t0));
  CUDA_CHECK(cudaEventDestroy(t1));
  CUDA_CHECK(cudaFree(d_heap));
  CUDA_CHECK(cudaFree(d_book));
  CUDA_CHECK(cudaFree(d_stacks));
  CUDA_CHECK(cudaFree(d_results));
  return 0;
}

static u64 host_term_new(u8 tag, u32 ext, u64 val) {
  return ((u64)(tag & 0x7F) << 56) | ((u64)(ext & 0x3FFFF) << 38) | (val & 0x3FFFFFFFFFULL);
}

static int run_par_eval(const char *dump_path, u32 tree_depth, cudaDeviceProp *prop_ptr) {
  cudaDeviceProp prop = *prop_ptr;
  FILE *f = fopen(dump_path, "rb");
  if (!f) {
    fprintf(stderr, "Error: could not open '%s'\n", dump_path);
    return 1;
  }

  u64 heap_used;
  u32 main_id, book_count;
  fread(&heap_used,   sizeof(u64), 1, f);
  fread(&main_id,     sizeof(u32), 1, f);
  fread(&book_count,  sizeof(u32), 1, f);

  u64 *h_heap = (u64 *)calloc(heap_used + 1, sizeof(u64));
  fread(h_heap, sizeof(u64), heap_used, f);

  size_t book_cap = 1ULL << 24;
  u64 *h_book = (u64 *)calloc(book_cap, sizeof(u64));
  for (u32 i = 0; i < book_count; i++) {
    u32 id; u64 val;
    fread(&id,  sizeof(u32), 1, f);
    fread(&val, sizeof(u64), 1, f);
    h_book[id] = val;
  }
  fclose(f);

  u64 root_loc = heap_used;
  h_heap[root_loc] = host_term_new(REF, main_id, 0);
  u64 alloc_start = root_loc + 1;

  u32 n_threads = 1u << tree_depth;
  u32 split_depth = tree_depth;
  u32 block_size = 1;
  if (n_threads > 32) block_size = 128;
  if (n_threads < block_size) block_size = n_threads;
  u32 n_blocks = n_threads / block_size;

  size_t stack_per_thread = (n_threads <= 65536) ? 1024 : 64;
  size_t book_bytes = book_cap * sizeof(u64);
  size_t stack_bytes = (size_t)n_threads * stack_per_thread * sizeof(Term);
  size_t reserved = 3ULL << 30;
  size_t overhead = book_bytes + stack_bytes + reserved;
  size_t avail = prop.totalGlobalMem > overhead ? prop.totalGlobalMem - overhead : 0;
  size_t heap_bytes = (avail / sizeof(u64)) * sizeof(u64);
  size_t heap_words = heap_bytes / sizeof(u64);

  printf("Parallel eval: %u threads (%u blocks x %u), tree_depth=%u, split_depth=%u\n",
         n_threads, n_blocks, block_size, tree_depth, split_depth);
  printf("Heap: %zu MB, Stacks: %zu MB (%zu/thread)\n",
         heap_bytes / (1024*1024), stack_bytes / (1024*1024), stack_per_thread);

  u64 *d_heap, *d_book, *d_results;
  Term *d_stacks;

  CUDA_CHECK(cudaMalloc(&d_heap, heap_bytes));
  CUDA_CHECK(cudaMemset(d_heap, 0, heap_bytes));
  CUDA_CHECK(cudaMemcpy(d_heap, h_heap, (heap_used + 1) * sizeof(u64), cudaMemcpyHostToDevice));
  CUDA_CHECK(cudaMalloc(&d_book, book_bytes));
  CUDA_CHECK(cudaMemcpy(d_book, h_book, book_bytes, cudaMemcpyHostToDevice));
  CUDA_CHECK(cudaMalloc(&d_stacks, stack_bytes));
  CUDA_CHECK(cudaMalloc(&d_results, 16 * sizeof(u64)));
  CUDA_CHECK(cudaMemset(d_results, 0, 16 * sizeof(u64)));

  free(h_heap);
  free(h_book);

  cudaEvent_t t0, t1;
  CUDA_CHECK(cudaEventCreate(&t0));
  CUDA_CHECK(cudaEventCreate(&t1));

  CUDA_CHECK(cudaEventRecord(t0));
  u64 max_region = g_region_words;
  par_eval_kernel<<<n_blocks, block_size, S_HOT_BYTES>>>(
    d_heap, d_book, d_stacks, root_loc, alloc_start, heap_words,
    tree_depth, split_depth, max_region, d_results);
  CUDA_CHECK(cudaEventRecord(t1));
  CUDA_CHECK(cudaEventSynchronize(t1));

  float ms = 0;
  CUDA_CHECK(cudaEventElapsedTime(&ms, t0, t1));

  u64 h_results[16];
  CUDA_CHECK(cudaMemcpy(h_results, d_results, 16 * sizeof(u64), cudaMemcpyDeviceToHost));

  double secs = ms / 1000.0;
  u64 itrs = h_results[0];
  double mips = (double)itrs / secs / 1e6;

  u64 max_depth = h_results[1] & 0xFFFFFFFFULL;

  printf("\n");
  printf("- Itrs: %llu interactions\n", (unsigned long long)itrs);
  printf("- Stck: %llu max depth\n", (unsigned long long)max_depth);
  printf("- Time: %.3f seconds\n", secs);
  printf("- Perf: %.2f M interactions/s\n", mips);

  CUDA_CHECK(cudaEventDestroy(t0));
  CUDA_CHECK(cudaEventDestroy(t1));
  CUDA_CHECK(cudaFree(d_heap));
  CUDA_CHECK(cudaFree(d_book));
  CUDA_CHECK(cudaFree(d_stacks));
  CUDA_CHECK(cudaFree(d_results));
  return 0;
}

int main(int argc, char **argv) {
  int device = 0;
  cudaDeviceProp prop;
  CUDA_CHECK(cudaGetDeviceProperties(&prop, device));
  // default stack size is fine for the interpreter
  printf("Device: %s (SM %d.%d, %zu MB)\n\n", prop.name, prop.major, prop.minor,
         prop.totalGlobalMem / (1024 * 1024));

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <dump.bin> [-p DEPTH] [-r REGION_KB]\n", argv[0]);
    return 1;
  }

  u32 par_depth = 0;
  int has_par = 0;
  u64 region_kb = 0;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      par_depth = atoi(argv[++i]);
      has_par = 1;
    } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
      region_kb = atoll(argv[++i]);
    }
  }
  g_region_words = region_kb * 128;
  if (has_par) return run_par_eval(argv[1], par_depth, &prop);

  return run_eval(argv[1]);
}
