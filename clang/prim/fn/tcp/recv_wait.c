// %tcp_recv_wait(tcp, max_bytes)
// ------------------------------
// %tcp_recv_wait_go_tcp(tcp, max_bytes)
fn Term prim_fn_tcp_recv_wait(Term *args) {
  Term args0[2] = {args[0], args[1]};
  Term t = term_new_pri(table_find("tcp_recv_wait_go_tcp", sizeof("tcp_recv_wait_go_tcp") - 1), 2, args0);
  return wnf(t);
}

// %tcp_recv_wait_go_tcp(tcp, max_bytes)
// -------------------------------------
// Lift `tcp` over ERA/INC/SUP; default forwards to max stage.
fn Term tcp_recv_wait_go_tcp(Term *args) {
  Term tcp_wnf   = wnf(args[0]);
  Term max_bytes = args[1];

  switch (term_tag(tcp_wnf)) {
    case ERA: {
      // %tcp_recv_wait_go_tcp(&{}, max_bytes)
      // ------------------------------------- tcp-recv-wait-go-tcp-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %tcp_recv_wait_go_tcp(↑x, max_bytes)
      // ------------------------------------ tcp-recv-wait-go-tcp-inc
      // ↑(%tcp_recv_wait(x, max_bytes))
      u32  inc_loc  = term_val(tcp_wnf);
      Term inner    = heap_read(inc_loc);
      Term args0[2] = {inner, max_bytes};
      Term next     = term_new_pri(table_find("tcp_recv_wait", sizeof("tcp_recv_wait") - 1), 2, args0);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %tcp_recv_wait_go_tcp(&L{x,y}, max_bytes)
      // ----------------------------------------- tcp-recv-wait-go-tcp-sup
      // &L{%tcp_recv_wait(x, max0), %tcp_recv_wait(y, max1)}
      u32  lab      = term_ext(tcp_wnf);
      u32  sup_loc  = term_val(tcp_wnf);
      Term x        = heap_read(sup_loc + 0);
      Term y        = heap_read(sup_loc + 1);
      Copy M        = term_clone(lab, max_bytes);
      Term a0[2]    = {x, M.k0};
      Term a1[2]    = {y, M.k1};
      Term t0       = term_new_pri(table_find("tcp_recv_wait", sizeof("tcp_recv_wait") - 1), 2, a0);
      Term t1       = term_new_pri(table_find("tcp_recv_wait", sizeof("tcp_recv_wait") - 1), 2, a1);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %tcp_recv_wait_go_tcp(tcp, max_bytes)
      // ------------------------------------- tcp-recv-wait-go-tcp-default
      // %tcp_recv_wait_go_max(tcp, max_bytes)
      Term args0[2] = {tcp_wnf, max_bytes};
      Term t = term_new_pri(table_find("tcp_recv_wait_go_max", sizeof("tcp_recv_wait_go_max") - 1), 2, args0);
      return wnf(t);
    }
  }
}

// %tcp_recv_wait_go_max(tcp, max_bytes)
// -------------------------------------
// Lift `max_bytes` over ERA/INC/SUP; default forwards to io stage.
fn Term tcp_recv_wait_go_max(Term *args) {
  Term tcp     = args[0];
  Term max_wnf = wnf(args[1]);

  switch (term_tag(max_wnf)) {
    case ERA: {
      // %tcp_recv_wait_go_max(tcp, &{})
      // ------------------------------- tcp-recv-wait-go-max-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %tcp_recv_wait_go_max(tcp, ↑x)
      // ------------------------------ tcp-recv-wait-go-max-inc
      // ↑(%tcp_recv_wait(tcp, x))
      u32  inc_loc  = term_val(max_wnf);
      Term inner    = heap_read(inc_loc);
      Term args0[2] = {tcp, inner};
      Term next     = term_new_pri(table_find("tcp_recv_wait", sizeof("tcp_recv_wait") - 1), 2, args0);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %tcp_recv_wait_go_max(tcp, &L{x,y})
      // ----------------------------------- tcp-recv-wait-go-max-sup
      // &L{%tcp_recv_wait(tcp0, x), %tcp_recv_wait(tcp1, y)}
      u32  lab      = term_ext(max_wnf);
      u32  sup_loc  = term_val(max_wnf);
      Term x        = heap_read(sup_loc + 0);
      Term y        = heap_read(sup_loc + 1);
      Copy T        = term_clone(lab, tcp);
      Term a0[2]    = {T.k0, x};
      Term a1[2]    = {T.k1, y};
      Term t0       = term_new_pri(table_find("tcp_recv_wait", sizeof("tcp_recv_wait") - 1), 2, a0);
      Term t1       = term_new_pri(table_find("tcp_recv_wait", sizeof("tcp_recv_wait") - 1), 2, a1);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %tcp_recv_wait_go_max(tcp, max_bytes)
      // ------------------------------------- tcp-recv-wait-go-max-default
      // %tcp_recv_wait_go_io(tcp, max_bytes)
      Term args0[2] = {tcp, max_wnf};
      Term t = term_new_pri(table_find("tcp_recv_wait_go_io", sizeof("tcp_recv_wait_go_io") - 1), 2, args0);
      return wnf(t);
    }
  }
}

// %tcp_recv_wait_go_io(tcp, max_bytes)
// ------------------------------------
// #OK{#Rdy{#Tcp{id,seq+1},#Recv{bytes}|#Eof{}|#Fail{reason,msg}}} | #ERR{String}
fn Term prim_fn_tcp_recv_wait_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(args[0], &id, &seq)) {
    return tcp_new_err("tcp_recv_wait", TCP_ERR_BAD_HANDLE, "invalid `tcp`; expected #Tcp{id,seq}");
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_err("tcp_recv_wait", TCP_ERR_BAD_HANDLE, "unknown tcp id");
  }

  Term err = term_new_era();
  u32  max_bytes = 0;
  if (!tcp_parse_recv_max(args[1], &max_bytes, &err)) {
    return err;
  }

  TcpSnap snap;
  if (!tcp_claim(id, seq, &snap)) {
    return tcp_new_err("tcp_recv_wait", TCP_ERR_STALE, "stale tcp handle");
  }

  switch (snap.state) {
    case TCP_STATE_REMOTE_EOF: {
      return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_new_eof_evt()));
    }
    case TCP_STATE_OPEN: {
      if (snap.fd < 0) {
        tcp_slot_set_fd_state(id, -1, TCP_STATE_FAILED);
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_not_connected_evt()));
      }

      u8 *buf = malloc(max_bytes);
      if (!buf) {
        return tcp_new_err("tcp_recv_wait", TCP_ERR_IO, "out of memory");
      }

      u64 wait_start_ms = tcp_now_ms();

      while (1) {
        ssize_t got = recv(snap.fd, buf, max_bytes, 0);
        if (got > 0) {
          Term bytes = tcp_bytes_to_list(buf, (u32)got);
          free(buf);
          return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_new_recv_evt(bytes)));
        }

        if (got == 0) {
          free(buf);
          tcp_slot_set_fd_state(id, snap.fd, TCP_STATE_REMOTE_EOF);
          return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_new_eof_evt()));
        }

        if (errno == EINTR) {
          continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          int timeout_ms = tcp_timeout_to_poll_ms(wait_start_ms, snap.read_timeout_ms);
          if (timeout_ms == 0) {
            free(buf);
            return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_timeout_evt("tcp_recv")));
          }

          struct pollfd pfd = {
            .fd      = snap.fd,
            .events  = POLLIN | POLLERR | POLLHUP,
            .revents = 0,
          };

          int polled = tcp_poll_retry(&pfd, 1, timeout_ms);
          if (polled < 0) {
            free(buf);
            return tcp_new_err("tcp_recv_wait", TCP_ERR_IO, strerror(errno));
          }
          if (polled == 0) {
            free(buf);
            return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_timeout_evt("tcp_recv")));
          }
          continue;
        }

        int errn = errno;
        free(buf);
        tcp_slot_close_and_set(id, snap.fd, TCP_STATE_FAILED);
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_from_errno_evt("tcp_recv", errn)));
      }
    }
    case TCP_STATE_CONNECTING:
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

fn void prim_tcp_recv_wait_init(void) {
  prim_register("tcp_recv_wait", sizeof("tcp_recv_wait") - 1, 2, prim_fn_tcp_recv_wait);
  prim_register("tcp_recv_wait_go_tcp", sizeof("tcp_recv_wait_go_tcp") - 1, 2, tcp_recv_wait_go_tcp);
  prim_register("tcp_recv_wait_go_max", sizeof("tcp_recv_wait_go_max") - 1, 2, tcp_recv_wait_go_max);
  prim_register("tcp_recv_wait_go_io", sizeof("tcp_recv_wait_go_io") - 1, 2, prim_fn_tcp_recv_wait_go_io);
}
