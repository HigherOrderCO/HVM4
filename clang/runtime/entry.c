// Runtime Entry Lookup
// --------------------
// Resolves a named top-level entrypoint to a BOOK id.

// Resolves one top-level entry id by name; returns 1 when defined.
fn int runtime_entry(const char *name, u32 *out_id) {
  if (name == NULL || out_id == NULL) {
    return 0;
  }

  u32 len = (u32)strlen(name);
  u32 id  = table_find(name, len);
  if (BOOK[id] == 0) {
    return 0;
  }

  *out_id = id;
  return 1;
}
