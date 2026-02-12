fn void cdef_init(const char *src_path) {
  cdef_reset(TABLE_LEN);
  u32 defs = cdef_fill_aris();
  if (defs == 0) {
    return;
  }

  int mk = system("mkdir -p .build");
  if (mk != 0) {
    sys_error("failed to create .build directory");
  }

  char c_path[1024];
  char so_path[1024];
  cdef_path(c_path, sizeof(c_path), src_path, ".cdefs.c");
  cdef_path(so_path, sizeof(so_path), src_path, ".cdefs.so");

  cdef_gen(c_path);
  cdef_build(c_path, so_path);
  cdef_load(so_path);
}
