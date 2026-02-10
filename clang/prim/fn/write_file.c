fn Term write_file_go_path(Term *args);
fn Term write_file_go_path_chr(Term *args);
fn Term write_file_go_path_num(Term *args);
fn Term write_file_go_data(Term *args);
fn Term write_file_go_data_chr(Term *args);
fn Term write_file_go_data_num(Term *args);

// %write_file(path, data)
// -----------------------
// %write_file_go_path(λx.x, path, data)
fn Term prim_fn_write_file(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[3] = {acc, args[0], args[1]};
  Term t        = term_new_pri(table_find("write_file_go_path", 18), 3, args0);
  return wnf(t);
}

// %write_file_go_path(acc, list, data)
// -------------------------------------
// Walk list shape with lifting over ERA/INC/SUP.
fn Term write_file_go_path(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);
  Term data     = args[2];

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %write_file_go_path(acc, &{}, data)
      // ------------------------------------ write-file-go-path-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %write_file_go_path(acc, ↑x, data)
      // ----------------------------------- write-file-go-path-inc
      // ↑(%write_file(acc(x), data))
      u32  inc_loc = term_val(list_wnf);
      Term inner   = heap_read(inc_loc);
      Term app     = term_new_app(acc, inner);
      Term next_args[2] = {app, data};
      Term next    = term_new_pri(table_find("write_file", 10), 2, next_args);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %write_file_go_path(acc, &L{x,y}, data)
      // ---------------------------------------- write-file-go-path-sup
      // &L{%write_file(acc0(x), data0), %write_file(acc1(y), data1)}
      u32  lab       = term_ext(list_wnf);
      u32  sup_loc   = term_val(list_wnf);
      Term x         = heap_read(sup_loc + 0);
      Term y         = heap_read(sup_loc + 1);
      Copy A         = term_clone(lab, acc);
      Copy D         = term_clone(lab, data);
      Term app0      = term_new_app(A.k0, x);
      Term app1      = term_new_app(A.k1, y);
      Term args0[2]  = {app0, D.k0};
      Term args1[2]  = {app1, D.k1};
      Term t0        = term_new_pri(table_find("write_file", 10), 2, args0);
      Term t1        = term_new_pri(table_find("write_file", 10), 2, args1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == NAM_NIL) {
        // %write_file_go_path(acc, #Nil, data)
        // ------------------------------------- write-file-go-path-nil
        // %write_file_go_data(acc(#Nil), λx.x, data)
        Term nil = term_new_ctr(NAM_NIL, 0, 0);
        Term path = term_new_app(acc, nil);
        u64  loc      = heap_alloc(1);
        Term var      = term_new_var(loc);
        Term data_acc = term_new_lam_at(loc, var);
        Term args0[3] = {path, data_acc, data};
        Term t        = term_new_pri(table_find("write_file_go_data", 18), 3, args0);
        return wnf(t);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == NAM_CON) {
        // %write_file_go_path(acc, #Con{h,t}, data)
        // ------------------------------------------ write-file-go-path-con
        // %write_file_go_path_chr(acc, h, t, data)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[4] = {acc, head, tail, data};
        Term t        = term_new_pri(table_find("write_file_go_path_chr", 22), 4, args0);
        return wnf(t);
      }
      // %write_file_go_path(acc, x, data)
      // ---------------------------------- write-file-go-path-fallback
      // fallthrough default
    }
    default: {
      // %write_file_go_path(acc, x, data)
      // ---------------------------------- write-file-go-path-default
      // %write_file_go_data(acc(x), λx.x, data)
      Term path = term_new_app(acc, list_wnf);
      u64  loc      = heap_alloc(1);
      Term var      = term_new_var(loc);
      Term data_acc = term_new_lam_at(loc, var);
      Term args0[3] = {path, data_acc, data};
      Term t        = term_new_pri(table_find("write_file_go_data", 18), 3, args0);
      return wnf(t);
    }
  }
}

// %write_file_go_path_chr(acc, head, tail, data)
// -----------------------------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term write_file_go_path_chr(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];
  Term data     = args[3];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %write_file_go_path_chr(acc, &{}, t, data)
      // ------------------------------------------- write-file-go-path-chr-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %write_file_go_path_chr(acc, ↑x, t, data)
      // ------------------------------------------ write-file-go-path-chr-inc
      // ↑(%write_file(acc(#Con{x, t}), data))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next_args[2] = {app, data};
      Term next        = term_new_pri(table_find("write_file", 10), 2, next_args);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %write_file_go_path_chr(acc, &L{x,y}, t, data)
      // ----------------------------------------------- write-file-go-path-chr-sup
      // &L{%write_file(acc0(#Con{x, t0}), data0), %write_file(acc1(#Con{y, t1}), data1)}
      u32  lab          = term_ext(head_wnf);
      u32  sup_loc      = term_val(head_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Copy D            = term_clone(lab, data);
      Term con0_args[2] = {x, T.k0};
      Term con1_args[2] = {y, T.k1};
      Term con0         = term_new_ctr(NAM_CON, 2, con0_args);
      Term con1         = term_new_ctr(NAM_CON, 2, con1_args);
      Term app0         = term_new_app(A.k0, con0);
      Term app1         = term_new_app(A.k1, con1);
      Term args0[2]     = {app0, D.k0};
      Term args1[2]     = {app1, D.k1};
      Term t0           = term_new_pri(table_find("write_file", 10), 2, args0);
      Term t1           = term_new_pri(table_find("write_file", 10), 2, args1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == NAM_CHR) {
        // %write_file_go_path_chr(acc, #Chr{c}, t, data)
        // ----------------------------------------------- write-file-go-path-chr-chr
        // %write_file_go_path_num(acc, c, t, data)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[4] = {acc, code, tail, data};
        Term t        = term_new_pri(table_find("write_file_go_path_num", 22), 4, args0);
        return wnf(t);
      }
      // %write_file_go_path_chr(acc, h, t, data)
      // ----------------------------------------- write-file-go-path-chr-fallback
      // fallthrough default
    }
    default: {
      // %write_file_go_path_chr(acc, h, t, data)
      // ----------------------------------------- write-file-go-path-chr-default
      // %write_file_go_data(acc(#Con{h, t}), λx.x, data)
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      u64  loc      = heap_alloc(1);
      Term var      = term_new_var(loc);
      Term data_acc = term_new_lam_at(loc, var);
      Term args0[3] = {path, data_acc, data};
      Term t        = term_new_pri(table_find("write_file_go_data", 18), 3, args0);
      return wnf(t);
    }
  }
}

// %write_file_go_path_num(acc, code, tail, data)
// -----------------------------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term write_file_go_path_num(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];
  Term data     = args[3];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %write_file_go_path_num(acc, &{}, t, data)
      // ------------------------------------------- write-file-go-path-num-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %write_file_go_path_num(acc, ↑x, t, data)
      // ------------------------------------------ write-file-go-path-num-inc
      // ↑(%write_file(acc(#Con{#Chr{x}, t}), data))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next_args[2] = {app, data};
      Term next        = term_new_pri(table_find("write_file", 10), 2, next_args);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %write_file_go_path_num(acc, &L{x,y}, t, data)
      // ----------------------------------------------- write-file-go-path-num-sup
      // &L{%write_file(acc0(#Con{#Chr{x}, t0}), data0), %write_file(acc1(#Con{#Chr{y}, t1}), data1)}
      u32  lab          = term_ext(code_wnf);
      u32  sup_loc      = term_val(code_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Copy D            = term_clone(lab, data);
      Term chr0         = term_new_ctr(NAM_CHR, 1, &x);
      Term chr1         = term_new_ctr(NAM_CHR, 1, &y);
      Term con0_args[2] = {chr0, T.k0};
      Term con1_args[2] = {chr1, T.k1};
      Term con0         = term_new_ctr(NAM_CON, 2, con0_args);
      Term con1         = term_new_ctr(NAM_CON, 2, con1_args);
      Term app0         = term_new_app(A.k0, con0);
      Term app1         = term_new_app(A.k1, con1);
      Term args0[2]     = {app0, D.k0};
      Term args1[2]     = {app1, D.k1};
      Term t0           = term_new_pri(table_find("write_file", 10), 2, args0);
      Term t1           = term_new_pri(table_find("write_file", 10), 2, args1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %write_file_go_path_num(acc, n, t, data)
      // ----------------------------------------- write-file-go-path-num-num
      // %write_file_go_path(λx.acc(#Con{#Chr{n}, x}), t, data)
      u64  loc         = heap_alloc(1);
      Term var         = term_new_var(loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term bod         = term_new_app(acc, con);
      Term acc_next    = term_new_lam_at(loc, bod);
      Term args0[3]    = {acc_next, tail, data};
      Term t           = term_new_pri(table_find("write_file_go_path", 18), 3, args0);
      return wnf(t);
    }
    default: {
      // %write_file_go_path_num(acc, c, t, data)
      // ----------------------------------------- write-file-go-path-num-default
      // %write_file_go_data(acc(#Con{#Chr{c}, t}), λx.x, data)
      Term chr = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      u64  loc      = heap_alloc(1);
      Term var      = term_new_var(loc);
      Term data_acc = term_new_lam_at(loc, var);
      Term args0[3] = {path, data_acc, data};
      Term t        = term_new_pri(table_find("write_file_go_data", 18), 3, args0);
      return wnf(t);
    }
  }
}

// %write_file_go_data(path, acc, list)
// ------------------------------------
// Walk data list shape with lifting over ERA/INC/SUP.
fn Term write_file_go_data(Term *args) {
  Term path     = args[0];
  Term acc      = args[1];
  Term list_wnf = wnf(args[2]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %write_file_go_data(path, acc, &{})
      // ----------------------------------- write-file-go-data-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %write_file_go_data(path, acc, ↑x)
      // ---------------------------------- write-file-go-data-inc
      // ↑(%write_file(path, acc(x)))
      u32  inc_loc   = term_val(list_wnf);
      Term inner     = heap_read(inc_loc);
      Term data      = term_new_app(acc, inner);
      Term next_args[2] = {path, data};
      Term next      = term_new_pri(table_find("write_file", 10), 2, next_args);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %write_file_go_data(path, acc, &L{x,y})
      // --------------------------------------- write-file-go-data-sup
      // &L{%write_file(path0, acc0(x)), %write_file(path1, acc1(y))}
      u32  lab       = term_ext(list_wnf);
      u32  sup_loc   = term_val(list_wnf);
      Term x         = heap_read(sup_loc + 0);
      Term y         = heap_read(sup_loc + 1);
      Copy P         = term_clone(lab, path);
      Copy A         = term_clone(lab, acc);
      Term data0     = term_new_app(A.k0, x);
      Term data1     = term_new_app(A.k1, y);
      Term args0[2]  = {P.k0, data0};
      Term args1[2]  = {P.k1, data1};
      Term t0        = term_new_pri(table_find("write_file", 10), 2, args0);
      Term t1        = term_new_pri(table_find("write_file", 10), 2, args1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == NAM_NIL) {
        // %write_file_go_data(path, acc, #Nil)
        // ------------------------------------ write-file-go-data-nil
        // %write_file_go_io(path, acc(#Nil))
        Term nil = term_new_ctr(NAM_NIL, 0, 0);
        Term data = term_new_app(acc, nil);
        Term io_args[2] = {path, data};
        Term io = term_new_pri(table_find("write_file_go_io", 16), 2, io_args);
        return wnf(io);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == NAM_CON) {
        // %write_file_go_data(path, acc, #Con{h,t})
        // ----------------------------------------- write-file-go-data-con
        // %write_file_go_data_chr(path, acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[4] = {path, acc, head, tail};
        Term t        = term_new_pri(table_find("write_file_go_data_chr", 22), 4, args0);
        return wnf(t);
      }
      // %write_file_go_data(path, acc, x)
      // ---------------------------------- write-file-go-data-fallback
      // fallthrough default
    }
    default: {
      // %write_file_go_data(path, acc, x)
      // ---------------------------------- write-file-go-data-default
      // %write_file_go_io(path, acc(x))
      Term data = term_new_app(acc, list_wnf);
      Term io_args[2] = {path, data};
      Term io = term_new_pri(table_find("write_file_go_io", 16), 2, io_args);
      return wnf(io);
    }
  }
}

// %write_file_go_data_chr(path, acc, head, tail)
// -----------------------------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term write_file_go_data_chr(Term *args) {
  Term path     = args[0];
  Term acc      = args[1];
  Term head_wnf = wnf(args[2]);
  Term tail     = args[3];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %write_file_go_data_chr(path, acc, &{}, t)
      // ------------------------------------------- write-file-go-data-chr-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %write_file_go_data_chr(path, acc, ↑x, t)
      // ------------------------------------------ write-file-go-data-chr-inc
      // ↑(%write_file(path, acc(#Con{x, t})))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term data        = term_new_app(acc, con);
      Term next_args[2] = {path, data};
      Term next        = term_new_pri(table_find("write_file", 10), 2, next_args);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %write_file_go_data_chr(path, acc, &L{x,y}, t)
      // ----------------------------------------------- write-file-go-data-chr-sup
      // &L{%write_file(path0, acc0(#Con{x, t0})), %write_file(path1, acc1(#Con{y, t1}))}
      u32  lab          = term_ext(head_wnf);
      u32  sup_loc      = term_val(head_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy P            = term_clone(lab, path);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Term con0_args[2] = {x, T.k0};
      Term con1_args[2] = {y, T.k1};
      Term con0         = term_new_ctr(NAM_CON, 2, con0_args);
      Term con1         = term_new_ctr(NAM_CON, 2, con1_args);
      Term data0        = term_new_app(A.k0, con0);
      Term data1        = term_new_app(A.k1, con1);
      Term args0[2]     = {P.k0, data0};
      Term args1[2]     = {P.k1, data1};
      Term t0           = term_new_pri(table_find("write_file", 10), 2, args0);
      Term t1           = term_new_pri(table_find("write_file", 10), 2, args1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == NAM_CHR) {
        // %write_file_go_data_chr(path, acc, #Chr{c}, t)
        // ----------------------------------------------- write-file-go-data-chr-chr
        // %write_file_go_data_num(path, acc, c, t)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[4] = {path, acc, code, tail};
        Term t        = term_new_pri(table_find("write_file_go_data_num", 22), 4, args0);
        return wnf(t);
      }
      // %write_file_go_data_chr(path, acc, h, t)
      // ----------------------------------------- write-file-go-data-chr-fallback
      // fallthrough default
    }
    default: {
      // %write_file_go_data_chr(path, acc, h, t)
      // ----------------------------------------- write-file-go-data-chr-default
      // %write_file_go_io(path, acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term data = term_new_app(acc, con);
      Term io_args[2] = {path, data};
      Term io = term_new_pri(table_find("write_file_go_io", 16), 2, io_args);
      return wnf(io);
    }
  }
}

// %write_file_go_data_num(path, acc, code, tail)
// -----------------------------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term write_file_go_data_num(Term *args) {
  Term path     = args[0];
  Term acc      = args[1];
  Term code_wnf = wnf(args[2]);
  Term tail     = args[3];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %write_file_go_data_num(path, acc, &{}, t)
      // ------------------------------------------- write-file-go-data-num-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %write_file_go_data_num(path, acc, ↑x, t)
      // ------------------------------------------ write-file-go-data-num-inc
      // ↑(%write_file(path, acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term data        = term_new_app(acc, con);
      Term next_args[2] = {path, data};
      Term next        = term_new_pri(table_find("write_file", 10), 2, next_args);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %write_file_go_data_num(path, acc, &L{x,y}, t)
      // ----------------------------------------------- write-file-go-data-num-sup
      // &L{%write_file(path0, acc0(#Con{#Chr{x}, t0})), %write_file(path1, acc1(#Con{#Chr{y}, t1}))}
      u32  lab          = term_ext(code_wnf);
      u32  sup_loc      = term_val(code_wnf);
      Term x            = heap_read(sup_loc + 0);
      Term y            = heap_read(sup_loc + 1);
      Copy P            = term_clone(lab, path);
      Copy A            = term_clone(lab, acc);
      Copy T            = term_clone(lab, tail);
      Term chr0         = term_new_ctr(NAM_CHR, 1, &x);
      Term chr1         = term_new_ctr(NAM_CHR, 1, &y);
      Term con0_args[2] = {chr0, T.k0};
      Term con1_args[2] = {chr1, T.k1};
      Term con0         = term_new_ctr(NAM_CON, 2, con0_args);
      Term con1         = term_new_ctr(NAM_CON, 2, con1_args);
      Term data0        = term_new_app(A.k0, con0);
      Term data1        = term_new_app(A.k1, con1);
      Term args0[2]     = {P.k0, data0};
      Term args1[2]     = {P.k1, data1};
      Term t0           = term_new_pri(table_find("write_file", 10), 2, args0);
      Term t1           = term_new_pri(table_find("write_file", 10), 2, args1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %write_file_go_data_num(path, acc, n, t)
      // ----------------------------------------- write-file-go-data-num-num
      // %write_file_go_data(path, λx.acc(#Con{#Chr{n}, x}), t)
      u64  loc         = heap_alloc(1);
      Term var         = term_new_var(loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term bod         = term_new_app(acc, con);
      Term acc_next    = term_new_lam_at(loc, bod);
      Term args0[3]    = {path, acc_next, tail};
      Term t           = term_new_pri(table_find("write_file_go_data", 18), 3, args0);
      return wnf(t);
    }
    default: {
      // %write_file_go_data_num(path, acc, c, t)
      // ----------------------------------------- write-file-go-data-num-default
      // %write_file_go_io(path, acc(#Con{#Chr{c}, t}))
      Term chr = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term data = term_new_app(acc, con);
      Term io_args[2] = {path, data};
      Term io = term_new_pri(table_find("write_file_go_io", 16), 2, io_args);
      return wnf(io);
    }
  }
}

// %write_file_go_io(path, data)
// -----------------------------
// #OK{#NIL} | #ERR{String}
fn Term prim_fn_write_file_go_io(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  int  data_i = 0;
  Term data_item = args[1];
  const char *DATA_EXPECTED       = "ERROR(write_file): invalid `data`; expected #NIL or #CON(#CHR{NUM}, tail)";
  const char *OPEN_PATH_ERR_FMT   = "ERROR(write_file): failed to open path '%s': %s (errno=%d)";
  const char *DATA_INVALID_CP_FMT = "ERROR(write_file): invalid UTF-32 codepoint U+%08llX at `data` index %i";
  const char *WRITE_IO_ERR_FMT    = "ERROR(write_file): I/O error while writing '%s': %s (errno=%d)";
  const char *FLUSH_ERR_FMT       = "ERROR(write_file): failed to flush '%s': %s (errno=%d)";
  const char *CLOSE_ERR_FMT       = "ERROR(write_file): failed to close '%s': %s (errno=%d)";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("write_file", "path", MAX_PATH, path_err);
  }

  FILE *file = fopen(path, "wb");
  if (!file) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(OPEN_PATH_ERR_FMT, path, strerror(err), err) });
  }

  // Write hvm4 string List<#CHR{NUM}> into file as UTF-8 bytes.
  data_item = wnf(data_item);
  while (term_tag(data_item) == C02) {
    // wnf(data_item) must be List<#CHR{c}>
    if (term_ext(data_item) != NAM_CON) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    Term head_loc = term_val(data_item);
    Term head     = heap_read(head_loc + 0);
    Term tail     = heap_read(head_loc + 1);
    head = wnf(head);

    // wnf(head) must be #CHR{c}
    if (term_tag(head) != C01 || term_ext(head) != NAM_CHR) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    Term c_loc = term_val(head);
    Term c_trm = wnf(heap_read(c_loc));

    // c in #CHR{c} must be NUM
    if (term_tag(c_trm) != NUM) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    // Encode UTF-32 codepoint (NUM) into UTF-8 bytes.
    u32 cp = term_val(c_trm);
    char cp_utf8[4];
    int n_bytes = utf8_encode_scalar(cp, cp_utf8);
    if (n_bytes < 0) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(DATA_INVALID_CP_FMT, (unsigned long long)cp, data_i) });
    }

    if (fwrite(cp_utf8, 1, (size_t)n_bytes, file) != (size_t)n_bytes) {
      // Capture errno before fclose because fclose may overwrite it.
      int err = errno;
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(WRITE_IO_ERR_FMT, path, strerror(err), err) });
    }

    data_i += 1;
    data_item = wnf(tail);
  }

  if (term_tag(data_item) != C00 || term_ext(data_item) != NAM_NIL) {
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
  }

  if (fflush(file) != 0) {
    // Capture errno before fclose because fclose may overwrite it.
    int err = errno;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(FLUSH_ERR_FMT, path, strerror(err), err) });
  }

  if (fclose(file) != 0) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(CLOSE_ERR_FMT, path, strerror(err), err) });
  }

  Term Nil = term_new_ctr(NAM_NIL, 0, 0);
  return term_new_ctr(NAM_OK, 1, &Nil);
}

fn void prim_write_file_init(void) {
  prim_register("write_file", 10, 2, prim_fn_write_file);
  prim_register("write_file_go_path", 18, 3, write_file_go_path);
  prim_register("write_file_go_path_chr", 22, 4, write_file_go_path_chr);
  prim_register("write_file_go_path_num", 22, 4, write_file_go_path_num);
  prim_register("write_file_go_data", 18, 3, write_file_go_data);
  prim_register("write_file_go_data_chr", 22, 4, write_file_go_data_chr);
  prim_register("write_file_go_data_num", 22, 4, write_file_go_data_num);
  prim_register("write_file_go_io", 16, 2, prim_fn_write_file_go_io);
}
