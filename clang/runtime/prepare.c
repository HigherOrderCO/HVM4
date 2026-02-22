// Runtime Program Prepare
// =======================
// Parses one source buffer, validates static-space limits, and resolves @main.

// Forward declarations
// --------------------

fn void parse_program(const char *source_path, char *src);
fn int  runtime_entry(const char *name, u32 *out_id);

// Runtime Prepare
// ---------------

// Parses and validates one source buffer, returning @main id on success.
fn int runtime_prepare(u32 *main_id, const char *src_path, char *src) {
  if (main_id == NULL || src == NULL) {
    return 0;
  }

  parse_program(src_path, src);

  if (HEAP_NEXT_AT(0) > (ALO_TM_MASK + 1)) {
    fprintf(stderr, "Error: static book exceeds 24-bit location space (%llu words used)\n", HEAP_NEXT_AT(0));
    return 0;
  }

  if (!runtime_entry("main", main_id)) {
    fprintf(stderr, "Error: @main not defined\n");
    return 0;
  }

  return 1;
}
