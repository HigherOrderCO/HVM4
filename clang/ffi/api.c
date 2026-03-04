#define HVM_RUNTIME
#include "../hvm_ffi.h"
#undef HVM_RUNTIME

fn Term wnf(Term term);
fn u32 prim_register(const char *name, u32 len, u32 arity, HvmPrimFn fun);

fn Term term_new_num(u32 n);
fn Term term_new_ctr(u32 name, u32 arity, Term *args);
fn Term term_new_sup(u32 label, Term a, Term b);
fn Term term_new_dup(u32 label, Term expr, Term body);
fn Term term_new_app(Term f, Term x);
fn Term term_new_lam_at(u64 loc, Term body);
fn Term term_new_lam(Term body);
fn Term term_new_var(u64 loc);
fn Term term_new_era(void);
fn Term term_new_ref(u32 nam);
fn Term term_new_mat(u32 nam, Term val, Term nxt);

fn u8   term_tag(Term t);
fn u32  term_ext(Term t);
fn u64  term_val(Term t);
fn u32  term_arity(Term t);
fn u8   term_sub_get(Term t);
fn Term term_sub_set(Term t, u8 sub);

fn Term heap_read(u64 loc);
fn void heap_set(u64 loc, Term t);
fn u64 heap_alloc(u64 words);

fn u32   table_find(const char *name, u32 len);
fn char *table_get(u32 id);

fn void sys_runtime_error(const char *msg);

fn u64 ffi_book_get(u32 id) {
  return BOOK[id];
}

fn void ffi_book_set(u32 id, u64 loc) {
  BOOK[id] = loc;
}

static const HvmApi HVM_API = {
  .abi_version   = ABI_VERSION,

  .register_prim = prim_register,
  .wnf           = wnf,

  .term_new_num    = term_new_num,
  .term_new_ctr    = term_new_ctr,
  .term_new_sup    = term_new_sup,
  .term_new_dup    = term_new_dup,
  .term_new_app    = term_new_app,
  .term_new_lam_at = term_new_lam_at,
  .term_new_lam    = term_new_lam,
  .term_new_var    = term_new_var,
  .term_new_era    = term_new_era,
  .term_new_ref    = term_new_ref,
  .term_new_mat    = term_new_mat,

  .term_tag        = term_tag,
  .term_ext        = term_ext,
  .term_val        = term_val,
  .term_arity      = term_arity,
  .term_sub_get    = term_sub_get,
  .term_sub_set    = term_sub_set,

  .heap_read       = heap_read,
  .heap_set        = heap_set,
  .heap_alloc      = heap_alloc,

  .table_find      = table_find,
  .name_from_str   = table_find,
  .table_get       = table_get,
  .book_get        = ffi_book_get,
  .book_set        = ffi_book_set,

  .runtime_error   = sys_runtime_error
};

fn const HvmApi *ffi_api(void) {
  return &HVM_API;
}
