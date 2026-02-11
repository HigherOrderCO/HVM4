// %tcp_connect(req)
// -----------------
// %tcp_connect_go_req(req)
fn Term prim_fn_tcp_connect(Term *args) {
  Term args0[1] = {args[0]};
  Term t = term_new_pri(table_find("tcp_connect_go_req", sizeof("tcp_connect_go_req") - 1), 1, args0);
  return wnf(t);
}

// %tcp_connect_go_req(req)
// ------------------------
// Lift `req` over ERA/INC/SUP; default forwards to io stage.
fn Term tcp_connect_go_req(Term *args) {
  Term req_wnf = wnf(args[0]);

  switch (term_tag(req_wnf)) {
    case ERA: {
      // %tcp_connect_go_req(&{})
      // ------------------------ tcp-connect-go-req-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %tcp_connect_go_req(↑x)
      // ----------------------- tcp-connect-go-req-inc
      // ↑(%tcp_connect(x))
      u32  inc_loc = term_val(req_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("tcp_connect", sizeof("tcp_connect") - 1), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %tcp_connect_go_req(&L{x,y})
      // ---------------------------- tcp-connect-go-req-sup
      // &L{%tcp_connect(x), %tcp_connect(y)}
      u32  lab     = term_ext(req_wnf);
      u32  sup_loc = term_val(req_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("tcp_connect", sizeof("tcp_connect") - 1), 1, &x);
      Term t1      = term_new_pri(table_find("tcp_connect", sizeof("tcp_connect") - 1), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %tcp_connect_go_req(req)
      // ------------------------ tcp-connect-go-req-default
      // %tcp_connect_go_io(req)
      Term args0[1] = {req_wnf};
      Term t = term_new_pri(table_find("tcp_connect_go_io", sizeof("tcp_connect_go_io") - 1), 1, args0);
      return wnf(t);
    }
  }
}

// %tcp_connect_go_io(req)
// -----------------------
// #OK{#Tcp{id,0}} | #ERR{String}
fn Term prim_fn_tcp_connect_go_io(Term *args) {
  char host[TCP_HOST_CAP];
  u32  port               = 0;
  u32  connect_timeout_ms = 0;
  u32  read_timeout_ms    = 0;
  u32  write_timeout_ms   = 0;
  u8   nodelay            = 0;
  u8   keepalive          = 0;

  Term parse_err = term_new_era();
  if (!tcp_parse_req(
    args[0],
    host,
    &port,
    &connect_timeout_ms,
    &read_timeout_ms,
    &write_timeout_ms,
    &nodelay,
    &keepalive,
    &parse_err
  )) {
    return parse_err;
  }

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo  hints;
  struct addrinfo *res      = NULL;
  struct addrinfo *it       = NULL;
  int              last_err = 0;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  int gai_err = getaddrinfo(host, port_str, &hints, &res);
  if (gai_err != 0) {
    Term evt = tcp_fail_from_gai_evt("tcp_connect", gai_err);
    u32  loc = term_val(evt);
    Term msg = heap_read(loc + 1);
    return term_new_ctr(NAM_ERR, 1, &msg);
  }

  int fd      = -1;
  u8  state   = TCP_STATE_CONNECTING;

  for (it = res; it != NULL; it = it->ai_next) {
    fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      last_err = errno;
      continue;
    }

    if (!tcp_set_nonblocking(fd)) {
      last_err = errno;
      tcp_close_fd(fd);
      fd = -1;
      continue;
    }

    if (!tcp_apply_sockopts(fd, nodelay, keepalive)) {
      last_err = errno;
      tcp_close_fd(fd);
      fd = -1;
      continue;
    }

    if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
      state = TCP_STATE_OPEN;
      break;
    }

    if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
      state = TCP_STATE_CONNECTING;
      break;
    }

    last_err = errno;
    tcp_close_fd(fd);
    fd = -1;
  }

  freeaddrinfo(res);

  if (fd < 0) {
    if (last_err == 0) {
      last_err = EIO;
    }
    return tcp_new_err("tcp_connect", TCP_ERR_IO, strerror(last_err));
  }

  pthread_mutex_lock(&TCP_LOCK);

  u32 id = TCP_NEXT_ID;
  if (id >= TCP_CAP) {
    pthread_mutex_unlock(&TCP_LOCK);
    tcp_close_fd(fd);
    return tcp_new_err("tcp_connect", TCP_ERR_FULL, "tcp table is full");
  }

  TCP_NEXT_ID = id + 1;
  TcpSlot *slot = &TCP_SLOTS[id];
  slot->expected_seq      = 0;
  slot->fd                = fd;
  slot->state             = state;
  slot->connect_timeout_ms = connect_timeout_ms;
  slot->read_timeout_ms   = read_timeout_ms;
  slot->write_timeout_ms  = write_timeout_ms;
  slot->connect_start_ms  = tcp_now_ms();

  pthread_mutex_unlock(&TCP_LOCK);
  return tcp_new_ok(tcp_new_tcp(id, 0));
}

fn void prim_tcp_connect_init(void) {
  prim_register("tcp_connect", sizeof("tcp_connect") - 1, 1, prim_fn_tcp_connect);
  prim_register("tcp_connect_go_req", sizeof("tcp_connect_go_req") - 1, 1, tcp_connect_go_req);
  prim_register("tcp_connect_go_io", sizeof("tcp_connect_go_io") - 1, 1, prim_fn_tcp_connect_go_io);
}
