// hvm_lib.h - Public header for HVM4 as a linkable library.
//
// Include this header to call HVM functions from an embedding application.
// Link against libhvm.a (static) or libhvm.so/dylib (shared) built from lib.c.

#ifndef HVM_LIB_H
#define HVM_LIB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Types
// -----

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef u64 Term;
typedef Term (*HvmPrimFn)(Term *args);

// Tags
// ----

#define HVM_TAG_APP  0
#define HVM_TAG_VAR  1
#define HVM_TAG_LAM  2
#define HVM_TAG_DP0  3
#define HVM_TAG_DP1  4
#define HVM_TAG_SUP  5
#define HVM_TAG_DUP  6
#define HVM_TAG_ALO  7
#define HVM_TAG_REF  8
#define HVM_TAG_NAM  9
#define HVM_TAG_DRY 10
#define HVM_TAG_ERA 11
#define HVM_TAG_MAT 12
#define HVM_TAG_NUM 30
#define HVM_TAG_SWI 31
#define HVM_TAG_PRI 45

// Runtime lifecycle
// -----------------

void hvm_init(u32 threads, int debug, int silent, int steps);
void hvm_free(void);
int  hvm_prepare(u32 *main_id, const char *src_path, char *src);
int  hvm_prepare_text(u32 *main_id, const char *src_path, const char *src_text);

// Evaluation
// ----------

Term hvm_wnf(Term term);
Term hvm_normalize(Term term);

// Term constructors
// -----------------

Term hvm_term_new_num(u32 n);
Term hvm_term_new_ctr(u32 name, u32 arity, Term *args);
Term hvm_term_new_sup(u32 label, Term a, Term b);
Term hvm_term_new_dup(u32 label, Term expr, Term body);
Term hvm_term_new_app(Term f, Term x);
Term hvm_term_new_lam(Term body);
Term hvm_term_new_lam_at(u64 loc, Term body);
Term hvm_term_new_var(u64 loc);
Term hvm_term_new_ref(u32 name);

// Term accessors
// --------------

u8   hvm_term_tag(Term t);
u32  hvm_term_ext(Term t);
u64  hvm_term_val(Term t);

// Heap operations
// ---------------

Term hvm_heap_read(u64 loc);
void hvm_heap_set(u64 loc, Term t);
u64  hvm_heap_alloc(u64 words);

// Symbol table
// ------------

u32  hvm_table_find(const char *name, u32 len);

// Primitives
// ----------

u32  hvm_prim_register(const char *name, u32 len, u32 arity, HvmPrimFn fun);

#ifdef __cplusplus
}
#endif

#endif
