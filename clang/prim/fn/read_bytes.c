fn Term read_bytes_go_path(Term *args);
fn Term read_bytes_go_chr(Term *args);
fn Term read_bytes_go_num(Term *args);

// %read_bytes(path)
// -----------------
// %read_bytes_go_path(λx.x, path)
fn Term prim_fn_read_bytes(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[2] = {acc, args[0]};
  Term t        = term_new_pri(table_find("read_bytes_go_path", 18), 2, args0);
  return wnf(t);
}

// %read_bytes_go_path(acc, list)
// ------------------------------
// Walk list shape with lifting over ERA/INC/SUP.
fn Term read_bytes_go_path(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %read_bytes_go_path(acc, &{})
      // ----------------------------- read-bytes-go-path-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %read_bytes_go_path(acc, ↑x)
      // ---------------------------- read-bytes-go-path-inc
      // ↑(%read_bytes(acc(x)))
      u32  inc_loc = term_val(list_wnf);
      Term inner   = heap_read(inc_loc);
      Term app     = term_new_app(acc, inner);
      Term next    = term_new_pri(table_find("read_bytes", 10), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %read_bytes_go_path(acc, &L{x,y})
      // --------------------------------- read-bytes-go-path-sup
      // &L{%read_bytes(acc0(x)), %read_bytes(acc1(y))}
      u32  lab     = term_ext(list_wnf);
      u32  sup_loc = term_val(list_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Copy A       = term_clone(lab, acc);
      Term app0    = term_new_app(A.k0, x);
      Term app1    = term_new_app(A.k1, y);
      Term t0      = term_new_pri(table_find("read_bytes", 10), 1, &app0);
      Term t1      = term_new_pri(table_find("read_bytes", 10), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == NAM_NIL) {
        // %read_bytes_go_path(acc, #Nil)
        // ------------------------------ read-bytes-go-path-nil
        // %read_bytes_go_io(acc(#Nil))
        Term nil = term_new_ctr(NAM_NIL, 0, 0);
        Term path = term_new_app(acc, nil);
        Term io_args[1] = {path};
        Term io = term_new_pri(table_find("read_bytes_go_io", 16), 1, io_args);
        return wnf(io);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == NAM_CON) {
        // %read_bytes_go_path(acc, #Con{h,t})
        // ----------------------------------- read-bytes-go-path-con
        // %read_bytes_go_chr(acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[3] = {acc, head, tail};
        Term t        = term_new_pri(table_find("read_bytes_go_chr", 17), 3, args0);
        return wnf(t);
      }
      // %read_bytes_go_path(acc, x)
      // ---------------------------- read-bytes-go-path-fallback
      // fallthrough default
    }
    default: {
      // %read_bytes_go_path(acc, x)
      // ---------------------------- read-bytes-go-path-default
      // %read_bytes_go_io(acc(x))
      Term path = term_new_app(acc, list_wnf);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("read_bytes_go_io", 16), 1, io_args);
      return wnf(io);
    }
  }
}

// %read_bytes_go_chr(acc, head, tail)
// ------------------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term read_bytes_go_chr(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %read_bytes_go_chr(acc, &{}, t)
      // ------------------------------- read-bytes-go-chr-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %read_bytes_go_chr(acc, ↑x, t)
      // ------------------------------- read-bytes-go-chr-inc
      // ↑(%read_bytes(acc(#Con{x, t})))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("read_bytes", 10), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %read_bytes_go_chr(acc, &L{x,y}, t)
      // ------------------------------------ read-bytes-go-chr-sup
      // &L{%read_bytes(acc0(#Con{x, t0})), %read_bytes(acc1(#Con{y, t1}))}
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
      Term t0           = term_new_pri(table_find("read_bytes", 10), 1, &app0);
      Term t1           = term_new_pri(table_find("read_bytes", 10), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == NAM_CHR) {
        // %read_bytes_go_chr(acc, #Chr{c}, t)
        // ------------------------------------ read-bytes-go-chr-chr
        // %read_bytes_go_num(acc, c, t)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[3] = {acc, code, tail};
        Term t        = term_new_pri(table_find("read_bytes_go_num", 17), 3, args0);
        return wnf(t);
      }
      // %read_bytes_go_chr(acc, h, t)
      // ------------------------------ read-bytes-go-chr-fallback
      // fallthrough default
    }
    default: {
      // %read_bytes_go_chr(acc, h, t)
      // ------------------------------ read-bytes-go-chr-default
      // %read_bytes_go_io(acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("read_bytes_go_io", 16), 1, io_args);
      return wnf(io);
    }
  }
}

// %read_bytes_go_num(acc, code, tail)
// ------------------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term read_bytes_go_num(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %read_bytes_go_num(acc, &{}, t)
      // ------------------------------- read-bytes-go-num-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %read_bytes_go_num(acc, ↑x, t)
      // ------------------------------- read-bytes-go-num-inc
      // ↑(%read_bytes(acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("read_bytes", 10), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %read_bytes_go_num(acc, &L{x,y}, t)
      // ------------------------------------ read-bytes-go-num-sup
      // &L{%read_bytes(acc0(#Con{#Chr{x}, t0})), %read_bytes(acc1(#Con{#Chr{y}, t1}))}
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
      Term t0           = term_new_pri(table_find("read_bytes", 10), 1, &app0);
      Term t1           = term_new_pri(table_find("read_bytes", 10), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %read_bytes_go_num(acc, n, t)
      // ------------------------------ read-bytes-go-num-num
      // %read_bytes_go_path(λx.acc(#Con{#Chr{n}, x}), t)
      u64  loc         = heap_alloc(1);
      Term var         = term_new_var(loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term bod         = term_new_app(acc, con);
      Term acc_next    = term_new_lam_at(loc, bod);
      Term args0[2]    = {acc_next, tail};
      Term t           = term_new_pri(table_find("read_bytes_go_path", 18), 2, args0);
      return wnf(t);
    }
    default: {
      // %read_bytes_go_num(acc, c, t)
      // ------------------------------ read-bytes-go-num-default
      // %read_bytes_go_io(acc(#Con{#Chr{c}, t}))
      Term chr = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("read_bytes_go_io", 16), 1, io_args);
      return wnf(io);
    }
  }
}

// %read_bytes_go_io(path)
// -----------------------
// #OK{List<#BYT{NUM}>} | #ERR{String}
fn Term prim_fn_read_bytes_go_io(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  const char *OPEN_PATH_ERR_FMT = "ERROR(read_bytes): failed to open path '%s': %s (errno=%d)";
  const char *READ_IO_ERR_FMT   = "ERROR(read_bytes): I/O error while reading '%s': %s (errno=%d)";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("read_bytes", "path", MAX_PATH, path_err);
  }

  FILE *file = fopen(path, "rb");
  if (!file) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(OPEN_PATH_ERR_FMT, path, strerror(err), err) });
  }

  // Initialize output list (empty file => #OK{#NIL}).
  Term Nil = term_new_ctr(NAM_NIL, 0, 0);
  unsigned char c;
  // First read distinguishes empty file (EOF) from read error.
  if (fread(&c, 1, 1, file) != 1) {
    if (ferror(file)) {
      // Capture errno before fclose because fclose may overwrite it.
      int err = errno;
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(READ_IO_ERR_FMT, path, strerror(err), err) });
    }
    // First read hit EOF: file is empty, so payload is #NIL.
    fclose(file);
    return term_new_ctr(NAM_OK, 1, &Nil);
  }

  Term byt[1] = {term_new_num(c)};
  Term h_t[2] = {term_new_ctr(NAM_BYT, 1, byt), Nil};
  Term result = term_new_ctr(NAM_CON, 2, h_t);

  // `curr` is the last #CON in the output List<#BYT{NUM}>.
  Term curr = result;
  while (fread(&c, 1, 1, file) == 1) {
    byt[0] = term_new_num(c);
    h_t[0] = term_new_ctr(NAM_BYT, 1, byt);
    // Append #CON{#BYT{NUM}, #NIL} at curr tail.
    heap_set(term_val(curr) + 1, term_new_ctr(NAM_CON, 2, h_t));
    curr = heap_read(term_val(curr) + 1);
  }

  if (ferror(file)) {
    // Capture errno before fclose because fclose may overwrite it.
    int err = errno;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(READ_IO_ERR_FMT, path, strerror(err), err) });
  }

  fclose(file);
  return term_new_ctr(NAM_OK, 1, &result);
}

fn void prim_read_bytes_init(void) {
  prim_register("read_bytes", 10, 1, prim_fn_read_bytes);
  prim_register("read_bytes_go_path", 18, 2, read_bytes_go_path);
  prim_register("read_bytes_go_chr", 17, 3, read_bytes_go_chr);
  prim_register("read_bytes_go_num", 17, 3, read_bytes_go_num);
  prim_register("read_bytes_go_io", 16, 1, prim_fn_read_bytes_go_io);
}
