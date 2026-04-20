// %tcp_send_wait(tcp, bytes)
// --------------------------
// %tcp_send_wait_go_tcp(tcp, bytes)
fn Term prim_fn_tcp_send_wait(Term *args) {
  Term args0[2] = {args[0], args[1]};
  Term t = term_new_pri(table_find("tcp_send_wait_go_tcp", sizeof("tcp_send_wait_go_tcp") - 1), 2, args0);
  return wnf(t);
}

// %tcp_send_wait_go_tcp(tcp, bytes)
// ---------------------------------
// Lift `tcp` over ERA/INC/SUP; default forwards to bytes stage.
fn Term tcp_send_wait_go_tcp(Term *args) {
  Term tcp_wnf = wnf(args[0]);
  Term bytes   = args[1];

  switch (term_tag(tcp_wnf)) {
    case ERA: {
      // %tcp_send_wait_go_tcp(&{}, bytes)
      // --------------------------------- tcp-send-wait-go-tcp-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %tcp_send_wait_go_tcp(↑x, bytes)
      // -------------------------------- tcp-send-wait-go-tcp-inc
      // ↑(%tcp_send_wait(x, bytes))
      u32  inc_loc  = term_val(tcp_wnf);
      Term inner    = heap_read(inc_loc);
      Term args0[2] = {inner, bytes};
      Term next     = term_new_pri(table_find("tcp_send_wait", sizeof("tcp_send_wait") - 1), 2, args0);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %tcp_send_wait_go_tcp(&L{x,y}, bytes)
      // ------------------------------------- tcp-send-wait-go-tcp-sup
      // &L{%tcp_send_wait(x, bytes0), %tcp_send_wait(y, bytes1)}
      u32  lab      = term_ext(tcp_wnf);
      u32  sup_loc  = term_val(tcp_wnf);
      Term x        = heap_read(sup_loc + 0);
      Term y        = heap_read(sup_loc + 1);
      Copy B        = term_clone(lab, bytes);
      Term a0[2]    = {x, B.k0};
      Term a1[2]    = {y, B.k1};
      Term t0       = term_new_pri(table_find("tcp_send_wait", sizeof("tcp_send_wait") - 1), 2, a0);
      Term t1       = term_new_pri(table_find("tcp_send_wait", sizeof("tcp_send_wait") - 1), 2, a1);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %tcp_send_wait_go_tcp(tcp, bytes)
      // --------------------------------- tcp-send-wait-go-tcp-default
      // %tcp_send_wait_go_bytes(tcp, bytes)
      Term args0[2] = {tcp_wnf, bytes};
      Term t = term_new_pri(table_find("tcp_send_wait_go_bytes", sizeof("tcp_send_wait_go_bytes") - 1), 2, args0);
      return wnf(t);
    }
  }
}

// %tcp_send_wait_go_bytes(tcp, bytes)
// -----------------------------------
// Lift `bytes` over ERA/INC/SUP; default forwards to io stage.
fn Term tcp_send_wait_go_bytes(Term *args) {
  Term tcp       = args[0];
  Term bytes_wnf = wnf(args[1]);

  switch (term_tag(bytes_wnf)) {
    case ERA: {
      // %tcp_send_wait_go_bytes(tcp, &{})
      // --------------------------------- tcp-send-wait-go-bytes-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %tcp_send_wait_go_bytes(tcp, ↑x)
      // -------------------------------- tcp-send-wait-go-bytes-inc
      // ↑(%tcp_send_wait(tcp, x))
      u32  inc_loc  = term_val(bytes_wnf);
      Term inner    = heap_read(inc_loc);
      Term args0[2] = {tcp, inner};
      Term next     = term_new_pri(table_find("tcp_send_wait", sizeof("tcp_send_wait") - 1), 2, args0);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %tcp_send_wait_go_bytes(tcp, &L{x,y})
      // ------------------------------------- tcp-send-wait-go-bytes-sup
      // &L{%tcp_send_wait(tcp0, x), %tcp_send_wait(tcp1, y)}
      u32  lab      = term_ext(bytes_wnf);
      u32  sup_loc  = term_val(bytes_wnf);
      Term x        = heap_read(sup_loc + 0);
      Term y        = heap_read(sup_loc + 1);
      Copy T        = term_clone(lab, tcp);
      Term a0[2]    = {T.k0, x};
      Term a1[2]    = {T.k1, y};
      Term t0       = term_new_pri(table_find("tcp_send_wait", sizeof("tcp_send_wait") - 1), 2, a0);
      Term t1       = term_new_pri(table_find("tcp_send_wait", sizeof("tcp_send_wait") - 1), 2, a1);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %tcp_send_wait_go_bytes(tcp, bytes)
      // ----------------------------------- tcp-send-wait-go-bytes-default
      // %tcp_send_wait_go_io(tcp, bytes)
      Term args0[2] = {tcp, bytes_wnf};
      Term t = term_new_pri(table_find("tcp_send_wait_go_io", sizeof("tcp_send_wait_go_io") - 1), 2, args0);
      return wnf(t);
    }
  }
}

// %tcp_send_wait_go_io(tcp, bytes)
// --------------------------------
// #OK{#Rdy{#Tcp{id,seq+1},#Sent{n}|#TcpFail{reason,msg}}} | #ERR{String}
fn Term prim_fn_tcp_send_wait_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(args[0], &id, &seq)) {
    return tcp_new_err("tcp_send_wait", TCP_ERR_BAD_HANDLE, "invalid `tcp`; expected #Tcp{id,seq}");
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_err("tcp_send_wait", TCP_ERR_BAD_HANDLE, "unknown tcp id");
  }

  Term err = term_new_era();
  u8  *buf = NULL;
  u32  len = 0;
  if (!tcp_decode_bytes(args[1], &buf, &len, TCP_SEND_CAP, &err)) {
    return err;
  }

  TcpSnap snap;
  if (!tcp_claim(id, seq, &snap)) {
    free(buf);
    return tcp_new_err("tcp_send_wait", TCP_ERR_STALE, "stale tcp handle");
  }

  switch (snap.state) {
    case TCP_STATE_OPEN:
    case TCP_STATE_REMOTE_EOF: {
      if (snap.fd < 0) {
        free(buf);
        tcp_slot_set_fd_state(id, -1, TCP_STATE_FAILED);
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_not_connected_evt()));
      }

      if (len == 0) {
        free(buf);
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_new_sent_evt(0)));
      }

      u64 wait_start_ms = tcp_now_ms();

      while (1) {
        ssize_t sent = send(snap.fd, buf, len, MSG_NOSIGNAL);
        if (sent >= 0) {
          free(buf);
          return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_new_sent_evt((u32)sent)));
        }

        if (errno == EINTR) {
          continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          int timeout_ms = tcp_timeout_to_poll_ms(wait_start_ms, snap.write_timeout_ms);
          if (timeout_ms == 0) {
            free(buf);
            return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_timeout_evt("tcp_send")));
          }

          struct pollfd pfd = {
            .fd      = snap.fd,
            .events  = POLLOUT | POLLERR | POLLHUP,
            .revents = 0,
          };

          int polled = tcp_poll_retry(&pfd, 1, timeout_ms);
          if (polled < 0) {
            free(buf);
            return tcp_new_err("tcp_send_wait", TCP_ERR_IO, strerror(errno));
          }
          if (polled == 0) {
            free(buf);
            return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_timeout_evt("tcp_send")));
          }
          continue;
        }

        int errn = errno;
        free(buf);
        tcp_slot_close_and_set(id, snap.fd, TCP_STATE_FAILED);
        return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_from_errno_evt("tcp_send", errn)));
      }
    }
    case TCP_STATE_CONNECTING:
    case TCP_STATE_CLOSED:
    case TCP_STATE_FAILED: {
      free(buf);
      return tcp_state_not_connected(id, seq);
    }
    default: {
      free(buf);
      tcp_slot_close_and_set(id, snap.fd, TCP_STATE_FAILED);
      return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_evt(TCP_FAIL_PROTOCOL, 0, "invalid tcp state")));
    }
  }
}

fn void prim_tcp_send_wait_init(void) {
  prim_register("tcp_send_wait", sizeof("tcp_send_wait") - 1, 2, prim_fn_tcp_send_wait);
  prim_register("tcp_send_wait_go_tcp", sizeof("tcp_send_wait_go_tcp") - 1, 2, tcp_send_wait_go_tcp);
  prim_register("tcp_send_wait_go_bytes", sizeof("tcp_send_wait_go_bytes") - 1, 2, tcp_send_wait_go_bytes);
  prim_register("tcp_send_wait_go_io", sizeof("tcp_send_wait_go_io") - 1, 2, prim_fn_tcp_send_wait_go_io);
}
