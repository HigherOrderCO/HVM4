fn Term wnf(Term term);
fn void log_fail(const char *msg);

fn Term log_acc_cons(Term acc, Term code) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term chr      = term_new_ctr(SYM_CHR, 1, &code);
  Term args[2]  = {chr, var};
  Term con      = term_new_ctr(SYM_CON, 2, args);
  Term bod      = term_new_app(acc, con);
  return term_new_lam_at(loc, bod);
}

fn Term prim_fn_log_go_2(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %log_go_2(acc, &{}, t)
      // ---------------------- log-go-2-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %log_go_2(acc, ↑x, t)
      // --------------------- log-go-2-inc
      // ↑(%log(acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(SYM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term log         = term_new_pri(table_find("log", 3), 1, &app);
      heap_set(inc_loc, log);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %log_go_2(acc, &L{x,y}, t)
      // -------------------------- log-go-2-sup
      // &L{%log(acc0(#Con{#Chr{x}, t0})), %log(acc1(#Con{#Chr{y}, t1}))}
      u32  lab          = term_ext(code_wnf);
      u32  sup_loc      = term_val(code_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Term chr0         = term_new_ctr(SYM_CHR, 1, &x);
      Term chr1         = term_new_ctr(SYM_CHR, 1, &y);
      Term con0_args[2] = {chr0, T.k0};
      Term con1_args[2] = {chr1, T.k1};
      Term con0         = term_new_ctr(SYM_CON, 2, con0_args);
      Term con1         = term_new_ctr(SYM_CON, 2, con1_args);
      Term app0         = term_new_app(A.k0, con0);
      Term log0         = term_new_pri(table_find("log", 3), 1, &app0);
      Term app1         = term_new_app(A.k1, con1);
      Term log1         = term_new_pri(table_find("log", 3), 1, &app1);
      return term_new_sup(lab, log0, log1);
    }
    case NUM: {
      // %log_go_2(acc, #n, t)
      // --------------------- log-go-2-num
      // %log_go_0(λx.acc(#Con{#Chr{#n}, x}), t)
      Term acc_next  = log_acc_cons(acc, code_wnf);
      Term args0[2]  = {acc_next, tail};
      Term t         = term_new_pri(table_find("log_go_0", 8), 2, args0);
      return wnf(t);
    }
    default: {
      log_fail("%log expected a numeric codepoint");
    }
  }
  return term_new_era();
}
