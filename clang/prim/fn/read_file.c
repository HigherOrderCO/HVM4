fn Term read_file_go_path(Term *args);
fn Term read_file_go_chr(Term *args);
fn Term read_file_go_num(Term *args);

// %read_file(path)
// -----------------
// %read_file_go_path(λx.x, path)
fn Term prim_fn_read_file(Term *args) {
  u64  loc      = heap_alloc(1);
  Term var      = term_new_var(loc);
  Term acc      = term_new_lam_at(loc, var);
  Term args0[2] = {acc, args[0]};
  Term t        = term_new_pri(table_find("read_file_go_path", 17), 2, args0);
  return wnf(t);
}

// %read_file_go_path(acc, list)
// ---------------------------
// Walk list shape with lifting over ERA/INC/SUP.
fn Term read_file_go_path(Term *args) {
  Term acc      = args[0];
  Term list_wnf = wnf(args[1]);

  switch (term_tag(list_wnf)) {
    case ERA: {
      // %read_file_go_path(acc, &{})
      // -------------------------- read-file-go-path-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %read_file_go_path(acc, ↑x)
      // ------------------------- read-file-go-path-inc
      // ↑(%read_file(acc(x)))
      u32  inc_loc = term_val(list_wnf);
      Term inner   = heap_read(inc_loc);
      Term app     = term_new_app(acc, inner);
      Term next    = term_new_pri(table_find("read_file", 9), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %read_file_go_path(acc, &L{x,y})
      // ------------------------------ read-file-go-path-sup
      // &L{%read_file(acc0(x)), %read_file(acc1(y))}
      u32  lab     = term_ext(list_wnf);
      u32  sup_loc = term_val(list_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Copy A       = term_clone(lab, acc);
      Term app0    = term_new_app(A.k0, x);
      Term app1    = term_new_app(A.k1, y);
      Term t0      = term_new_pri(table_find("read_file", 9), 1, &app0);
      Term t1      = term_new_pri(table_find("read_file", 9), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(list_wnf) == C00 && term_ext(list_wnf) == NAM_NIL) {
        // %read_file_go_path(acc, #Nil)
        // ---------------------------- read-file-go-path-nil
        // %read_file_go_io(acc(#Nil))
        Term nil = term_new_ctr(NAM_NIL, 0, 0);
        Term path = term_new_app(acc, nil);
        Term io_args[1] = {path};
        Term io = term_new_pri(table_find("read_file_go_io", 15), 1, io_args);
        return wnf(io);
      }
      if (term_tag(list_wnf) == C02 && term_ext(list_wnf) == NAM_CON) {
        // %read_file_go_path(acc, #Con{h,t})
        // --------------------------------- read-file-go-path-con
        // %read_file_go_chr(acc, h, t)
        u32  con_loc  = term_val(list_wnf);
        Term head     = heap_read(con_loc + 0);
        Term tail     = heap_read(con_loc + 1);
        Term args0[3] = {acc, head, tail};
        Term t        = term_new_pri(table_find("read_file_go_chr", 16), 3, args0);
        return wnf(t);
      }
      // %read_file_go_path(acc, x)
      // ------------------------- read-file-go-path-fallback
      // fallthrough default
    }
    default: {
      // %read_file_go_path(acc, x)
      // ------------------------- read-file-go-path-default
      // %read_file_go_io(acc(x))
      Term path = term_new_app(acc, list_wnf);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("read_file_go_io", 15), 1, io_args);
      return wnf(io);
    }
  }
}

// %read_file_go_chr(acc, head, tail)
// ---------------------------------
// Lift head over ERA/INC/SUP; on concrete #CHR{code}, continue with `code`.
fn Term read_file_go_chr(Term *args) {
  Term acc      = args[0];
  Term head_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(head_wnf)) {
    case ERA: {
      // %read_file_go_chr(acc, &{}, t)
      // ----------------------------- read-file-go-chr-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %read_file_go_chr(acc, ↑x, t)
      // ------------------------------ read-file-go-chr-inc
      // ↑(%read_file(acc(#Con{x, t})))
      u32  inc_loc     = term_val(head_wnf);
      Term inner       = heap_read(inc_loc);
      Term con_args[2] = {inner, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("read_file", 9), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %read_file_go_chr(acc, &L{x,y}, t)
      // --------------------------------- read-file-go-chr-sup
      // &L{%read_file(acc0(#Con{x, t0})), %read_file(acc1(#Con{y, t1}))}
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
      Term t0           = term_new_pri(table_find("read_file", 9), 1, &app0);
      Term t1           = term_new_pri(table_find("read_file", 9), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case C00 ... C16: {
      if (term_tag(head_wnf) == C01 && term_ext(head_wnf) == NAM_CHR) {
        // %read_file_go_chr(acc, #Chr{c}, t)
        // ---------------------------------- read-file-go-chr-chr
        // %read_file_go_num(acc, c, t)
        u32  chr_loc  = term_val(head_wnf);
        Term code     = heap_read(chr_loc + 0);
        Term args0[3] = {acc, code, tail};
        Term t        = term_new_pri(table_find("read_file_go_num", 16), 3, args0);
        return wnf(t);
      }
      // %read_file_go_chr(acc, h, t)
      // ---------------------------- read-file-go-chr-fallback
      // fallthrough default
    }
    default: {
      // %read_file_go_chr(acc, h, t)
      // ---------------------------- read-file-go-chr-default
      // %read_file_go_io(acc(#Con{h, t}))
      Term con_args[2] = {head_wnf, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("read_file_go_io", 15), 1, io_args);
      return wnf(io);
    }
  }
}

// %read_file_go_num(acc, code, tail)
// ---------------------------------
// Lift code over ERA/INC/SUP; on concrete NUM, extend accumulator and continue.
fn Term read_file_go_num(Term *args) {
  Term acc      = args[0];
  Term code_wnf = wnf(args[1]);
  Term tail     = args[2];

  switch (term_tag(code_wnf)) {
    case ERA: {
      // %read_file_go_num(acc, &{}, t)
      // ----------------------------- read-file-go-num-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %read_file_go_num(acc, ↑x, t)
      // ------------------------------ read-file-go-num-inc
      // ↑(%read_file(acc(#Con{#Chr{x}, t})))
      u32  inc_loc     = term_val(code_wnf);
      Term inner       = heap_read(inc_loc);
      Term chr         = term_new_ctr(NAM_CHR, 1, &inner);
      Term con_args[2] = {chr, tail};
      Term con         = term_new_ctr(NAM_CON, 2, con_args);
      Term app         = term_new_app(acc, con);
      Term next        = term_new_pri(table_find("read_file", 9), 1, &app);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %read_file_go_num(acc, &L{x,y}, t)
      // --------------------------------- read-file-go-num-sup
      // &L{%read_file(acc0(#Con{#Chr{x}, t0})), %read_file(acc1(#Con{#Chr{y}, t1}))}
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
      Term t0           = term_new_pri(table_find("read_file", 9), 1, &app0);
      Term t1           = term_new_pri(table_find("read_file", 9), 1, &app1);
      return term_new_sup(lab, t0, t1);
    }
    case NUM: {
      // %read_file_go_num(acc, n, t)
      // ----------------------------- read-file-go-num-num
      // %read_file_go_path(λx.acc(#Con{#Chr{n}, x}), t)
      u64  loc      = heap_alloc(1);
      Term var      = term_new_var(loc);
      Term chr      = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, var};
      Term con      = term_new_ctr(NAM_CON, 2, con_args);
      Term bod      = term_new_app(acc, con);
      Term acc_next = term_new_lam_at(loc, bod);
      Term args0[2]  = {acc_next, tail};
      Term t         = term_new_pri(table_find("read_file_go_path", 17), 2, args0);
      return wnf(t);
    }
    default: {
      // %read_file_go_num(acc, c, t)
      // ---------------------------- read-file-go-num-default
      // %read_file_go_io(acc(#Con{#Chr{c}, t}))
      Term chr = term_new_ctr(NAM_CHR, 1, &code_wnf);
      Term con_args[2] = {chr, tail};
      Term con = term_new_ctr(NAM_CON, 2, con_args);
      Term path = term_new_app(acc, con);
      Term io_args[1] = {path};
      Term io = term_new_pri(table_find("read_file_go_io", 15), 1, io_args);
      return wnf(io);
    }
  }
}

// %read_file_go_io(path)
// ----------------------
// #OK{List<#CHR{NUM}>} | #ERR{String}
fn Term prim_fn_read_file_go_io(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  const char *OPEN_PATH_ERR_FMT = "ERROR(read_file): failed to open path '%s': %s (errno=%d)";
  const char *READ_IO_ERR_FMT = "ERROR(read_file): I/O error while reading '%s': %s (errno=%d)";
  const char *INVALID_UTF8_FMT = "ERROR(read_file): invalid UTF-8 at byte index %i";
  const char *TRUNC_UTF8_FMT = "ERROR(read_file): truncated UTF-8 sequence at byte index %i";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("read_file", "path", MAX_PATH, path_err);
  }

  FILE *file = fopen(path, "rb");
  if (!file) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(OPEN_PATH_ERR_FMT, path, strerror(err), err) });
  }

  Term Nil = term_new_ctr(NAM_NIL, 0, 0);
  Term result = Nil;
  Term curr = Nil;
  u8   has_node = 0;

  // Incremental UTF-8 decoder state.
  // `seq` stores bytes of the current candidate codepoint.
  u8 seq[4];
  int seq_len = 0;
  int byte_i = 0;

  u8 b;
  while (fread(&b, 1, 1, file) == 1) {
    if (seq_len >= 4) {
      int seq_start = byte_i - seq_len;
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(INVALID_UTF8_FMT, seq_start) });
    }
    seq[seq_len] = b;
    seq_len += 1;

    // Try to decode the current sequence from byte slice (not NUL-terminated).
    u32 seq_idx = 0;
    u32 cp = 0;
    int dec = utf8_decode_next_bytes(seq, (u32)seq_len, &seq_idx, &cp);
    if (dec == -2) {
      // Need more bytes for the current codepoint.
      byte_i += 1;
      continue;
    }
    if (dec < 0) {
      int seq_start = byte_i - (seq_len - 1);
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(INVALID_UTF8_FMT, seq_start) });
    }

    Term num = term_new_num(cp);
    Term chr = term_new_ctr(NAM_CHR, 1, &num);
    Term h_t[2] = {chr, Nil};
    Term node = term_new_ctr(NAM_CON, 2, h_t);

    if (!has_node) {
      result = node;
      has_node = 1;
    } else {
      heap_set(term_val(curr) + 1, node);
    }
    curr = node;

    seq_len = 0;
    byte_i += 1;
  }

  if (ferror(file)) {
    // Capture errno before fclose because fclose may overwrite it.
    int err = errno;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(READ_IO_ERR_FMT, path, strerror(err), err) });
  }

  if (seq_len != 0) {
    int seq_start = byte_i - seq_len;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(TRUNC_UTF8_FMT, seq_start) });
  }

  fclose(file);
  return term_new_ctr(NAM_OK, 1, &result);
}

fn void prim_read_file_init(void) {
  prim_register("read_file", 9, 1, prim_fn_read_file);
  prim_register("read_file_go_path", 17, 2, read_file_go_path);
  prim_register("read_file_go_chr", 16, 3, read_file_go_chr);
  prim_register("read_file_go_num", 16, 3, read_file_go_num);
  prim_register("read_file_go_io", 15, 1, prim_fn_read_file_go_io);
}
