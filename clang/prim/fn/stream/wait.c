// %stream_wait(strm)
// ------------------
// %stream_wait_go_strm(strm)
fn Term prim_fn_stream_wait(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("stream_wait_go_strm", 19), 1, args0);
  return wnf(t);
}

// %stream_wait_go_strm(strm)
// --------------------------
// Lift `strm` over ERA/INC/SUP; default forwards to io stage.
fn Term stream_wait_go_strm(Term *args) {
  Term strm_wnf = wnf(args[0]);

  switch (term_tag(strm_wnf)) {
    case ERA: {
      // %stream_wait_go_strm(&{})
      // ------------------------- stream-wait-go-strm-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %stream_wait_go_strm(↑x)
      // ------------------------ stream-wait-go-strm-inc
      // ↑(%stream_wait(x))
      u32  inc_loc = term_val(strm_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("stream_wait", 11), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %stream_wait_go_strm(&L{x,y})
      // ------------------------------ stream-wait-go-strm-sup
      // &L{%stream_wait(x), %stream_wait(y)}
      u32  lab     = term_ext(strm_wnf);
      u32  sup_loc = term_val(strm_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("stream_wait", 11), 1, &x);
      Term t1      = term_new_pri(table_find("stream_wait", 11), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %stream_wait_go_strm(strm)
      // -------------------------- stream-wait-go-strm-default
      // %stream_wait_go_io(strm)
      Term args0[1] = {strm_wnf};
      Term t        = term_new_pri(table_find("stream_wait_go_io", 17), 1, args0);
      return wnf(t);
    }
  }
}

// %stream_wait_go_io(strm)
// ------------------------
// #OK{#Rdy{#Strm{id,seq+1},#Byt{n}|#Eof}} | #ERR{String}
fn Term prim_fn_stream_wait_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!stream_parse_handle(args[0], &id, &seq)) {
    return stream_new_err("stream_wait", STREAM_ERR_BAD_HANDLE, "invalid `strm`; expected #Strm{id,seq}");
  }

  if (!stream_is_valid_id(id)) {
    return stream_new_err("stream_wait", STREAM_ERR_BAD_HANDLE, "unknown stream id");
  }

  u8  kind   = 0;
  u8  closed = 0;
  int fd     = -1;
  if (!stream_claim(id, seq, &kind, &closed, &fd)) {
    return stream_new_err("stream_wait", STREAM_ERR_STALE, "stale stream handle");
  }

  if (closed) {
    return stream_new_err("stream_wait", STREAM_ERR_BAD_HANDLE, "stream is closed");
  }

  if (kind != STREAM_KIND_STDIN) {
    return stream_new_err("stream_wait", STREAM_ERR_BAD_HANDLE, "unsupported stream kind");
  }

  while (1) {
    u8  byt      = 0;
    u8  eof      = 0;
    int read_ret = stream_stdin_read(fd, -1, &byt, &eof);
    if (read_ret < 0) {
      return stream_new_err("stream_wait", STREAM_ERR_IO, strerror(errno));
    }
    if (read_ret == 0) {
      continue;
    }
    if (eof) {
      return stream_new_ok(stream_new_rdy_eof(id, seq + 1));
    }
    return stream_new_ok(stream_new_rdy_byt(id, seq + 1, byt));
  }
}

fn void prim_stream_wait_init(void) {
  prim_register("stream_wait",         11, 1, prim_fn_stream_wait);
  prim_register("stream_wait_go_strm", 19, 1, stream_wait_go_strm);
  prim_register("stream_wait_go_io",   17, 1, prim_fn_stream_wait_go_io);
}
