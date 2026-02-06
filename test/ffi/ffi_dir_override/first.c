#include "clang/hvm4_ffi.h"

static const HvmApi *api;

static Term prim_ffi_same(Term *args) {
  (void)args;
  return api->term_new_num(7);
}

void hvm4_ffi_init(const HvmApi *api_arg) {
  api = api_arg;
  if (api->abi_version != ABI_VERSION) {
    api->runtime_error("FFI ABI mismatch");
  }
  api->register_prim("ffi_same", 8, 1, prim_ffi_same);
}
