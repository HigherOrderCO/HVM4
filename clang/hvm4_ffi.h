#ifndef HVM4_FFI_H
#define HVM4_FFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ABI_VERSION 1

#ifndef HVM4_RUNTIME
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef u64 Term;
#endif

typedef Term (*HvmPrimFn)(Term *args);

typedef struct {
  u32 abi_version;

  // primitive registration
  u32  (*register_prim)(const char *name, u32 len, u32 arity, HvmPrimFn fun);

  // evaluator
  Term (*wnf)(Term term);

  // constructors
  Term (*term_new_num)(u32 n);
  Term (*term_new_ctr)(u32 name, u32 arity, Term *args);
  Term (*term_new_sup)(u32 label, Term a, Term b);
  Term (*term_new_dup)(u32 label, Term expr, Term body);
  Term (*term_new_app)(Term f, Term x);
  Term (*term_new_lam_at)(u32 loc, Term body);
  Term (*term_new_lam)(Term body);
  Term (*term_new_var)(u32 loc);

  // accessors
  u8   (*term_tag)(Term t);
  u32  (*term_ext)(Term t);
  u32  (*term_val)(Term t);

  // heap
  Term (*heap_read)(u32 loc);
  void (*heap_set)(u32 loc, Term t);
  u64  (*heap_alloc)(u64 words);

  // names
  u32  (*table_find)(const char *name, u32 len);
  u32  (*name_from_str)(const char *name, u32 len);

  // errors
  void (*runtime_error)(const char *msg);
} HvmApi;

#ifndef HVM4_RUNTIME
// Tags
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
#define RED 36
#define EQL 37
#define AND 38
#define OR  39
#define UNS 40
#define ANY 41
#define INC 42
#define BJV 43
#define BJ0 44
#define BJ1 45
#define PRI 46

// LAM Ext Flags
#define LAM_ERA_MASK 0x800000
#endif

#ifdef __cplusplus
}
#endif

#endif
