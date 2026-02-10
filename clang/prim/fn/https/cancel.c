// %https_cancel(http)
// -------------------
// %https_cancel_go_http(http)
fn Term prim_fn_https_cancel(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("https_cancel_go_http", 20), 1, args0);
  return wnf(t);
}

// %https_cancel_go_http(http)
// ---------------------------
// Lift `http` over ERA/INC/SUP; default forwards to io stage.
fn Term https_cancel_go_http(Term *args) {
  Term http_wnf = wnf(args[0]);

  switch (term_tag(http_wnf)) {
    case ERA: {
      // %https_cancel_go_http(&{})
      // -------------------------- https-cancel-go-http-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %https_cancel_go_http(↑x)
      // ------------------------- https-cancel-go-http-inc
      // ↑(%https_cancel(x))
      u32  inc_loc = term_val(http_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("https_cancel", 12), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %https_cancel_go_http(&L{x,y})
      // ------------------------------ https-cancel-go-http-sup
      // &L{%https_cancel(x), %https_cancel(y)}
      u32  lab     = term_ext(http_wnf);
      u32  sup_loc = term_val(http_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("https_cancel", 12), 1, &x);
      Term t1      = term_new_pri(table_find("https_cancel", 12), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %https_cancel_go_http(http)
      // --------------------------- https-cancel-go-http-default
      // %https_cancel_go_io(http)
      Term args0[1] = {http_wnf};
      Term t        = term_new_pri(table_find("https_cancel_go_io", 18), 1, args0);
      return wnf(t);
    }
  }
}

// %https_cancel_go_io(http)
// -------------------------
// #OK{#Pend{#Http{id,seq+1}}|#Rdy{#Http{id,seq+1},outcome}} | #ERR{String}
fn Term prim_fn_https_cancel_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!https_parse_handle(args[0], &id, &seq)) {
    return https_new_err("https_cancel", HTTPS_ERR_BAD_HANDLE, "invalid `http`; expected #Http{id,seq}");
  }

  if (!https_is_valid_id(id)) {
    return https_new_err("https_cancel", HTTPS_ERR_BAD_HANDLE, "unknown https id");
  }

  pid_t pid      = 0;
  u8    finished = 0;
  u8    parsed   = 0;
  u8    canceled = 0;
  u8    signaled = 0;
  u32   code     = 0;
  Term  outcome  = term_new_era();
  char *body     = NULL;
  char *meta     = NULL;
  char *err      = NULL;
  if (!https_claim(
    id,
    seq,
    &pid,
    &finished,
    &parsed,
    &canceled,
    &signaled,
    &code,
    &outcome,
    &body,
    &meta,
    &err
  )) {
    return https_new_err("https_cancel", HTTPS_ERR_STALE, "stale https handle");
  }

  if (parsed) {
    return https_new_ok(https_new_rdy(id, seq + 1, outcome));
  }

  if (finished) {
    Term done = https_parse_and_store_outcome(id, canceled, signaled, code, meta, body, err);
    return https_new_ok(https_new_rdy(id, seq + 1, done));
  }

  https_set_canceled(id);

  if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
    return https_new_err("https_cancel", HTTPS_ERR_IO, strerror(errno));
  }

  int   status = 0;
  pid_t got    = https_waitpid_retry(pid, &status, WNOHANG);
  if (got < 0) {
    if (errno == ECHILD) {
      Term done = https_parse_and_store_outcome(id, 1, 0, 0, meta, body, err);
      return https_new_ok(https_new_rdy(id, seq + 1, done));
    }
    return https_new_err("https_cancel", HTTPS_ERR_IO, strerror(errno));
  }

  if (got == 0) {
    return https_new_ok(https_new_pend(id, seq + 1));
  }

  https_status_from_wait(status, &signaled, &code);
  https_set_finished(id, signaled, code);

  Term done = https_parse_and_store_outcome(id, 1, signaled, code, meta, body, err);
  return https_new_ok(https_new_rdy(id, seq + 1, done));
}

fn void prim_https_cancel_init(void) {
  prim_register("https_cancel",         12, 1, prim_fn_https_cancel);
  prim_register("https_cancel_go_http", 20, 1, https_cancel_go_http);
  prim_register("https_cancel_go_io",   18, 1, prim_fn_https_cancel_go_io);
}
