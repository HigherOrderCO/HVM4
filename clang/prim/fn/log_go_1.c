fn Term wnf(Term term);
fn void log_fail(const char *msg);

fn Term prim_fn_log_go_1(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %log_go_1(acc, &{}, t)
      // ---------------------- log-go-1-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %log_go_1(acc, ↑x, t)
      // --------------------- log-go-1-inc
      // ↑(%log(acc(#Con{x, t})))
      u64  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term log         = term_new_pri(table_find("log", 3), 1, &app);
      heap_set(inc_loc, log);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %log_go_1(acc, &L{x,y}, t)
      // -------------------------- log-go-1-sup
      // &L{%log(acc0(#Con{x, t0})), %log(acc1(#Con{y, t1}))}
      u32  lab          = term_ext(head_wnf);
      u64  sup_loc      = term_val(head_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Term con0_args[2] = {x, T.k0};
      Term con1_args[2] = {y, T.k1};
      Term con0         = term_new_ctr(SYM_CON, 2, con0_args);
      Term con1         = term_new_ctr(SYM_CON, 2, con1_args);
      Term app0         = term_new_app(A.k0, con0);
      Term log0         = term_new_pri(table_find("log", 3), 1, &app0);
      Term app1         = term_new_app(A.k1, con1);
      Term log1         = term_new_pri(table_find("log", 3), 1, &app1);
      return term_new_sup(lab, log0, log1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == SYM_CHR) {
        // %log_go_1(acc, #Chr{c}, t)
        // --------------------------- log-go-1-chr
        // %log_go_2(acc, c, t)
        u64  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[3] = {acc, code, tail};
        Term t        = term_new_pri(table_find("log_go_2", 8), 3, args0);
        return wnf(t);
      }
      log_fail("%log expected #Chr head");
    }
    default: {
      log_fail("%log expected #Chr head");
    }
  }
  return term_new_era();
}
