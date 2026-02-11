#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <limits.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define TCP_CAP       (1u << 20)
#define TCP_HOST_CAP  1024
#define TCP_RECV_CAP  (1u << 20)
#define TCP_SEND_CAP  (1u << 20)

#define TCP_ERR_BAD_ARG    1
#define TCP_ERR_BAD_HANDLE 2
#define TCP_ERR_STALE      3
#define TCP_ERR_FULL       4
#define TCP_ERR_IO         5

#define TCP_STATE_CONNECTING 1
#define TCP_STATE_OPEN       2
#define TCP_STATE_REMOTE_EOF 3
#define TCP_STATE_CLOSED     4
#define TCP_STATE_FAILED     5

#define TCP_FAIL_TIMEOUT       1
#define TCP_FAIL_DNS           2
#define TCP_FAIL_REFUSED       3
#define TCP_FAIL_UNREACHABLE   4
#define TCP_FAIL_RESET         5
#define TCP_FAIL_BROKEN_PIPE   6
#define TCP_FAIL_PROTOCOL      7
#define TCP_FAIL_NOT_CONNECTED 8
#define TCP_FAIL_SYS           9

typedef struct {
  u32 expected_seq;
  int fd;
  u8  state;
  u32 connect_timeout_ms;
  u32 read_timeout_ms;
  u32 write_timeout_ms;
  u64 connect_start_ms;
} TcpSlot;

typedef struct {
  int fd;
  u8  state;
  u32 connect_timeout_ms;
  u32 read_timeout_ms;
  u32 write_timeout_ms;
  u64 connect_start_ms;
} TcpSnap;

static TcpSlot TCP_SLOTS[TCP_CAP];
static u32     TCP_NEXT_ID = 1;
static pthread_mutex_t TCP_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 TCP_NAM_TCP      = 0;
static u32 TCP_NAM_TCP_REQ  = 0;
static u32 TCP_NAM_TCP_OPTS = 0;

static u32 TCP_NAM_PEND   = 0;
static u32 TCP_NAM_RDY    = 0;
static u32 TCP_NAM_CONN   = 0;
static u32 TCP_NAM_RECV   = 0;
static u32 TCP_NAM_SENT   = 0;
static u32 TCP_NAM_EOF    = 0;
static u32 TCP_NAM_CLOSED = 0;
static u32 TCP_NAM_FAIL   = 0;

static u32 TCP_NAM_TIMEOUT       = 0;
static u32 TCP_NAM_DNS           = 0;
static u32 TCP_NAM_REFUSED       = 0;
static u32 TCP_NAM_UNREACHABLE   = 0;
static u32 TCP_NAM_RESET         = 0;
static u32 TCP_NAM_BROKEN_PIPE   = 0;
static u32 TCP_NAM_PROTOCOL      = 0;
static u32 TCP_NAM_NOT_CONNECTED = 0;
static u32 TCP_NAM_SYS           = 0;

static u32 TCP_NAM_T = 0;
static u32 TCP_NAM_F = 0;

fn Term wnf(Term term);

fn Term tcp_new_err(const char *prim, u32 code, const char *msg) {
  Term txt = term_string_printf("ERROR(%s): E%u %s", prim, code, msg);
  return term_new_ctr(NAM_ERR, 1, &txt);
}

fn Term tcp_new_ok(Term val) {
  return term_new_ctr(NAM_OK, 1, &val);
}

fn Term tcp_new_tcp(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(TCP_NAM_TCP, 2, args);
}

fn Term tcp_new_pend(u32 id, u32 seq) {
  Term tcp = tcp_new_tcp(id, seq);
  return term_new_ctr(TCP_NAM_PEND, 1, &tcp);
}

fn Term tcp_new_rdy(u32 id, u32 seq, Term evt) {
  Term tcp = tcp_new_tcp(id, seq);
  Term args[2] = {tcp, evt};
  return term_new_ctr(TCP_NAM_RDY, 2, args);
}

fn Term tcp_new_conn_evt(void) {
  return term_new_ctr(TCP_NAM_CONN, 0, NULL);
}

fn Term tcp_new_recv_evt(Term bytes) {
  return term_new_ctr(TCP_NAM_RECV, 1, &bytes);
}

fn Term tcp_new_sent_evt(u32 n) {
  Term arg = term_new_num(n);
  return term_new_ctr(TCP_NAM_SENT, 1, &arg);
}

fn Term tcp_new_eof_evt(void) {
  return term_new_ctr(TCP_NAM_EOF, 0, NULL);
}

fn Term tcp_new_closed_evt(void) {
  return term_new_ctr(TCP_NAM_CLOSED, 0, NULL);
}

fn Term tcp_new_reason(u8 kind, u32 code) {
  switch (kind) {
    case TCP_FAIL_TIMEOUT: {
      return term_new_ctr(TCP_NAM_TIMEOUT, 0, NULL);
    }
    case TCP_FAIL_DNS: {
      return term_new_ctr(TCP_NAM_DNS, 0, NULL);
    }
    case TCP_FAIL_REFUSED: {
      return term_new_ctr(TCP_NAM_REFUSED, 0, NULL);
    }
    case TCP_FAIL_UNREACHABLE: {
      return term_new_ctr(TCP_NAM_UNREACHABLE, 0, NULL);
    }
    case TCP_FAIL_RESET: {
      return term_new_ctr(TCP_NAM_RESET, 0, NULL);
    }
    case TCP_FAIL_BROKEN_PIPE: {
      return term_new_ctr(TCP_NAM_BROKEN_PIPE, 0, NULL);
    }
    case TCP_FAIL_PROTOCOL: {
      return term_new_ctr(TCP_NAM_PROTOCOL, 0, NULL);
    }
    case TCP_FAIL_NOT_CONNECTED: {
      return term_new_ctr(TCP_NAM_NOT_CONNECTED, 0, NULL);
    }
    default: {
      Term arg = term_new_num(code);
      return term_new_ctr(TCP_NAM_SYS, 1, &arg);
    }
  }
}

fn Term tcp_new_fail_evt(Term reason, Term msg) {
  Term args[2] = {reason, msg};
  return term_new_ctr(TCP_NAM_FAIL, 2, args);
}

fn Term tcp_fail_evt(u8 kind, u32 code, const char *msg) {
  Term reason = tcp_new_reason(kind, code);
  Term text   = term_string_from_utf8(msg);
  return tcp_new_fail_evt(reason, text);
}

fn Term tcp_fail_not_connected_evt(void) {
  return tcp_fail_evt(TCP_FAIL_NOT_CONNECTED, 0, "socket is not connected");
}

fn Term tcp_fail_timeout_evt(const char *op) {
  Term reason = tcp_new_reason(TCP_FAIL_TIMEOUT, 0);
  Term msg    = term_string_printf("%s timeout", op);
  return tcp_new_fail_evt(reason, msg);
}

fn Term tcp_fail_from_errno_evt(const char *op, int err) {
  u8 kind = TCP_FAIL_SYS;

  switch (err) {
    case ETIMEDOUT: {
      kind = TCP_FAIL_TIMEOUT;
      break;
    }
    case ECONNREFUSED: {
      kind = TCP_FAIL_REFUSED;
      break;
    }
    case EHOSTUNREACH:
    case ENETUNREACH: {
      kind = TCP_FAIL_UNREACHABLE;
      break;
    }
    case ECONNRESET: {
      kind = TCP_FAIL_RESET;
      break;
    }
    case EPIPE: {
      kind = TCP_FAIL_BROKEN_PIPE;
      break;
    }
    case ENOTCONN: {
      kind = TCP_FAIL_NOT_CONNECTED;
      break;
    }
    case EPROTO:
    case EPROTONOSUPPORT:
    case EPROTOTYPE: {
      kind = TCP_FAIL_PROTOCOL;
      break;
    }
    default: {
      kind = TCP_FAIL_SYS;
      break;
    }
  }

  Term reason = tcp_new_reason(kind, (u32)err);
  Term msg    = term_string_printf("%s: %s (errno=%d)", op, strerror(err), err);
  return tcp_new_fail_evt(reason, msg);
}

fn Term tcp_fail_from_gai_evt(const char *op, int gai_err) {
  if (gai_err == EAI_NONAME || gai_err == EAI_AGAIN) {
    Term reason = tcp_new_reason(TCP_FAIL_DNS, 0);
    Term msg    = term_string_printf("%s: %s (gai=%d)", op, gai_strerror(gai_err), gai_err);
    return tcp_new_fail_evt(reason, msg);
  }

  Term reason = tcp_new_reason(TCP_FAIL_SYS, (u32)(gai_err < 0 ? -gai_err : gai_err));
  Term msg    = term_string_printf("%s: %s (gai=%d)", op, gai_strerror(gai_err), gai_err);
  return tcp_new_fail_evt(reason, msg);
}

fn u64 tcp_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull;
}

fn int tcp_timeout_to_poll_ms(u64 start_ms, u32 timeout_ms) {
  if (timeout_ms == 0) {
    return -1;
  }

  u64 now = tcp_now_ms();
  if (now >= start_ms + (u64)timeout_ms) {
    return 0;
  }

  u64 rem = (start_ms + (u64)timeout_ms) - now;
  if (rem > (u64)INT_MAX) {
    return INT_MAX;
  }

  return (int)rem;
}

fn u8 tcp_parse_num(Term term, u32 *out) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case NUM: {
      *out = term_val(val);
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 tcp_parse_bool(Term term, u8 *out) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C00: {
      u32 ext = term_ext(val);
      if (ext == TCP_NAM_T) {
        *out = 1;
        return 1;
      }
      if (ext == TCP_NAM_F) {
        *out = 0;
        return 1;
      }
      return 0;
    }
    default: {
      return 0;
    }
  }
}

fn u8 tcp_parse_handle(Term term, u32 *id, u32 *seq) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C02: {
      if (term_ext(val) != TCP_NAM_TCP) {
        return 0;
      }

      u32  loc    = term_val(val);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);

      if (!tcp_parse_num(id_tm, id)) {
        return 0;
      }
      if (!tcp_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 tcp_is_valid_id(u32 id) {
  pthread_mutex_lock(&TCP_LOCK);

  if (id == 0 || id >= TCP_NEXT_ID || id >= TCP_CAP) {
    pthread_mutex_unlock(&TCP_LOCK);
    return 0;
  }

  pthread_mutex_unlock(&TCP_LOCK);
  return 1;
}

fn u8 tcp_claim(u32 id, u32 seq, TcpSnap *out) {
  pthread_mutex_lock(&TCP_LOCK);

  if (id == 0 || id >= TCP_NEXT_ID || id >= TCP_CAP) {
    pthread_mutex_unlock(&TCP_LOCK);
    return 0;
  }

  TcpSlot *slot = &TCP_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&TCP_LOCK);
    return 0;
  }

  slot->expected_seq     = seq + 1;
  out->fd                = slot->fd;
  out->state             = slot->state;
  out->connect_timeout_ms = slot->connect_timeout_ms;
  out->read_timeout_ms   = slot->read_timeout_ms;
  out->write_timeout_ms  = slot->write_timeout_ms;
  out->connect_start_ms  = slot->connect_start_ms;

  pthread_mutex_unlock(&TCP_LOCK);
  return 1;
}

fn void tcp_slot_set_fd_state(u32 id, int fd, u8 state) {
  pthread_mutex_lock(&TCP_LOCK);

  if (id != 0 && id < TCP_NEXT_ID && id < TCP_CAP) {
    TcpSlot *slot = &TCP_SLOTS[id];
    slot->fd      = fd;
    slot->state   = state;
  }

  pthread_mutex_unlock(&TCP_LOCK);
}

fn int tcp_close_fd(int fd) {
  while (1) {
    if (close(fd) == 0) {
      return 1;
    }
    if (errno == EINTR) {
      continue;
    }
    return 0;
  }
}

fn void tcp_slot_close_and_set(u32 id, int fd, u8 state) {
  if (fd >= 0) {
    tcp_close_fd(fd);
  }
  tcp_slot_set_fd_state(id, -1, state);
}

fn int tcp_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return 0;
  }

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return 0;
  }

  return 1;
}

fn int tcp_apply_sockopts(int fd, u8 nodelay, u8 keepalive) {
#ifdef SO_NOSIGPIPE
  {
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) < 0) {
      return 0;
    }
  }
#endif

  if (nodelay) {
    int on = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
      return 0;
    }
  }

  if (keepalive) {
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0) {
      return 0;
    }
  }

  return 1;
}

fn u8 tcp_parse_req(
  Term req_tm,
  char host[TCP_HOST_CAP],
  u32 *port,
  u32 *connect_timeout_ms,
  u32 *read_timeout_ms,
  u32 *write_timeout_ms,
  u8  *nodelay,
  u8  *keepalive,
  Term *err_out
) {
  Term req = wnf(req_tm);

  if (term_tag(req) != C03 || term_ext(req) != TCP_NAM_TCP_REQ) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `req`; expected #TcpReq{host,port,opts}");
    return 0;
  }

  u32  req_loc  = term_val(req);
  Term host_tm  = heap_read(req_loc + 0);
  Term port_tm  = heap_read(req_loc + 1);
  Term opts_tm  = heap_read(req_loc + 2);

  HStrErr host_err;
  if (!term_string_to_utf8_cstr(host_tm, host, TCP_HOST_CAP, NULL, &host_err)) {
    *err_out = term_string_from_hstrerr("tcp_connect", "host", TCP_HOST_CAP, host_err);
    return 0;
  }

  if (!tcp_parse_num(port_tm, port) || *port == 0 || *port > 65535) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `port`; expected NUM in [1,65535]");
    return 0;
  }

  Term opts = wnf(opts_tm);
  if (term_tag(opts) != C05 || term_ext(opts) != TCP_NAM_TCP_OPTS) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `opts`; expected #TcpOpts{connect_timeout_ms,read_timeout_ms,write_timeout_ms,nodelay,keepalive}");
    return 0;
  }

  u32  opts_loc = term_val(opts);
  Term connect_tm = heap_read(opts_loc + 0);
  Term read_tm    = heap_read(opts_loc + 1);
  Term write_tm   = heap_read(opts_loc + 2);
  Term nodelay_tm = heap_read(opts_loc + 3);
  Term keep_tm    = heap_read(opts_loc + 4);

  if (!tcp_parse_num(connect_tm, connect_timeout_ms)) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `connect_timeout_ms`; expected NUM");
    return 0;
  }
  if (!tcp_parse_num(read_tm, read_timeout_ms)) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `read_timeout_ms`; expected NUM");
    return 0;
  }
  if (!tcp_parse_num(write_tm, write_timeout_ms)) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `write_timeout_ms`; expected NUM");
    return 0;
  }
  if (!tcp_parse_bool(nodelay_tm, nodelay)) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `nodelay`; expected #T{}|#F{}");
    return 0;
  }
  if (!tcp_parse_bool(keep_tm, keepalive)) {
    *err_out = tcp_new_err("tcp_connect", TCP_ERR_BAD_ARG, "invalid `keepalive`; expected #T{}|#F{}");
    return 0;
  }

  return 1;
}

fn u8 tcp_parse_recv_max(Term max_tm, u32 *max_bytes, Term *err_out) {
  if (!tcp_parse_num(max_tm, max_bytes) || *max_bytes == 0 || *max_bytes > TCP_RECV_CAP) {
    *err_out = tcp_new_err("tcp_recv", TCP_ERR_BAD_ARG, "invalid `max_bytes`; expected NUM in [1,TCP_RECV_CAP]");
    return 0;
  }
  return 1;
}

fn u8 tcp_decode_bytes(
  Term bytes_tm,
  u8 **buf_out,
  u32 *len_out,
  u32 max_len,
  Term *err_out
) {
  Term cur = wnf(bytes_tm);

  u8  *buf = NULL;
  u32  len = 0;
  u32  cap = 0;

  while (term_tag(cur) == C02 && term_ext(cur) == NAM_CON) {
    u32  loc  = term_val(cur);
    Term head = wnf(heap_read(loc + 0));
    Term tail = heap_read(loc + 1);

    if (term_tag(head) != C01 || term_ext(head) != NAM_BYT) {
      free(buf);
      *err_out = tcp_new_err("tcp_send", TCP_ERR_BAD_ARG, "invalid `bytes`; expected List<#BYT{n}>");
      return 0;
    }

    Term num = wnf(heap_read(term_val(head)));
    if (term_tag(num) != NUM) {
      free(buf);
      *err_out = tcp_new_err("tcp_send", TCP_ERR_BAD_ARG, "invalid `bytes`; expected #BYT{NUM}");
      return 0;
    }

    u32 val = term_val(num);
    if (val > 255) {
      free(buf);
      *err_out = tcp_new_err("tcp_send", TCP_ERR_BAD_ARG, "invalid `bytes`; byte must be in [0,255]");
      return 0;
    }

    if (len >= max_len) {
      free(buf);
      *err_out = tcp_new_err("tcp_send", TCP_ERR_BAD_ARG, "invalid `bytes`; payload too large");
      return 0;
    }

    if (len == cap) {
      u32 next_cap = cap == 0 ? 256 : cap * 2;
      if (next_cap > max_len) {
        next_cap = max_len;
      }
      if (next_cap == cap) {
        free(buf);
        *err_out = tcp_new_err("tcp_send", TCP_ERR_BAD_ARG, "invalid `bytes`; payload too large");
        return 0;
      }

      u8 *next = realloc(buf, next_cap);
      if (!next) {
        free(buf);
        *err_out = tcp_new_err("tcp_send", TCP_ERR_IO, "out of memory while decoding bytes");
        return 0;
      }

      buf = next;
      cap = next_cap;
    }

    buf[len] = (u8)val;
    len      = len + 1;
    cur      = wnf(tail);
  }

  if (term_tag(cur) != C00 || term_ext(cur) != NAM_NIL) {
    free(buf);
    *err_out = tcp_new_err("tcp_send", TCP_ERR_BAD_ARG, "invalid `bytes`; expected List<#BYT{n}>");
    return 0;
  }

  *buf_out = buf;
  *len_out = len;
  return 1;
}

fn Term tcp_bytes_to_list(const u8 *buf, u32 len) {
  Term nil = term_new_ctr(NAM_NIL, 0, NULL);
  if (len == 0) {
    return nil;
  }

  Term byt[1] = {term_new_num(buf[0])};
  Term head_tail[2] = {term_new_ctr(NAM_BYT, 1, byt), nil};
  Term out = term_new_ctr(NAM_CON, 2, head_tail);
  Term cur = out;

  for (u32 i = 1; i < len; ++i) {
    byt[0]       = term_new_num(buf[i]);
    head_tail[0] = term_new_ctr(NAM_BYT, 1, byt);
    heap_set(term_val(cur) + 1, term_new_ctr(NAM_CON, 2, head_tail));
    cur = heap_read(term_val(cur) + 1);
  }

  return out;
}

fn int tcp_poll_retry(struct pollfd *pfd, nfds_t nfd, int timeout_ms) {
  while (1) {
    int got = poll(pfd, nfd, timeout_ms);
    if (got >= 0) {
      return got;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }
}

fn Term tcp_connect_check_ready(u32 id, u32 seq, int fd) {
  int       so_err = 0;
  socklen_t len    = sizeof(so_err);

  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) < 0) {
    int err = errno;
    tcp_slot_close_and_set(id, fd, TCP_STATE_FAILED);
    return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_from_errno_evt("tcp_connect", err)));
  }

  if (so_err == 0) {
    tcp_slot_set_fd_state(id, fd, TCP_STATE_OPEN);
    return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_new_conn_evt()));
  }

  tcp_slot_close_and_set(id, fd, TCP_STATE_FAILED);
  return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_from_errno_evt("tcp_connect", so_err)));
}

fn Term tcp_state_not_connected(u32 id, u32 seq) {
  return tcp_new_ok(tcp_new_rdy(id, seq + 1, tcp_fail_not_connected_evt()));
}

#include "connect.c"
#include "connect_poll.c"
#include "connect_wait.c"
#include "recv_poll.c"
#include "recv_wait.c"
#include "send_poll.c"
#include "send_wait.c"
#include "close.c"

fn void prim_tcp_init(void) {
  TCP_NAM_TCP      = nick_from_str("Tcp", 3);
  TCP_NAM_TCP_REQ  = nick_from_str("TcpReq", 6);
  TCP_NAM_TCP_OPTS = nick_from_str("TcpOpts", 7);

  TCP_NAM_PEND   = nick_from_str("Pend", 4);
  TCP_NAM_RDY    = nick_from_str("Rdy", 3);
  TCP_NAM_CONN   = nick_from_str("Conn", 4);
  TCP_NAM_RECV   = nick_from_str("Recv", 4);
  TCP_NAM_SENT   = nick_from_str("Sent", 4);
  TCP_NAM_EOF    = nick_from_str("Eof", 3);
  TCP_NAM_CLOSED = nick_from_str("Closed", 6);
  TCP_NAM_FAIL   = nick_from_str("Fail", 4);

  TCP_NAM_TIMEOUT       = nick_from_str("Timeout", 7);
  TCP_NAM_DNS           = nick_from_str("Dns", 3);
  TCP_NAM_REFUSED       = nick_from_str("Refused", 7);
  TCP_NAM_UNREACHABLE   = nick_from_str("Unreachable", 11);
  TCP_NAM_RESET         = nick_from_str("Reset", 5);
  TCP_NAM_BROKEN_PIPE   = nick_from_str("BrokenPipe", 10);
  TCP_NAM_PROTOCOL      = nick_from_str("Protocol", 8);
  TCP_NAM_NOT_CONNECTED = nick_from_str("NotConnected", 12);
  TCP_NAM_SYS           = nick_from_str("Sys", 3);

  TCP_NAM_T = nick_from_str("T", 1);
  TCP_NAM_F = nick_from_str("F", 1);

  prim_tcp_connect_init();
  prim_tcp_connect_poll_init();
  prim_tcp_connect_wait_init();
  prim_tcp_recv_poll_init();
  prim_tcp_recv_wait_init();
  prim_tcp_send_poll_init();
  prim_tcp_send_wait_init();
  prim_tcp_close_init();
}
