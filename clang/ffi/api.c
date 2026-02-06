#define HVM4_RUNTIME
#include "../hvm4_ffi.h"
#undef HVM4_RUNTIME

fn Term wnf(Term term);
fn u32 prim_register(const char *name, u32 len, u32 arity, HvmPrimFn fun);

fn Term term_new_num(u32 n);
fn Term term_new_ctr(u32 name, u32 arity, Term *args);
fn Term term_new_sup(u32 label, Term a, Term b);
fn Term term_new_dup(u32 label, Term expr, Term body);
fn Term term_new_app(Term f, Term x);
fn Term term_new_lam_at(u32 loc, Term body);
fn Term term_new_lam(Term body);
fn Term term_new_var(u32 loc);

fn u8  term_tag(Term t);
fn u32 term_ext(Term t);
fn u32 term_val(Term t);

fn Term heap_read(u32 loc);
fn void heap_set(u32 loc, Term t);
fn u64 heap_alloc(u64 words);

fn u32 table_find(const char *name, u32 len);
fn u32 nick_from_str(const char *name, u32 len);

fn void sys_runtime_error(const char *msg);

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

  .term_tag        = term_tag,
  .term_ext        = term_ext,
  .term_val        = term_val,

  .heap_read       = heap_read,
  .heap_set        = heap_set,
  .heap_alloc      = heap_alloc,

  .table_find      = table_find,
  .name_from_str   = nick_from_str,

  .runtime_error   = sys_runtime_error
};

fn const HvmApi *ffi_api(void) {
  return &HVM_API;
}
