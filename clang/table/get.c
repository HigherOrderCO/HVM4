// Returns the name string for a given id, or NULL if not set.
fn char *table_get(u32 id) {
  if (TABLE.data == NULL || id >= TABLE.len) {
    return NULL;
  }
  return TABLE.data[id];
}
