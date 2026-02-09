fn Term prim_fn_log(Term *args);

// %panic(s)
// ---------------- panic
// !t = %log(s); abort
fn Term prim_fn_panic(Term *args) {
  (void)prim_fn_log(args);
  exit(1);
  return term_new_era();
}

fn void prim_panic_init(void) {
  prim_register("panic", 5, 1, prim_fn_panic);
}
