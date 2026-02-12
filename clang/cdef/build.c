fn void cdef_build(const char *c_path, const char *so_path) {
  char cmd[4096];
  snprintf(cmd, sizeof(cmd), "clang -O3 -fPIC -shared \"%s\" -o \"%s\"", c_path, so_path);
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr, "ERROR: failed to compile compiled-def library: %s\n", so_path);
    exit(1);
  }
}
