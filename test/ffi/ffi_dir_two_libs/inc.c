#include "clang/hvm4_ffi.h"

static const HvmApi *api;

static Term prim_ffi_inc(Term *args) {
  Term a = api->wnf(args[0]);
  if (api->term_tag(a) != NUM) {
    api->runtime_error("%ffi_inc expected number");
  }
  u32 res = api->term_val(a) + 1;
  return api->term_new_num(res);
}

void hvm4_ffi_init(const HvmApi *api_arg) {
  api = api_arg;
  if (api->abi_version != ABI_VERSION) {
    api->runtime_error("FFI ABI mismatch");
  }
  api->register_prim("ffi_inc", 7, 1, prim_ffi_inc);
}
