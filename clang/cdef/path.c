fn void cdef_path(char *out, u32 out_len, const char *src_path, const char *suffix) {
  const char *base = strrchr(src_path, '/');
  if (base != NULL) {
    base = base + 1;
  } else {
    base = src_path;
  }

  char name[256];
  u32  len = 0;
  while (base[len] != '\0' && len < sizeof(name) - 1) {
    char c = base[len];
    name[len] = isalnum((u8)c) ? c : '_';
    len++;
  }
  name[len] = '\0';

  snprintf(out, out_len, ".build/%s%s", name, suffix);
}
