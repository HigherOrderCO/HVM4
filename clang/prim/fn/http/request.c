// %http_request(req)
// ------------------
// %http_request_go_req(req)
fn Term prim_fn_http_request(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("http_request_go_req", 19), 1, args0);
  return wnf(t);
}

// %http_request_go_req(req)
// -------------------------
// Lift `req` over ERA/INC/SUP; default forwards to io stage.
fn Term http_request_go_req(Term *args) {
  Term req_wnf = wnf(args[0]);

  switch (term_tag(req_wnf)) {
    case ERA: {
      // %http_request_go_req(&{})
      // ------------------------- http-request-go-req-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %http_request_go_req(↑x)
      // ------------------------ http-request-go-req-inc
      // ↑(%http_request(x))
      u32  inc_loc = term_val(req_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("http_request", 12), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %http_request_go_req(&L{x,y})
      // ----------------------------- http-request-go-req-sup
      // &L{%http_request(x), %http_request(y)}
      u32  lab     = term_ext(req_wnf);
      u32  sup_loc = term_val(req_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("http_request", 12), 1, &x);
      Term t1      = term_new_pri(table_find("http_request", 12), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %http_request_go_req(req)
      // ------------------------- http-request-go-req-default
      // %http_request_go_io(req)
      Term args0[1] = {req_wnf};
      Term t        = term_new_pri(table_find("http_request_go_io", 18), 1, args0);
      return wnf(t);
    }
  }
}

// %http_request_go_io(req)
// ------------------------
// #OK{#Http{id,0}} | #ERR{String}
fn Term prim_fn_http_request_go_io(Term *args) {
  HttpReq req;
  http_req_init(&req);

  Term parse_err = term_new_era();
  if (!http_parse_request(args[0], &req, &parse_err)) {
    http_req_free(&req);
    return parse_err;
  }

  char *tmp_dir   = NULL;
  char *hdr_path  = NULL;
  char *body_path = NULL;
  char *meta_path = NULL;
  char *err_path  = NULL;
  char *req_path  = NULL;

  if (!http_make_paths(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path, &req_path)) {
    http_req_free(&req);
    return http_new_err("http_request", HTTP_ERR_IO, "failed to allocate temporary files");
  }

  if (!http_write_body_file(req_path, &req)) {
    int err = errno;
    http_req_free(&req);
    http_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path, &req_path);
    return http_new_err("http_request", HTTP_ERR_IO, strerror(err));
  }

  pid_t pid = fork();
  if (pid < 0) {
    int err = errno;
    http_req_free(&req);
    http_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path, &req_path);
    return http_new_err("http_request", HTTP_ERR_IO, strerror(err));
  }

  if (pid == 0) {
    http_child_exec_request(&req, hdr_path, body_path, meta_path, err_path, req_path);
  }

  u32 max_body_bytes = req.max_body_bytes;
  http_req_free(&req);

  pthread_mutex_lock(&HTTP_LOCK);

  u32 id = HTTP_NEXT_ID;
  if (id >= HTTP_CAP) {
    pthread_mutex_unlock(&HTTP_LOCK);

    if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
      http_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path, &req_path);
      return http_new_err("http_request", HTTP_ERR_IO, strerror(errno));
    }

    int   status = 0;
    pid_t got    = http_waitpid_retry(pid, &status, 0);
    if (got < 0 && errno != ECHILD) {
      http_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path, &req_path);
      return http_new_err("http_request", HTTP_ERR_IO, strerror(errno));
    }

    http_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path, &req_path);
    return http_new_err("http_request", HTTP_ERR_FULL, "http table is full");
  }

  HTTP_NEXT_ID = id + 1;

  HttpSlot *slot      = &HTTP_SLOTS[id];
  slot->expected_seq  = 0;
  slot->pid           = pid;
  slot->finished      = 0;
  slot->canceled      = 0;
  slot->signaled      = 0;
  slot->parsed        = 0;
  slot->code          = 0;
  slot->max_body_bytes = max_body_bytes;
  slot->outcome       = term_new_era();
  slot->tmp_dir       = tmp_dir;
  slot->hdr_path      = hdr_path;
  slot->body_path     = body_path;
  slot->meta_path     = meta_path;
  slot->err_path      = err_path;
  slot->req_path      = req_path;

  pthread_mutex_unlock(&HTTP_LOCK);
  return http_new_ok(http_new_http(id, 0));
}

fn void prim_http_request_init(void) {
  prim_register("http_request",        12, 1, prim_fn_http_request);
  prim_register("http_request_go_req", 19, 1, http_request_go_req);
  prim_register("http_request_go_io",  18, 1, prim_fn_http_request_go_io);
}
