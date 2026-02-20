// JIT Module: Native Compilation
// ------------------------------
// Invokes clang to build one emitted JIT C file into a platform shared object.

// Compiles one emitted JIT C file into a shared library.
fn void jit_compile(const char *c_path, const char *lib_path, const char *jit_include_dir) {
  char cmd[4096];
#if defined(__APPLE__)
  snprintf(cmd, sizeof(cmd),
    "clang -O2 -dynamiclib -fPIC -Wl,-undefined,dynamic_lookup -I'%s' -o '%s' '%s'",
    jit_include_dir,
    lib_path,
    c_path);
#else
  snprintf(cmd, sizeof(cmd),
    "clang -O2 -shared -fPIC -I'%s' -o '%s' '%s'",
    jit_include_dir,
    lib_path,
    c_path);
#endif

  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr, "ERROR: failed to compile JIT module '%s'\n", c_path);
    exit(1);
  }
}
