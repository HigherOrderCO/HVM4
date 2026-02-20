// JIT Module: Symbol Sanitizer
// ----------------------------
// Converts HVM names into filesystem-safe identifiers for generated filenames.

// Converts a symbol name into a filesystem-safe alnum/_ identifier.
fn char *jit_sanitize(const char *name) {
  size_t len = strlen(name);
  size_t cap = (len * 4) + 1;
  char *out  = malloc(cap);
  if (out == NULL) {
    return NULL;
  }

  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    u8 c = (u8)name[i];
    if (isalnum(c) || c == '_') {
      out[j++] = (char)c;
      continue;
    }

    if (j + 4 >= cap) {
      free(out);
      return NULL;
    }

    static const char HEX[] = "0123456789ABCDEF";
    out[j++] = '_';
    out[j++] = 'x';
    out[j++] = HEX[(c >> 4) & 0xF];
    out[j++] = HEX[c & 0xF];
  }

  out[j] = '\0';
  return out;
}
