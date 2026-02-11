// %tcp_connect_wait(tcp)
// ----------------------
// %tcp_connect_wait_go_tcp(tcp)
fn Term prim_fn_tcp_connect_wait(Term *args) {
  Term args0[1] = {args[0]};
  Term t = term_new_pri(table_find("tcp_connect_wait_go_tcp", sizeof("tcp_connect_wait_go_tcp") - 1), 1, args0);
  return wnf(t);
}

// %tcp_connect_wait_go_tcp(tcp)
// -----------------------------
// Lift `tcp` over ERA/INC/SUP; default forwards to io stage.
fn Term tcp_connect_wait_go_tcp(Term *args) {
  Term tcp_wnf = wnf(args[0]);

  switch (term_tag(tcp_wnf)) {
    case ERA: {
      // %tcp_connect_wait_go_tcp(&{})
      // ----------------------------- tcp-connect-wait-go-tcp-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %tcp_connect_wait_go_tcp(↑x)
      // ---------------------------- tcp-connect-wait-go-tcp-inc
      // ↑(%tcp_connect_wait(x))
      u32  inc_loc = term_val(tcp_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("tcp_connect_wait", sizeof("tcp_connect_wait") - 1), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %tcp_connect_wait_go_tcp(&L{x,y})
      // --------------------------------- tcp-connect-wait-go-tcp-sup
      // &L{%tcp_connect_wait(x), %tcp_connect_wait(y)}
      u32  lab     = term_ext(tcp_wnf);
      u32  sup_loc = term_val(tcp_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("tcp_connect_wait", sizeof("tcp_connect_wait") - 1), 1, &x);
      Term t1      = term_new_pri(table_find("tcp_connect_wait", sizeof("tcp_connect_wait") - 1), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %tcp_connect_wait_go_tcp(tcp)
      // ----------------------------- tcp-connect-wait-go-tcp-default
      // %tcp_connect_wait_go_io(tcp)
      Term args0[1] = {tcp_wnf};
      Term t = term_new_pri(table_find("tcp_connect_wait_go_io", sizeof("tcp_connect_wait_go_io") - 1), 1, args0);
      return wnf(t);
    }
  }
}

// %tcp_connect_wait_go_io(tcp)
// ----------------------------
// #OK{#Rdy{#Tcp{id,seq+1},#Conn{}|#Fail{reason,msg}}} | #ERR{String}
fn Term prim_fn_tcp_connect_wait_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(args[0], &id, &seq)) {
    return tcp_new_err("tcp_connect_wait", TCP_ERR_BAD_HANDLE, "invalid `tcp`; expected #Tcp{id,seq}");
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_err("tcp_connect_wait", TCP_ERR_BAD_HANDLE, "unknown tcp id");
  }

  TcpSnap snap;
  if (!tcp_claim(id, seq, &snap)) {
    return tcp_new_err("tcp_connect_wait", TCP_ERR_STALE, "stale tcp handle");
  }

  switch (snap.state) {
    case TCP_STATE_OPEN:
    case TCP_STATE_REMOTE_EOF: {
      return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_new_conn_evt()));
    }
    case TCP_STATE_CONNECTING: {
      if (snap.fd < 0) {
        tcp_slot_set_fd_state(id, -1, TCP_STATE_FAILED);
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_evt(TCP_FAIL_PROTOCOL, 0, "invalid connecting socket")));
      }

      int timeout_ms = tcp_timeout_to_poll_ms(snap.connect_start_ms, snap.connect_timeout_ms);
      if (timeout_ms == 0) {
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_timeout_evt("tcp_connect")));
      }

      struct pollfd pfd = {
        .fd      = snap.fd,
        .events  = POLLOUT | POLLERR | POLLHUP,
        .revents = 0,
      };

      int got = tcp_poll_retry(&pfd, 1, timeout_ms);
      if (got < 0) {
        return tcp_new_err("tcp_connect_wait", TCP_ERR_IO, strerror(errno));
      }
      if (got == 0) {
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_timeout_evt("tcp_connect")));
      }

      return tcp_connect_check_ready(id, seq, snap.fd);
    }
    case TCP_STATE_CLOSED:
    case TCP_STATE_FAILED: {
      return tcp_state_not_connected(id, seq);
    }
    default: {
      tcp_slot_close_and_set(id, snap.fd, TCP_STATE_FAILED);
      return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_evt(TCP_FAIL_PROTOCOL, 0, "invalid tcp state")));
    }
  }
}

fn void prim_tcp_connect_wait_init(void) {
  prim_register("tcp_connect_wait", sizeof("tcp_connect_wait") - 1, 1, prim_fn_tcp_connect_wait);
  prim_register("tcp_connect_wait_go_tcp", sizeof("tcp_connect_wait_go_tcp") - 1, 1, tcp_connect_wait_go_tcp);
  prim_register("tcp_connect_wait_go_io", sizeof("tcp_connect_wait_go_io") - 1, 1, prim_fn_tcp_connect_wait_go_io);
}
