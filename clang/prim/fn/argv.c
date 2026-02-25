// %argv(dummy)
// ------------
// List<String> with CLI args passed after bare `--`.
fn Term prim_fn_argv(Term *args) {
  (void)args[0];

  Term nil      = term_new_ctr(SYM_NIL, 0, 0);
  Term out      = nil;
  Term cur      = nil;
  u8   has_node = 0;

  for (int i = 0; i < PRIM_ARGC; i++) {
    const char *arg = PRIM_ARGV[i];

    Term str     = term_string_from_utf8(arg);
    Term h_t[2]  = {str, nil};
    Term node    = term_new_ctr(SYM_CON, 2, h_t);

    if (!has_node) {
      out      = node;
      has_node = 1;
    } else {
      heap_set(term_val(cur) + 1, node);
    }
    cur = node;
  }

  return out;
}

fn void prim_argv_init(void) {
  prim_register("argv", 4, 1, prim_fn_argv);
}
