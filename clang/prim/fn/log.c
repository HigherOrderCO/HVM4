fn Term wnf(Term term);
fn Term prim_fn_log_go_0(Term *args);
fn Term prim_fn_log_go_1(Term *args);
fn Term prim_fn_log_go_2(Term *args);

// %log(s)
// ---------------- log
// %log_go_0(λx.x, s)
fn Term prim_fn_log(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[2] = {acc, args[0]};
  Term t        = term_new_pri(table_find("log_go_0", 8), 2, args0);
  return wnf(t);
}

fn void prim_log_init(void) {
  prim_register("log", 3, 1, prim_fn_log);
  prim_register("log_go_0", 8, 2, prim_fn_log_go_0);
  prim_register("log_go_1", 8, 3, prim_fn_log_go_1);
  prim_register("log_go_2", 8, 3, prim_fn_log_go_2);
}
