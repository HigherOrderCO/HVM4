# C FFI

This document describes the C FFI for HVM4: how it is implemented in the runtime and how to use it from external C code.

**Overview**
- FFI libraries are loaded before parsing, so primitives can be registered in time for arity checks.
- Each library exposes `hvm4_ffi_init` and receives an API table with all safe entry points.
- Registered primitives are callable with the `%name(args...)` syntax.
- Re-registering an existing primitive name overrides it and prints a warning.

**FFI Header**
- Include the public header at `clang/hvm4_ffi.h`.
- This header defines `HvmApi`, `Term`, tags, and `ABI_VERSION`.

**Loading Model**
- Load a shared library with `--ffi <path>`.
- Load all shared libraries in a directory with `--ffi-dir <path>`.
- Libraries must export:
```c
void hvm4_ffi_init(const HvmApi *api);
```

**Implementation Notes**
- Dynamic loading is done with `dlopen`/`dlsym` in `clang/ffi/load.c` and `clang/ffi/load_dir.c`.
- The API table is built in `clang/ffi/api.c` and passed to `hvm4_ffi_init`.
- Primitive registration uses `prim_register` in `clang/prim/register.c`, which stores a function pointer and arity by name ID.
- The parser uses `prim_arity` to validate `%name(...)` applications before evaluation.
- The evaluator calls primitives via `wnf/pri.c`.

**Minimal Example**
```c
#include "clang/hvm4_ffi.h"

static const HvmApi *api;

static Term prim_c_add(Term *args) {
  Term a = api->wnf(args[0]);
  Term b = api->wnf(args[1]);
  if (api->term_tag(a) != NUM || api->term_tag(b) != NUM) {
    api->runtime_error("%c_add expected numbers");
  }
  return api->term_new_num(api->term_val(a) + api->term_val(b));
}

void hvm4_ffi_init(const HvmApi *api_arg) {
  api = api_arg;
  if (api->abi_version != ABI_VERSION) {
    api->runtime_error("FFI ABI mismatch");
  }
  api->register_prim("c_add", 5, 2, prim_c_add);
}
```

**Build the FFI Library**
- macOS:
```bash
clang -dynamiclib -fPIC -I /path/to/hvm4 \
  -o /path/to/libffi_example.dylib \
  /path/to/ffi_example.c
```
- Linux:
```bash
clang -shared -fPIC -I /path/to/hvm4 \
  -o /path/to/libffi_example.so \
  /path/to/ffi_example.c
```

**Use It**
```bash
./clang/main program.hvm4 --ffi /path/to/libffi_example.dylib
./clang/main program.hvm4 --ffi-dir /path/to/ffi_dir
```

**Strings and Constructors**
- HVM strings are lists using `#CON`, `#CHR`, and `#NIL` (uppercase).
- `term_new_ctr` takes a nick-encoded constructor name, not a table ID.
- Use `name_from_str("CON", 3)` and friends to get the correct constructor IDs.

**Threading and Safety**
- Primitives may run concurrently in `-T<N>` mode.
- Calling `wnf`, `heap_alloc`, or `heap_read/set` inside a primitive is safe because primitives run on runtime worker threads with proper thread IDs.
- Do not call HVM API functions from threads you create yourself.
- `table_find` mutates global state, so only call it during `hvm4_ffi_init`.
- Any external side effects (I/O, logging, global state) must be synchronized by the FFI if you need deterministic ordering.

**Limitations**
- GPU execution does not support FFI (runtime aborts).
