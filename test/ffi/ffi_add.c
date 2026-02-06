#include "clang/hvm4_ffi.h"

static const HvmApi *api;

static Term prim_c_add(Term *args) {
  Term a = api->wnf(args[0]);
  Term b = api->wnf(args[1]);
  if (api->term_tag(a) != NUM || api->term_tag(b) != NUM) {
    api->runtime_error("%c_add expected numbers");
  }
  u32 res = api->term_val(a) + api->term_val(b);
  return api->term_new_num(res);
}

void hvm4_ffi_init(const HvmApi *api_arg) {
  api = api_arg;
  if (api->abi_version != ABI_VERSION) {
    api->runtime_error("FFI ABI mismatch");
  }
  api->register_prim("c_add", 5, 2, prim_c_add);
}
