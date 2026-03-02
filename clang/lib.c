// lib.c - HVM4 library entry point for embedding.
//
// Builds HVM as a linkable library by exposing selected static inline
// functions as real exported symbols. Include this instead of main.c
// when embedding HVM in another application.
//
// Build (static library):
//   clang -O2 -c lib.c -o hvm.o
//   ar rcs libhvm.a hvm.o
//
// Build (shared library, macOS):
//   clang -O2 -dynamiclib lib.c -o libhvm.dylib
//
// Build (shared library, Linux):
//   clang -O2 -shared -fPIC lib.c -o libhvm.so -ldl

#include "hvm.c"

// Runtime lifecycle
// -----------------

void hvm_init(u32 threads, int debug, int silent, int steps) {
  runtime_init(threads, debug, silent, steps);
}

void hvm_free(void) {
  runtime_free();
}

int hvm_prepare(u32 *main_id, const char *src_path, char *src) {
  return runtime_prepare(main_id, src_path, src);
}

int hvm_prepare_text(u32 *main_id, const char *src_path, const char *src_text) {
  return runtime_prepare_text(main_id, src_path, src_text);
}

// Evaluation
// ----------

Term hvm_wnf(Term term) {
  return wnf(term);
}

Term hvm_normalize(Term term) {
  return eval_normalize(term);
}

// Term constructors
// -----------------

Term hvm_term_new_num(u32 n) {
  return term_new_num(n);
}

Term hvm_term_new_ctr(u32 name, u32 arity, Term *args) {
  return term_new_ctr(name, arity, args);
}

Term hvm_term_new_sup(u32 label, Term a, Term b) {
  return term_new_sup(label, a, b);
}

Term hvm_term_new_dup(u32 label, Term expr, Term body) {
  return term_new_dup(label, expr, body);
}

Term hvm_term_new_app(Term f, Term x) {
  return term_new_app(f, x);
}

Term hvm_term_new_lam(Term body) {
  return term_new_lam(body);
}

Term hvm_term_new_lam_at(u64 loc, Term body) {
  return term_new_lam_at(loc, body);
}

Term hvm_term_new_var(u64 loc) {
  return term_new_var(loc);
}

Term hvm_term_new_ref(u32 name) {
  return term_new_ref(name);
}

// Term accessors
// --------------

u8 hvm_term_tag(Term t) {
  return term_tag(t);
}

u32 hvm_term_ext(Term t) {
  return term_ext(t);
}

u64 hvm_term_val(Term t) {
  return term_val(t);
}

// Heap operations
// ---------------

Term hvm_heap_read(u64 loc) {
  return heap_read(loc);
}

void hvm_heap_set(u64 loc, Term t) {
  heap_set(loc, t);
}

u64 hvm_heap_alloc(u64 words) {
  return heap_alloc(words);
}

// Symbol table
// ------------

u32 hvm_table_find(const char *name, u32 len) {
  return table_find(name, len);
}

// Primitives
// ----------

u32 hvm_prim_register(const char *name, u32 len, u32 arity, HvmPrimFn fun) {
  return prim_register(name, len, arity, fun);
}
