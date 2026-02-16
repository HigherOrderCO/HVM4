fn Term wnf(Term term);

fn void log_fail(const char *msg) {
  fprintf(stderr, "RUNTIME_ERROR: %s\n", msg);
  exit(1);
}

fn void log_print(Term acc) {
  Term cur = term_new_app(acc, term_new_ctr(SYM_NIL, 0, 0));

  while (1) {
    cur = wnf(cur);

    switch (term_tag(cur)) {
      case ERA: {
        log_fail("%log expected a string list");
      }
      case INC: {
        log_fail("%log expected a string list");
      }
      case SUP: {
        log_fail("%log expected a string list");
      }
      case C00 ... C16: {
        if (term_tag(cur) == C00 && term_ext(cur) == SYM_NIL) {
          fputc('\n', stdout);
          return;
        }
        if (term_tag(cur) == C02 && term_ext(cur) == SYM_CON) {
          u32  con_loc = term_val(cur);
          Term head    = heap_read(con_loc + 0);
          Term tail    = heap_read(con_loc + 1);
          Term head_wnf = wnf(head);
          if (!(term_tag(head_wnf) == C01 && term_ext(head_wnf) == SYM_CHR)) {
            log_fail("%log expected #Chr head");
          }
          u32  chr_loc  = term_val(head_wnf);
          Term code     = heap_read(chr_loc + 0);
          Term code_wnf  = wnf(code);
          if (term_tag(code_wnf) != NUM) {
            log_fail("%log expected a numeric codepoint");
          }
          print_utf8(stdout, term_val(code_wnf));
          cur = tail;
          continue;
        }
        log_fail("%log expected a string list");
      }
      default: {
        log_fail("%log expected a string list");
      }
    }
  }
}

fn Term prim_fn_log_go_0(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %log_go_0(acc, &{})
      // -------------------- log-go-0-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %log_go_0(acc, ↑x)
      // ------------------- log-go-0-inc
      // ↑(%log(acc(x)))
      u32  inc_loc     = term_val(list_wnf);
      Term inner       = heap_read(inc_loc);
      Term app         = term_new_app(acc, inner);
      Term log         = term_new_pri(table_find("log", 3), 1, &app);
      heap_set(inc_loc, log);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %log_go_0(acc, &L{x,y})
      // ------------------------ log-go-0-sup
      // &L{%log(acc0(x)), %log(acc1(y))}
      u32  lab         = term_ext(list_wnf);
      u32  sup_loc     = term_val(list_wnf);
      Term x           = heap_read(sup_loc + 0);
      Term y           = heap_read(sup_loc + 1);
      Copy A           = term_clone(lab, acc);
      Term app0         = term_new_app(A.k0, x);
      Term log0         = term_new_pri(table_find("log", 3), 1, &app0);
      Term app1         = term_new_app(A.k1, y);
      Term log1         = term_new_pri(table_find("log", 3), 1, &app1);
      return term_new_sup(lab, log0, log1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == SYM_NIL) {
        // %log_go_0(acc, #Nil)
        // --------------------- log-go-0-nil
        // #Nil
        log_print(acc);
        return term_new_ctr(SYM_NIL, 0, 0);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == SYM_CON) {
        // %log_go_0(acc, #Con{h,t})
        // -------------------------- log-go-0-con
        // %log_go_1(acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[3] = {acc, head, tail};
        Term t        = term_new_pri(table_find("log_go_1", 8), 3, args0);
        return wnf(t);
      }
      log_fail("%log expected a string list");
    }
    default: {
      log_fail("%log expected a string list");
    }
  }
  return term_new_era();
}
