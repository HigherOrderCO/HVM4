fn Term env_go_name(Term *args);
fn Term env_go_chr(Term *args);
fn Term env_go_num(Term *args);

// %env(name)
// ----------
// %env_go_name(λx.x, name)
fn Term prim_fn_env(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[2] = {acc, args[0]};
  Term t        = term_new_pri(table_find("env_go_name", 11), 2, args0);
  return wnf(t);
}

// %env_go_name(acc, list)
// -----------------------
// Walk list shape with lifting over ERA/INC/SUP.
fn Term env_go_name(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %env_go_name(acc, &{})
      // ---------------------- env-go-name-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %env_go_name(acc, ↑x)
      // --------------------- env-go-name-inc
      // ↑(%env(acc(x)))
      u32  inc_loc = term_val(list_wnf);
      Term inner   = heap_read(inc_loc);
      Term app     = term_new_app(acc, inner);
      Term next    = term_new_pri(table_find("env", 3), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %env_go_name(acc, &L{x,y})
      // -------------------------- env-go-name-sup
      // &L{%env(acc0(x)), %env(acc1(y))}
      u32  lab     = term_ext(list_wnf);
      u32  sup_loc = term_val(list_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Copy A       = term_clone(lab, acc);
      Term app0    = term_new_app(A.k0, x);
      Term app1    = term_new_app(A.k1, y);
      Term t0      = term_new_pri(table_find("env", 3), 1, &app0);
      Term t1      = term_new_pri(table_find("env", 3), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == NAM_NIL) {
        // %env_go_name(acc, #Nil)
        // ----------------------- env-go-name-nil
        // %env_go_io(acc(#Nil))
        Term nil = term_new_ctr(NAM_NIL, 0, 0);
        Term name = term_new_app(acc, nil);
        Term io_args[1] = {name};
        Term io = term_new_pri(table_find("env_go_io", 9), 1, io_args);
        return wnf(io);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == NAM_CON) {
        // %env_go_name(acc, #Con{h,t})
        // ---------------------------- env-go-name-con
        // %env_go_chr(acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[3] = {acc, head, tail};
        Term t        = term_new_pri(table_find("env_go_chr", 10), 3, args0);
        return wnf(t);
      }
      // %env_go_name(acc, x)
      // --------------------- env-go-name-fallback
      // fallthrough default
    }
    default: {
      // %env_go_name(acc, x)
      // --------------------- env-go-name-default
      // %env_go_io(acc(x))
      Term name = term_new_app(acc, list_wnf);
      Term io_args[1] = {name};
      Term io = term_new_pri(table_find("env_go_io", 9), 1, io_args);
      return wnf(io);
    }
  }
}

// %env_go_chr(acc, head, tail)
// ----------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term env_go_chr(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %env_go_chr(acc, &{}, t)
      // ------------------------ env-go-chr-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %env_go_chr(acc, ↑x, t)
      // ----------------------- env-go-chr-inc
      // ↑(%env(acc(#Con{x, t})))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("env", 3), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %env_go_chr(acc, &L{x,y}, t)
      // ---------------------------- env-go-chr-sup
      // &L{%env(acc0(#Con{x, t0})), %env(acc1(#Con{y, t1}))}
      u32  lab          = term_ext(head_wnf);
      u32  sup_loc      = term_val(head_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Term con0_args[2] = {x, T.k0};
      Term con1_args[2] = {y, T.k1};
      Term con0         = term_new_ctr(NAM_CON, 2, con0_args);
      Term con1         = term_new_ctr(NAM_CON, 2, con1_args);
      Term app0         = term_new_app(A.k0, con0);
      Term app1         = term_new_app(A.k1, con1);
      Term t0           = term_new_pri(table_find("env", 3), 1, &app0);
      Term t1           = term_new_pri(table_find("env", 3), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == NAM_CHR) {
        // %env_go_chr(acc, #Chr{c}, t)
        // ---------------------------- env-go-chr-chr
        // %env_go_num(acc, c, t)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[3] = {acc, code, tail};
        Term t        = term_new_pri(table_find("env_go_num", 10), 3, args0);
        return wnf(t);
      }
      // %env_go_chr(acc, h, t)
      // ----------------------- env-go-chr-fallback
      // fallthrough default
    }
    default: {
      // %env_go_chr(acc, h, t)
      // ----------------------- env-go-chr-default
      // %env_go_io(acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term name = term_new_app(acc, con);
      Term io_args[1] = {name};
      Term io = term_new_pri(table_find("env_go_io", 9), 1, io_args);
      return wnf(io);
    }
  }
}

// %env_go_num(acc, code, tail)
// ----------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term env_go_num(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %env_go_num(acc, &{}, t)
      // ------------------------ env-go-num-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %env_go_num(acc, ↑x, t)
      // ----------------------- env-go-num-inc
      // ↑(%env(acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("env", 3), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %env_go_num(acc, &L{x,y}, t)
      // ---------------------------- env-go-num-sup
      // &L{%env(acc0(#Con{#Chr{x}, t0})), %env(acc1(#Con{#Chr{y}, t1}))}
      u32  lab          = term_ext(code_wnf);
      u32  sup_loc      = term_val(code_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Term chr0         = term_new_ctr(NAM_CHR, 1, &x);
      Term chr1         = term_new_ctr(NAM_CHR, 1, &y);
      Term con0_args[2] = {chr0, T.k0};
      Term con1_args[2] = {chr1, T.k1};
      Term con0         = term_new_ctr(NAM_CON, 2, con0_args);
      Term con1         = term_new_ctr(NAM_CON, 2, con1_args);
      Term app0         = term_new_app(A.k0, con0);
      Term app1         = term_new_app(A.k1, con1);
      Term t0           = term_new_pri(table_find("env", 3), 1, &app0);
      Term t1           = term_new_pri(table_find("env", 3), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %env_go_num(acc, n, t)
      // ----------------------- env-go-num-num
      // %env_go_name(λx.acc(#Con{#Chr{n}, x}), t)
      u64  loc         = heap_alloc(1);
      Term var         = term_new_var(loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term bod         = term_new_app(acc, con);
      Term acc_next    = term_new_lam_at(loc, bod);
      Term args0[2]    = {acc_next, tail};
      Term t           = term_new_pri(table_find("env_go_name", 11), 2, args0);
      return wnf(t);
    }
    default: {
      // %env_go_num(acc, c, t)
      // ----------------------- env-go-num-default
      // %env_go_io(acc(#Con{#Chr{c}, t}))
      Term chr = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term name = term_new_app(acc, con);
      Term io_args[1] = {name};
      Term io = term_new_pri(table_find("env_go_io", 9), 1, io_args);
      return wnf(io);
    }
  }
}

// %env_go_io(name)
// ----------------
// #OK{String} | #ERR{String}
fn Term prim_fn_env_go_io(Term *args) {
  int MAX_NAME = 1024;
  char name[MAX_NAME]; // UTF-8 bytes
  const char *NOT_FOUND_FMT = "ERROR(env): variable '%s' not found";

  // Decode HVM env name string (#CHR list) into `name` as UTF-8 bytes.
  HStrErr name_err;
  if (!term_string_to_utf8_cstr(args[0], name, MAX_NAME, NULL, &name_err)) {
    return term_string_from_hstrerr("env", "name", MAX_NAME, name_err);
  }

  const char *value = getenv(name);
  if (value == NULL) {
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(NOT_FOUND_FMT, name) });
  }

  Term out = term_string_from_utf8(value);
  return term_new_ctr(NAM_OK, 1, &out);
}

fn void prim_env_init(void) {
  prim_register("env", 3, 1, prim_fn_env);
  prim_register("env_go_name", 11, 2, env_go_name);
  prim_register("env_go_chr", 10, 3, env_go_chr);
  prim_register("env_go_num", 10, 3, env_go_num);
  prim_register("env_go_io", 9, 1, prim_fn_env_go_io);
}
