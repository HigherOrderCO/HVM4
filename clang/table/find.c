fn void sys_error(const char *msg);

// Finds or creates an ID for the given name in the global table.
// If the name exists, returns its existing ID.
// If not, assigns a new unique ID and stores the name.
fn u32 table_find(const char *name, u32 len) {
  if (TABLE.data == NULL) {
    sys_error("name table not initialized");
  }
  // Linear scan for existing name
  for (u32 i = 0; i < TABLE.len; i++) {
    if (strlen(TABLE.data[i]) == len && memcmp(TABLE.data[i], name, len) == 0) {
      return i;
    }
  }
  if (TABLE.len >= BOOK_CAP) {
    sys_error("name table overflow");
  }
  // Not found - create new entry
  u32  id   = TABLE.len++;
  char *copy = malloc(len + 1);
  if (copy == NULL) {
    sys_error("Memory allocation failed");
  }
  memcpy(copy, name, len);
  copy[len] = '\0';
  TABLE.data[id] = copy;
  return id;
}
