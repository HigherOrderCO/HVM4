fn void sys_runtime_error(const char *msg) {
  fprintf(stderr, "RUNTIME_ERROR: %s\n", msg);
  exit(1);
}
