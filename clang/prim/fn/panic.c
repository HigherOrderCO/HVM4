fn Term prim_fn_log(Term *args);
fn Term panic_go_msg(Term *args);
fn Term panic_go_chr(Term *args);
fn Term panic_go_num(Term *args);

// %panic_go_abort(s)
// ------------------
// !t = %log(s); abort
fn Term prim_fn_panic_go_abort(Term *args) {
  (void)prim_fn_log(args);
  exit(1);
  return term_new_era();
}

// %panic(s)
// ---------
// %panic_go_msg(λx.x, s)
fn Term prim_fn_panic(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[2] = {acc, args[0]};
  Term t        = term_new_pri(table_find("panic_go_msg", 12), 2, args0);
  return wnf(t);
}

// %panic_go_msg(acc, list)
// ------------------------
// Walk list shape with lifting over ERA/INC/SUP.
fn Term panic_go_msg(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %panic_go_msg(acc, &{})
      // ----------------------- panic-go-msg-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %panic_go_msg(acc, ↑x)
      // ---------------------- panic-go-msg-inc
      // ↑(%panic(acc(x)))
      u32  inc_loc = term_val(list_wnf);
      Term inner   = heap_read(inc_loc);
      Term app     = term_new_app(acc, inner);
      Term next    = term_new_pri(table_find("panic", 5), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %panic_go_msg(acc, &L{x,y})
      // --------------------------- panic-go-msg-sup
      // &L{%panic(acc0(x)), %panic(acc1(y))}
      u32  lab     = term_ext(list_wnf);
      u32  sup_loc = term_val(list_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Copy A       = term_clone(lab, acc);
      Term app0    = term_new_app(A.k0, x);
      Term app1    = term_new_app(A.k1, y);
      Term t0      = term_new_pri(table_find("panic", 5), 1, &app0);
      Term t1      = term_new_pri(table_find("panic", 5), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == SYM_NIL) {
        // %panic_go_msg(acc, #Nil)
        // ------------------------ panic-go-msg-nil
        // %panic_go_abort(acc(#Nil))
        Term nil = term_new_ctr(SYM_NIL, 0, 0);
        Term str = term_new_app(acc, nil);
        Term args0[1] = {str};
        Term t = term_new_pri(table_find("panic_go_abort", 14), 1, args0);
        return wnf(t);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == SYM_CON) {
        // %panic_go_msg(acc, #Con{h,t})
        // ----------------------------- panic-go-msg-con
        // %panic_go_chr(acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[3] = {acc, head, tail};
        Term t        = term_new_pri(table_find("panic_go_chr", 12), 3, args0);
        return wnf(t);
      }
      // %panic_go_msg(acc, x)
      // ---------------------- panic-go-msg-fallback
      // fallthrough default
    }
    default: {
      // %panic_go_msg(acc, x)
      // ---------------------- panic-go-msg-default
      // %panic_go_abort(acc(x))
      Term str = term_new_app(acc, list_wnf);
      Term args0[1] = {str};
      Term t = term_new_pri(table_find("panic_go_abort", 14), 1, args0);
      return wnf(t);
    }
  }
}

// %panic_go_chr(acc, head, tail)
// ------------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term panic_go_chr(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %panic_go_chr(acc, &{}, t)
      // -------------------------- panic-go-chr-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %panic_go_chr(acc, ↑x, t)
      // ------------------------- panic-go-chr-inc
      // ↑(%panic(acc(#Con{x, t})))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("panic", 5), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %panic_go_chr(acc, &L{x,y}, t)
      // ------------------------------ panic-go-chr-sup
      // &L{%panic(acc0(#Con{x, t0})), %panic(acc1(#Con{y, t1}))}
      u32  lab          = term_ext(head_wnf);
      u32  sup_loc      = term_val(head_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Term con0_args[2] = {x, T.k0};
      Term con1_args[2] = {y, T.k1};
      Term con0         = term_new_ctr(SYM_CON, 2, con0_args);
      Term con1         = term_new_ctr(SYM_CON, 2, con1_args);
      Term app0         = term_new_app(A.k0, con0);
      Term app1         = term_new_app(A.k1, con1);
      Term t0           = term_new_pri(table_find("panic", 5), 1, &app0);
      Term t1           = term_new_pri(table_find("panic", 5), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == SYM_CHR) {
        // %panic_go_chr(acc, #Chr{c}, t)
        // ------------------------------ panic-go-chr-chr
        // %panic_go_num(acc, c, t)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[3] = {acc, code, tail};
        Term t        = term_new_pri(table_find("panic_go_num", 12), 3, args0);
        return wnf(t);
      }
      // %panic_go_chr(acc, h, t)
      // ------------------------- panic-go-chr-fallback
      // fallthrough default
    }
    default: {
      // %panic_go_chr(acc, h, t)
      // ------------------------- panic-go-chr-default
      // %panic_go_abort(acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(SYM_CON, 2, con_args);
      Term str = term_new_app(acc, con);
      Term args0[1] = {str};
      Term t = term_new_pri(table_find("panic_go_abort", 14), 1, args0);
      return wnf(t);
    }
  }
}

// %panic_go_num(acc, code, tail)
// ------------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term panic_go_num(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %panic_go_num(acc, &{}, t)
      // -------------------------- panic-go-num-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %panic_go_num(acc, ↑x, t)
      // ------------------------- panic-go-num-inc
      // ↑(%panic(acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(SYM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("panic", 5), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %panic_go_num(acc, &L{x,y}, t)
      // ------------------------------ panic-go-num-sup
      // &L{%panic(acc0(#Con{#Chr{x}, t0})), %panic(acc1(#Con{#Chr{y}, t1}))}
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
      Term app1         = term_new_app(A.k1, con1);
      Term t0           = term_new_pri(table_find("panic", 5), 1, &app0);
      Term t1           = term_new_pri(table_find("panic", 5), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %panic_go_num(acc, n, t)
      // ------------------------- panic-go-num-num
      // %panic_go_msg(λx.acc(#Con{#Chr{n}, x}), t)
      u64  loc         = heap_alloc(1);
      Term var         = term_new_var(loc);
      Term chr         = term_new_ctr(SYM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term bod         = term_new_app(acc, con);
      Term acc_next    = term_new_lam_at(loc, bod);
      Term args0[2]    = {acc_next, tail};
      Term t           = term_new_pri(table_find("panic_go_msg", 12), 2, args0);
      return wnf(t);
    }
    default: {
      // %panic_go_num(acc, c, t)
      // ------------------------- panic-go-num-default
      // %panic_go_abort(acc(#Con{#Chr{c}, t}))
      Term chr = term_new_ctr(SYM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(SYM_CON, 2, con_args);
      Term str = term_new_app(acc, con);
      Term args0[1] = {str};
      Term t = term_new_pri(table_find("panic_go_abort", 14), 1, args0);
      return wnf(t);
    }
  }
}

fn void prim_panic_init(void) {
  prim_register("panic", 5, 1, prim_fn_panic);
  prim_register("panic_go_msg", 12, 2, panic_go_msg);
  prim_register("panic_go_chr", 12, 3, panic_go_chr);
  prim_register("panic_go_num", 12, 3, panic_go_num);
  prim_register("panic_go_abort", 14, 1, prim_fn_panic_go_abort);
}
