// Runtime Program Prepare Text
// ============================
// Convenience wrapper that prepares a program from immutable source text.

// Forward declarations
// --------------------

fn int runtime_prepare(u32 *main_id, const char *src_path, char *src);

// Runtime Prepare Text
// --------------------

// Duplicates immutable source text and prepares it as a runtime program.
fn int runtime_prepare_text(u32 *main_id, const char *src_path, const char *src_text) {
  if (src_text == NULL) {
    return 0;
  }

  char *src = strdup(src_text);
  if (src == NULL) {
    sys_error("AOT source allocation failed");
  }

  int ok = runtime_prepare(main_id, src_path, src);
  free(src);
  return ok;
}
