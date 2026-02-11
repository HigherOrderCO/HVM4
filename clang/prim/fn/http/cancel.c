// %http_cancel(http)
// ------------------
// %http_cancel_go_http(http)
fn Term prim_fn_http_cancel(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("http_cancel_go_http", 19), 1, args0);
  return wnf(t);
}

// %http_cancel_go_http(http)
// --------------------------
// Lift `http` over ERA/INC/SUP; default forwards to io stage.
fn Term http_cancel_go_http(Term *args) {
  Term http_wnf = wnf(args[0]);

  switch (term_tag(http_wnf)) {
    case ERA: {
      // %http_cancel_go_http(&{})
      // ------------------------- http-cancel-go-http-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %http_cancel_go_http(↑x)
      // ------------------------ http-cancel-go-http-inc
      // ↑(%http_cancel(x))
      u32  inc_loc = term_val(http_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("http_cancel", 11), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %http_cancel_go_http(&L{x,y})
      // ----------------------------- http-cancel-go-http-sup
      // &L{%http_cancel(x), %http_cancel(y)}
      u32  lab     = term_ext(http_wnf);
      u32  sup_loc = term_val(http_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("http_cancel", 11), 1, &x);
      Term t1      = term_new_pri(table_find("http_cancel", 11), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %http_cancel_go_http(http)
      // -------------------------- http-cancel-go-http-default
      // %http_cancel_go_io(http)
      Term args0[1] = {http_wnf};
      Term t        = term_new_pri(table_find("http_cancel_go_io", 17), 1, args0);
      return wnf(t);
    }
  }
}

// %http_cancel_go_io(http)
// ------------------------
// #OK{#Pend{#Http{id,seq+1}}|#Rdy{#Http{id,seq+1},outcome}} | #ERR{String}
fn Term prim_fn_http_cancel_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!http_parse_handle(args[0], &id, &seq)) {
    return http_new_err("http_cancel", HTTP_ERR_BAD_HANDLE, "invalid `http`; expected #Http{id,seq}");
  }

  if (!http_is_valid_id(id)) {
    return http_new_err("http_cancel", HTTP_ERR_BAD_HANDLE, "unknown http id");
  }

  pid_t pid            = 0;
  u8    finished       = 0;
  u8    parsed         = 0;
  u8    canceled       = 0;
  u8    signaled       = 0;
  u32   code           = 0;
  u32   max_body_bytes = 0;
  Term  outcome        = term_new_era();
  char *hdr            = NULL;
  char *meta           = NULL;
  char *body           = NULL;
  char *err            = NULL;
  if (!http_claim(
    id,
    seq,
    &pid,
    &finished,
    &parsed,
    &canceled,
    &signaled,
    &code,
    &max_body_bytes,
    &outcome,
    &hdr,
    &meta,
    &body,
    &err
  )) {
    return http_new_err("http_cancel", HTTP_ERR_STALE, "stale http handle");
  }

  if (parsed) {
    return http_new_ok(http_new_rdy(id, seq + 1, outcome));
  }

  if (finished) {
    Term done = http_parse_and_store_outcome(id, canceled, signaled, code, max_body_bytes, hdr, meta, body, err);
    return http_new_ok(http_new_rdy(id, seq + 1, done));
  }

  http_set_canceled(id);

  if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
    return http_new_err("http_cancel", HTTP_ERR_IO, strerror(errno));
  }

  int   status = 0;
  pid_t got    = http_waitpid_retry(pid, &status, WNOHANG);
  if (got < 0) {
    if (errno == ECHILD) {
      Term done = http_parse_and_store_outcome(id, 1, 0, 0, max_body_bytes, hdr, meta, body, err);
      return http_new_ok(http_new_rdy(id, seq + 1, done));
    }
    return http_new_err("http_cancel", HTTP_ERR_IO, strerror(errno));
  }

  if (got == 0) {
    return http_new_ok(http_new_pend(id, seq + 1));
  }

  http_status_from_wait(status, &signaled, &code);
  http_set_finished(id, signaled, code);

  Term done = http_parse_and_store_outcome(id, 1, signaled, code, max_body_bytes, hdr, meta, body, err);
  return http_new_ok(http_new_rdy(id, seq + 1, done));
}

fn void prim_http_cancel_init(void) {
  prim_register("http_cancel",         11, 1, prim_fn_http_cancel);
  prim_register("http_cancel_go_http", 19, 1, http_cancel_go_http);
  prim_register("http_cancel_go_io",   17, 1, prim_fn_http_cancel_go_io);
}
