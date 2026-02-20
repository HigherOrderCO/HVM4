// JIT Runtime API Header
// ----------------------
// Shared ABI between the core runtime and generated JIT C modules. Provides
// compact term helpers, fast stack arg probes, interaction code ids, and
// exported bridge functions used by emitted code.

#ifndef HVM4_JIT_RUNTIME_H
#define HVM4_JIT_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HVM4_RUNTIME
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef u64      Term;
#endif

#ifndef APP
#define APP 0
#endif

#ifndef TAG_SHIFT
#define TAG_SHIFT 56
#endif

#ifndef EXT_SHIFT
#define EXT_SHIFT 40
#endif

#ifndef TAG_MASK
#define TAG_MASK 0x7FULL
#endif

#ifndef EXT_MASK
#define EXT_MASK 0xFFFFULL
#endif

#ifndef VAL_MASK
#define VAL_MASK 0xFFFFFFFFFFULL
#endif

#ifndef C00
#define C00 13
#endif

#ifndef C16
#define C16 29
#endif

#ifndef NUM
#define NUM 30
#endif

#ifndef SWI
#define SWI 31
#endif

// Returns the runtime tag field from a packed term.
static inline u8 jit_tag(Term t) {
  return (u8)((t >> TAG_SHIFT) & TAG_MASK);
}

// Returns the runtime ext field from a packed term.
static inline u16 jit_ext(Term t) {
  return (u16)((t >> EXT_SHIFT) & EXT_MASK);
}

// Returns the runtime value field from a packed term.
static inline u64 jit_val(Term t) {
  return t & VAL_MASK;
}

// Reads the top APP argument without consuming the stack frame.
static inline int jit_peek_app_arg(Term *stack, u32 *s_pos, u32 base, Term *heap, Term *arg) {
  if (*s_pos <= base) {
    return 0;
  }
  Term frame = stack[*s_pos - 1];
  if (jit_tag(frame) != APP) {
    return 0;
  }
  u64 app_loc = jit_val(frame);
  *arg = heap[app_loc + 1];
  return 1;
}

// Reads a numeric APP argument without consuming the stack frame.
static inline int jit_peek_num_arg(Term *stack, u32 *s_pos, u32 base, Term *heap, u64 *num) {
  Term arg;
  if (!jit_peek_app_arg(stack, s_pos, base, heap, &arg)) {
    return 0;
  }
  if (jit_tag(arg) != NUM) {
    return 0;
  }
  *num = jit_val(arg);
  return 1;
}

// Reads a constructor APP argument without consuming the stack frame.
static inline int jit_peek_ctr_arg(Term *stack, u32 *s_pos, u32 base, Term *heap, Term *ctr, u8 *ctr_tag) {
  Term arg;
  if (!jit_peek_app_arg(stack, s_pos, base, heap, &arg)) {
    return 0;
  }
  u8 tag = jit_tag(arg);
  if (tag < C00 || tag > C16) {
    return 0;
  }
  *ctr = arg;
  *ctr_tag = tag;
  return 1;
}

enum {
  JIT_I_LAM   = 0,
  JIT_I_CTR_H = 1,
  JIT_I_CTR_M = 2,
  JIT_I_NUM_H = 3,
  JIT_I_NUM_M = 4,
};

Term *jit_heap(void);
void  jit_itr(u8 code);
void  jit_push_ctr_apps(Term *stack, u32 *s_pos, Term *heap, Term ctr, u8 ctr_tag);
Term  jit_alo(u64 tm_loc, u16 len, const Term *args);

typedef Term (*HvmJitFn)(Term *stack, u32 *s_pos, u32 base);

#ifdef __cplusplus
}
#endif

#endif
