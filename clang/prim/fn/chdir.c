#include <unistd.h>

fn Term chdir_go_path(Term *args);
fn Term chdir_go_chr(Term *args);
fn Term chdir_go_num(Term *args);

// %chdir(path)
// ------------
// %chdir_go_path(λx.x, path)
fn Term prim_fn_chdir(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[2] = {acc, args[0]};
  Term t        = term_new_pri(table_find("chdir_go_path", 13), 2, args0);
  return wnf(t);
}

// %chdir_go_path(acc, list)
// -------------------------
// Walk list shape with lifting over ERA/INC/SUP.
fn Term chdir_go_path(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %chdir_go_path(acc, &{})
      // ----------------------- chdir-go-path-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %chdir_go_path(acc, ↑x)
      // ----------------------- chdir-go-path-inc
      // ↑(%chdir(acc(x)))
      u32  inc_loc = term_val(list_wnf);
      Term inner   = heap_read(inc_loc);
      Term app     = term_new_app(acc, inner);
      Term next    = term_new_pri(table_find("chdir", 5), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %chdir_go_path(acc, &L{x,y})
      // ---------------------------- chdir-go-path-sup
      // &L{%chdir(acc0(x)), %chdir(acc1(y))}
      u32  lab     = term_ext(list_wnf);
      u32  sup_loc = term_val(list_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Copy A       = term_clone(lab, acc);
      Term app0    = term_new_app(A.k0, x);
      Term app1    = term_new_app(A.k1, y);
      Term t0      = term_new_pri(table_find("chdir", 5), 1, &app0);
      Term t1      = term_new_pri(table_find("chdir", 5), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == SYM_NIL) {
        // %chdir_go_path(acc, #Nil)
        // ------------------------- chdir-go-path-nil
        // %chdir_go_io(acc(#Nil))
        Term nil = term_new_ctr(SYM_NIL, 0, 0);
        Term path = term_new_app(acc, nil);
        Term io_args[1] = {path};
        Term io = term_new_pri(table_find("chdir_go_io", 11), 1, io_args);
        return wnf(io);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == SYM_CON) {
        // %chdir_go_path(acc, #Con{h,t})
        // ------------------------------ chdir-go-path-con
        // %chdir_go_chr(acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[3] = {acc, head, tail};
        Term t        = term_new_pri(table_find("chdir_go_chr", 12), 3, args0);
        return wnf(t);
      }
      // %chdir_go_path(acc, x)
      // ---------------------- chdir-go-path-fallback
      // fallthrough default
    }
    default: {
      // %chdir_go_path(acc, x)
      // ---------------------- chdir-go-path-default
      // %chdir_go_io(acc(x))
      Term path = term_new_app(acc, list_wnf);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("chdir_go_io", 11), 1, io_args);
      return wnf(io);
    }
  }
}

// %chdir_go_chr(acc, head, tail)
// ------------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term chdir_go_chr(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %chdir_go_chr(acc, &{}, t)
      // -------------------------- chdir-go-chr-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %chdir_go_chr(acc, ↑x, t)
      // ------------------------- chdir-go-chr-inc
      // ↑(%chdir(acc(#Con{x, t})))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("chdir", 5), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %chdir_go_chr(acc, &L{x,y}, t)
      // ------------------------------ chdir-go-chr-sup
      // &L{%chdir(acc0(#Con{x, t0})), %chdir(acc1(#Con{y, t1}))}
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
      Term t0           = term_new_pri(table_find("chdir", 5), 1, &app0);
      Term t1           = term_new_pri(table_find("chdir", 5), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == SYM_CHR) {
        // %chdir_go_chr(acc, #Chr{c}, t)
        // ------------------------------ chdir-go-chr-chr
        // %chdir_go_num(acc, c, t)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[3] = {acc, code, tail};
        Term t        = term_new_pri(table_find("chdir_go_num", 12), 3, args0);
        return wnf(t);
      }
      // %chdir_go_chr(acc, h, t)
      // ------------------------- chdir-go-chr-fallback
      // fallthrough default
    }
    default: {
      // %chdir_go_chr(acc, h, t)
      // ------------------------- chdir-go-chr-default
      // %chdir_go_io(acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(SYM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("chdir_go_io", 11), 1, io_args);
      return wnf(io);
    }
  }
}

// %chdir_go_num(acc, code, tail)
// ------------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term chdir_go_num(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %chdir_go_num(acc, &{}, t)
      // -------------------------- chdir-go-num-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %chdir_go_num(acc, ↑x, t)
      // ------------------------- chdir-go-num-inc
      // ↑(%chdir(acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(SYM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("chdir", 5), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %chdir_go_num(acc, &L{x,y}, t)
      // ------------------------------ chdir-go-num-sup
      // &L{%chdir(acc0(#Con{#Chr{x}, t0})), %chdir(acc1(#Con{#Chr{y}, t1}))}
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
      Term t0           = term_new_pri(table_find("chdir", 5), 1, &app0);
      Term t1           = term_new_pri(table_find("chdir", 5), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %chdir_go_num(acc, n, t)
      // ------------------------ chdir-go-num-num
      // %chdir_go_path(λx.acc(#Con{#Chr{n}, x}), t)
      u64  loc         = heap_alloc(1);
      Term var         = term_new_var(loc);
      Term chr         = term_new_ctr(SYM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con         = term_new_ctr(SYM_CON, 2, con_args);
      Term bod         = term_new_app(acc, con);
      Term acc_next    = term_new_lam_at(loc, bod);
      Term args0[2]    = {acc_next, tail};
      Term t           = term_new_pri(table_find("chdir_go_path", 13), 2, args0);
      return wnf(t);
    }
    default: {
      // %chdir_go_num(acc, c, t)
      // ------------------------ chdir-go-num-default
      // %chdir_go_io(acc(#Con{#Chr{c}, t}))
      Term chr = term_new_ctr(SYM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(SYM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("chdir_go_io", 11), 1, io_args);
      return wnf(io);
    }
  }
}

// %chdir_go_io(path)
// ------------------
// #OK{#NIL} | #ERR{String}
fn Term prim_fn_chdir_go_io(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  const char *CHDIR_ERR_FMT = "ERROR(chdir): failed to change directory to '%s': %s (errno=%d)";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("chdir", "path", MAX_PATH, path_err);
  }

  if (chdir(path) != 0) {
    int err = errno;
    return term_new_ctr(SYM_ERR, 1, (Term[]){ term_string_printf(CHDIR_ERR_FMT, path, strerror(err), err) });
  }

  Term Nil = term_new_ctr(SYM_NIL, 0, 0);
  return term_new_ctr(SYM_OK, 1, &Nil);
}

fn void prim_chdir_init(void) {
  prim_register("chdir", 5, 1, prim_fn_chdir);
  prim_register("chdir_go_path", 13, 2, chdir_go_path);
  prim_register("chdir_go_chr", 12, 3, chdir_go_chr);
  prim_register("chdir_go_num", 12, 3, chdir_go_num);
  prim_register("chdir_go_io", 11, 1, prim_fn_chdir_go_io);
}
