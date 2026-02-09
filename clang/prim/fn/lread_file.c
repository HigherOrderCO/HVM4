#include <stdint.h>

fn Term wnf(Term term);

fn Term lread_file_go_0(Term *args);
fn Term lread_file_go_1(Term *args);
fn Term lread_file_go_2(Term *args);

// Call strict %read_file(path).
fn Term lread_file_call_read_file(Term path) {
  Term args0[1] = {path};
  Term t = term_new_pri(table_find("read_file", 9), 1, args0);
  return wnf(t);
}

// %lread_file(path)
// -----------------
// %lread_file_go_0(λx.x, path)
fn Term prim_fn_lread_file(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[2] = {acc, args[0]};
  Term t        = term_new_pri(table_find("lread_file_go_0", 15), 2, args0);
  return wnf(t);
}

fn Term lread_file_go_call(Term acc, Term list) {
  Term path = term_new_app(acc, list);
  return lread_file_call_read_file(path);
}

// %lread_file_go_0(acc, list)
// ---------------------------
// Walk list shape with lifting over ERA/INC/SUP.
fn Term lread_file_go_0(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %lread_file_go_0(acc, &{})
      // -------------------------- lread-file-go-0-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %lread_file_go_0(acc, ↑x)
      // ------------------------- lread-file-go-0-inc
      // ↑(%lread_file(acc(x)))
      u32  inc_loc = term_val(list_wnf);
      Term inner   = heap_read(inc_loc);
      Term app     = term_new_app(acc, inner);
      Term next    = term_new_pri(table_find("lread_file", 10), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %lread_file_go_0(acc, &L{x,y})
      // ------------------------------ lread-file-go-0-sup
      // &L{%lread_file(acc0(x)), %lread_file(acc1(y))}
      u32  lab     = term_ext(list_wnf);
      u32  sup_loc = term_val(list_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Copy A       = term_clone(lab, acc);
      Term app0    = term_new_app(A.k0, x);
      Term app1    = term_new_app(A.k1, y);
      Term t0      = term_new_pri(table_find("lread_file", 10), 1, &app0);
      Term t1      = term_new_pri(table_find("lread_file", 10), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == NAM_NIL) {
        // %lread_file_go_0(acc, #Nil)
        // ---------------------------- lread-file-go-0-nil
        // %read_file(acc(#Nil))
        Term nil = term_new_ctr(NAM_NIL, 0, 0);
        return lread_file_go_call(acc, nil);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == NAM_CON) {
        // %lread_file_go_0(acc, #Con{h,t})
        // --------------------------------- lread-file-go-0-con
        // %lread_file_go_1(acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[3] = {acc, head, tail};
        Term t        = term_new_pri(table_find("lread_file_go_1", 15), 3, args0);
        return wnf(t);
      }
      // %lread_file_go_0(acc, x)
      // ------------------------- lread-file-go-0-fallback
      // %read_file(acc(x))
      return lread_file_go_call(acc, list_wnf);
    }
    default: {
      // %lread_file_go_0(acc, x)
      // ------------------------- lread-file-go-0-default
      // %read_file(acc(x))
      return lread_file_go_call(acc, list_wnf);
    }
  }
}

// %lread_file_go_1(acc, head, tail)
// ---------------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term lread_file_go_1(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %lread_file_go_1(acc, &{}, t)
      // ----------------------------- lread-file-go-1-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %lread_file_go_1(acc, ↑x, t)
      // ------------------------------ lread-file-go-1-inc
      // ↑(%lread_file(acc(#Con{x, t})))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("lread_file", 10), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %lread_file_go_1(acc, &L{x,y}, t)
      // --------------------------------- lread-file-go-1-sup
      // &L{%lread_file(acc0(#Con{x, t0})), %lread_file(acc1(#Con{y, t1}))}
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
      Term t0           = term_new_pri(table_find("lread_file", 10), 1, &app0);
      Term t1           = term_new_pri(table_find("lread_file", 10), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == NAM_CHR) {
        // %lread_file_go_1(acc, #Chr{c}, t)
        // ---------------------------------- lread-file-go-1-chr
        // %lread_file_go_2(acc, c, t)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[3] = {acc, code, tail};
        Term t        = term_new_pri(table_find("lread_file_go_2", 15), 3, args0);
        return wnf(t);
      }
      // %lread_file_go_1(acc, h, t)
      // ---------------------------- lread-file-go-1-fallback
      // %read_file(acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      return lread_file_go_call(acc, con);
    }
    default: {
      // %lread_file_go_1(acc, h, t)
      // ---------------------------- lread-file-go-1-default
      // %read_file(acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      return lread_file_go_call(acc, con);
    }
  }
}

// %lread_file_go_2(acc, code, tail)
// ---------------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term lread_file_go_2(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %lread_file_go_2(acc, &{}, t)
      // ----------------------------- lread-file-go-2-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %lread_file_go_2(acc, ↑x, t)
      // ------------------------------ lread-file-go-2-inc
      // ↑(%lread_file(acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("lread_file", 10), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %lread_file_go_2(acc, &L{x,y}, t)
      // --------------------------------- lread-file-go-2-sup
      // &L{%lread_file(acc0(#Con{#Chr{x}, t0})), %lread_file(acc1(#Con{#Chr{y}, t1}))}
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
      Term t0           = term_new_pri(table_find("lread_file", 10), 1, &app0);
      Term t1           = term_new_pri(table_find("lread_file", 10), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %lread_file_go_2(acc, n, t)
      // ----------------------------- lread-file-go-2-num
      // %lread_file_go_0(λx.acc(#Con{#Chr{n}, x}), t)
      u64  loc      = heap_alloc(1);
      Term var      = term_new_var(loc);
      Term chr      = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con      = term_new_ctr(NAM_CON, 2, con_args);
      Term bod      = term_new_app(acc, con);
      Term acc_next = term_new_lam_at(loc, bod);
      Term args0[2]  = {acc_next, tail};
      Term t         = term_new_pri(table_find("lread_file_go_0", 15), 2, args0);
      return wnf(t);
    }
    default: {
      // %lread_file_go_2(acc, c, t)
      // ---------------------------- lread-file-go-2-default
      // %read_file(acc(#Con{#Chr{c}, t}))
      Term chr = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      return lread_file_go_call(acc, con);
    }
  }
}

fn void prim_lread_file_init(void) {
  prim_register("lread_file", 10, 1, prim_fn_lread_file);
  prim_register("lread_file_go_0", 15, 2, lread_file_go_0);
  prim_register("lread_file_go_1", 15, 3, lread_file_go_1);
  prim_register("lread_file_go_2", 15, 3, lread_file_go_2);
}
