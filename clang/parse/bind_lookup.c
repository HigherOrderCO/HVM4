// Lookup binding by name. Skips to outer binds on false shadowing (dup var + lam bind, lam var + dup bind, etc)
// Returns 1 and sets the bind if found, skipped = 1 if skipped a binding with the same name.
fn PBind* parse_bind_lookup(u32 name, int side, int *skipped) {
  *skipped = 0;
  for (int i = PARSE_BINDS_LEN - 1; i >= 0; i--) {
    PBind* bind = &PARSE_BINDS[i];
    if (bind->name == name) {
      // Destructured dup aliases are used without an explicit subscript.
      if (bind->side >= 0) {
        if (side != -1) {
          *skipped = 1;
          continue;
        }
        return bind;
      }
      // Skip dup bindings if no subscript and not in fork mode
      if (side == -1 && bind->lab != 0 && !bind->forked) {
        *skipped = 1;
        continue;
      }
      // Skip non-dup bindings if subscript or fork mode
      if (side != -1 && bind->lab == 0) {
        *skipped = 1;
        continue;
      }
      return bind;
    }
  }
  return NULL;
}
