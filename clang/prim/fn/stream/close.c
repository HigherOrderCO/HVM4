// %stream_close(strm)
// -------------------
// %stream_close_go_strm(strm)
fn Term prim_fn_stream_close(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("stream_close_go_strm", 20), 1, args0);
  return wnf(t);
}

// %stream_close_go_strm(strm)
// ---------------------------
// Lift `strm` over ERA/INC/SUP; default forwards to io stage.
fn Term stream_close_go_strm(Term *args) {
  Term strm_wnf = wnf(args[0]);

  switch (term_tag(strm_wnf)) {
    case ERA: {
      // %stream_close_go_strm(&{})
      // -------------------------- stream-close-go-strm-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %stream_close_go_strm(↑x)
      // ------------------------- stream-close-go-strm-inc
      // ↑(%stream_close(x))
      u32  inc_loc = term_val(strm_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("stream_close", 12), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %stream_close_go_strm(&L{x,y})
      // ------------------------------- stream-close-go-strm-sup
      // &L{%stream_close(x), %stream_close(y)}
      u32  lab     = term_ext(strm_wnf);
      u32  sup_loc = term_val(strm_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("stream_close", 12), 1, &x);
      Term t1      = term_new_pri(table_find("stream_close", 12), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %stream_close_go_strm(strm)
      // --------------------------- stream-close-go-strm-default
      // %stream_close_go_io(strm)
      Term args0[1] = {strm_wnf};
      Term t        = term_new_pri(table_find("stream_close_go_io", 18), 1, args0);
      return wnf(t);
    }
  }
}

// %stream_close_go_io(strm)
// -------------------------
// #OK{#Nil} | #ERR{String}
fn Term prim_fn_stream_close_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!stream_parse_handle(args[0], &id, &seq)) {
    return stream_new_err("stream_close", STREAM_ERR_BAD_HANDLE, "invalid `strm`; expected #Strm{id,seq}");
  }

  if (!stream_is_valid_id(id)) {
    return stream_new_err("stream_close", STREAM_ERR_BAD_HANDLE, "unknown stream id");
  }

  u8  kind   = 0;
  u8  closed = 0;
  int fd     = -1;
  if (!stream_claim(id, seq, &kind, &closed, &fd)) {
    return stream_new_err("stream_close", STREAM_ERR_STALE, "stale stream handle");
  }

  if (closed) {
    return stream_new_err("stream_close", STREAM_ERR_BAD_HANDLE, "stream is closed");
  }

  if (kind == STREAM_KIND_FILE) {
    while (close(fd) < 0) {
      if (errno == EINTR) {
        continue;
      }
      stream_set_closed(id);
      return stream_new_err("stream_close", STREAM_ERR_IO, strerror(errno));
    }
  }

  stream_set_closed(id);
  Term nil = term_new_ctr(SYM_NIL, 0, NULL);
  return stream_new_ok(nil);
}

fn void prim_stream_close_init(void) {
  prim_register("stream_close",         12, 1, prim_fn_stream_close);
  prim_register("stream_close_go_strm", 20, 1, stream_close_go_strm);
  prim_register("stream_close_go_io",   18, 1, prim_fn_stream_close_go_io);
}
