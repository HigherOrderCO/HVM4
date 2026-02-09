#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define TCP_CAP (1u << 20)

#define TCP_ERR_BAD_ARG    1
#define TCP_ERR_BAD_HANDLE 2
#define TCP_ERR_STALE      3
#define TCP_ERR_FULL       4
#define TCP_ERR_IO         5
#define TCP_ERR_CLOSED     6

#define TCP_STATE_CONNECTING 0
#define TCP_STATE_READY      1
#define TCP_STATE_CLOSED     2
#define TCP_STATE_FAILED     3

typedef struct {
  u32 expected_seq;
  int fd;
  u8  state;
  u32 io_error;
} TcpSlot;

static TcpSlot TCP_SLOTS[TCP_CAP];
static u32     TCP_NEXT_ID = 1;
static pthread_mutex_t TCP_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 TCP_PRIM_CONNECT   = 0;
static u32 TCP_PRIM_POLL      = 0;
static u32 TCP_PRIM_WAIT      = 0;
static u32 TCP_PRIM_SEND      = 0;
static u32 TCP_PRIM_RECV_POLL = 0;
static u32 TCP_PRIM_RECV_WAIT = 0;
static u32 TCP_PRIM_CLOSE     = 0;

static u32 TCP_NAM_TCP     = 0;
static u32 TCP_NAM_PENDING = 0;
static u32 TCP_NAM_READY   = 0;
static u32 TCP_NAM_ERROR   = 0;
static u32 TCP_NAM_SENT    = 0;

fn Term wnf(Term term);

fn u32 prim_register(const char *name, u32 len, u32 arity, Term (*fun)(Term *args));
fn u32 nick_from_str(const char *name, u32 len);

fn Term tcp_new_error(u32 code) {
  Term arg = term_new_num(code);
  return term_new_ctr(TCP_NAM_ERROR, 1, &arg);
}

fn Term tcp_new_handle(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(TCP_NAM_TCP, 2, args);
}

fn Term tcp_new_pending(u32 id, u32 seq) {
  Term handle = tcp_new_handle(id, seq);
  return term_new_ctr(TCP_NAM_PENDING, 1, &handle);
}

fn Term tcp_new_ready(u32 id, u32 seq) {
  Term handle = tcp_new_handle(id, seq);
  return term_new_ctr(TCP_NAM_READY, 1, &handle);
}

fn Term tcp_new_sent(u32 count) {
  Term arg = term_new_num(count);
  return term_new_ctr(TCP_NAM_SENT, 1, &arg);
}

fn Term tcp_new_nil(void) {
  return term_new_ctr(NAM_NIL, 0, NULL);
}

fn Term tcp_new_chr(u32 code) {
  Term num = term_new_num(code);
  return term_new_ctr(NAM_CHR, 1, &num);
}

fn Term tcp_new_ready_data(u32 id, u32 seq, Term data) {
  Term handle = tcp_new_handle(id, seq);
  Term args[2] = {handle, data};
  return term_new_ctr(TCP_NAM_READY, 2, args);
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

fn u8 tcp_parse_handle(Term term, u32 *id, u32 *seq) {
  switch (term_tag(term)) {
    case C02: {
      if (term_ext(term) != TCP_NAM_TCP) {
        return 0;
      }
      u32  loc    = term_val(term);
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

fn u8 tcp_parse_port(Term term, u16 *out_port) {
  u32 port = 0;
  if (!tcp_parse_num(term, &port)) {
    return 0;
  }
  if (port == 0 || port > 65535) {
    return 0;
  }
  *out_port = (u16)port;
  return 1;
}

fn u8 tcp_decode_bytes(Term list_wnf, u8 **out_buf, u32 *out_len) {
  u32 cap = 64;
  u32 len = 0;
  u8 *buf = malloc(cap);
  if (buf == NULL) {
    return 0;
  }

  Term cur = list_wnf;
  while (1) {
    cur = wnf(cur);

    switch (term_tag(cur)) {
      case C00 ... C16: {
        if (term_tag(cur) == C00 && term_ext(cur) == NAM_NIL) {
          *out_buf = buf;
          *out_len = len;
          return 1;
        }
        if (term_tag(cur) != C02 || term_ext(cur) != NAM_CON) {
          free(buf);
          return 0;
        }

        u32  con_loc = term_val(cur);
        Term head    = heap_read(con_loc + 0);
        Term tail    = heap_read(con_loc + 1);
        Term head_w  = wnf(head);
        if (term_tag(head_w) != C01 || term_ext(head_w) != NAM_CHR) {
          free(buf);
          return 0;
        }

        u32  chr_loc = term_val(head_w);
        Term code_tm = heap_read(chr_loc + 0);
        Term code_w  = wnf(code_tm);
        if (term_tag(code_w) != NUM) {
          free(buf);
          return 0;
        }

        u32 code = term_val(code_w);
        if (code > 255) {
          free(buf);
          return 0;
        }

        if (len + 1 > cap) {
          u32 new_cap = cap * 2;
          char *new_buf = realloc(buf, new_cap);
          if (new_buf == NULL) {
            free(buf);
            return 0;
          }
          buf = (u8 *)new_buf;
          cap = new_cap;
        }

        buf[len++] = (u8)code;
        cur = tail;
        continue;
      }
      default: {
        free(buf);
        return 0;
      }
    }
  }
}

fn u8 tcp_decode_host(Term host_wnf, char **out_host) {
  u8  *bytes = NULL;
  u32  len   = 0;
  if (!tcp_decode_bytes(host_wnf, &bytes, &len)) {
    return 0;
  }

  if (len == 0) {
    free(bytes);
    return 0;
  }

  for (u32 i = 0; i < len; i++) {
    if (bytes[i] == 0) {
      free(bytes);
      return 0;
    }
  }

  char *host = malloc(len + 1);
  if (host == NULL) {
    free(bytes);
    return 0;
  }

  for (u32 i = 0; i < len; i++) {
    host[i] = (char)bytes[i];
  }
  host[len] = '\0';

  free(bytes);
  *out_host = host;
  return 1;
}

fn int tcp_wait_events(int fd, short events, int timeout_ms, short *revents_out) {
  struct pollfd pfd = {
    .fd      = fd,
    .events  = events,
    .revents = 0,
  };

  while (1) {
    int poll_ret = poll(&pfd, 1, timeout_ms);
    if (poll_ret >= 0) {
      if (revents_out != NULL) {
        *revents_out = pfd.revents;
      }
      if (poll_ret == 0) {
        return 0;
      }
      return 1;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }
}

fn u8 tcp_set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return 0;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return 0;
  }
  return 1;
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

fn u8 tcp_claim(u32 id, u32 seq, int *fd, u8 *state, u32 *io_error) {
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

  slot->expected_seq = seq + 1;
  *fd       = slot->fd;
  *state    = slot->state;
  *io_error = slot->io_error;

  pthread_mutex_unlock(&TCP_LOCK);
  return 1;
}

fn u8 tcp_snapshot(u32 id, int *fd, u8 *state, u32 *io_error) {
  pthread_mutex_lock(&TCP_LOCK);

  if (id == 0 || id >= TCP_NEXT_ID || id >= TCP_CAP) {
    pthread_mutex_unlock(&TCP_LOCK);
    return 0;
  }

  TcpSlot *slot = &TCP_SLOTS[id];
  *fd       = slot->fd;
  *state    = slot->state;
  *io_error = slot->io_error;

  pthread_mutex_unlock(&TCP_LOCK);
  return 1;
}

fn void tcp_mark_ready(u32 id) {
  pthread_mutex_lock(&TCP_LOCK);
  if (id != 0 && id < TCP_NEXT_ID && id < TCP_CAP) {
    TcpSlot *slot = &TCP_SLOTS[id];
    if (slot->state == TCP_STATE_CONNECTING) {
      slot->state    = TCP_STATE_READY;
      slot->io_error = 0;
    }
  }
  pthread_mutex_unlock(&TCP_LOCK);
}

fn void tcp_mark_failed(u32 id, u32 io_error) {
  pthread_mutex_lock(&TCP_LOCK);
  if (id != 0 && id < TCP_NEXT_ID && id < TCP_CAP) {
    TcpSlot *slot = &TCP_SLOTS[id];
    if (slot->fd >= 0) {
      close(slot->fd);
      slot->fd = -1;
    }
    slot->state    = TCP_STATE_FAILED;
    slot->io_error = io_error;
  }
  pthread_mutex_unlock(&TCP_LOCK);
}

fn void tcp_mark_closed(u32 id) {
  pthread_mutex_lock(&TCP_LOCK);
  if (id != 0 && id < TCP_NEXT_ID && id < TCP_CAP) {
    TcpSlot *slot = &TCP_SLOTS[id];
    if (slot->fd >= 0) {
      close(slot->fd);
      slot->fd = -1;
    }
    slot->state    = TCP_STATE_CLOSED;
    slot->io_error = 0;
  }
  pthread_mutex_unlock(&TCP_LOCK);
}

// Returns:
//   1  -> ready
//   0  -> pending
//  -1  -> failed/io
//  -2  -> closed
fn int tcp_finish_connect(u32 id, int timeout_ms) {
  int fd = -1;
  u8  state = TCP_STATE_FAILED;
  u32 io_error = 0;
  if (!tcp_snapshot(id, &fd, &state, &io_error)) {
    return -1;
  }

  switch (state) {
    case TCP_STATE_READY: {
      return 1;
    }
    case TCP_STATE_CONNECTING: {
      break;
    }
    case TCP_STATE_CLOSED: {
      return -2;
    }
    case TCP_STATE_FAILED: {
      return -1;
    }
    default: {
      return -1;
    }
  }

  short revents = 0;
  int wait_ret = tcp_wait_events(fd, POLLOUT | POLLERR | POLLHUP, timeout_ms, &revents);
  if (wait_ret < 0) {
    tcp_mark_failed(id, (u32)errno);
    return -1;
  }
  if (wait_ret == 0) {
    return 0;
  }

  int       sock_err = 0;
  socklen_t sock_len = sizeof(sock_err);
  while (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_len) < 0) {
    if (errno == EINTR) {
      continue;
    }
    tcp_mark_failed(id, (u32)errno);
    return -1;
  }

  if (sock_err != 0) {
    tcp_mark_failed(id, (u32)sock_err);
    return -1;
  }

  tcp_mark_ready(id);
  return 1;
}

fn Term tcp_core_connect(Term host_wnf, Term port_arg) {
  char *host = NULL;
  if (!tcp_decode_host(host_wnf, &host)) {
    return tcp_new_error(TCP_ERR_BAD_ARG);
  }

  Term port_wnf = wnf(port_arg);
  u16  port = 0;
  if (!tcp_parse_port(port_wnf, &port)) {
    free(host);
    return tcp_new_error(TCP_ERR_BAD_ARG);
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    free(host);
    return tcp_new_error(TCP_ERR_BAD_ARG);
  }
  free(host);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return tcp_new_error(TCP_ERR_IO);
  }

  if (!tcp_set_nonblock(fd)) {
    close(fd);
    return tcp_new_error(TCP_ERR_IO);
  }

#ifdef SO_NOSIGPIPE
  {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif

  u8  state    = TCP_STATE_CONNECTING;
  u32 io_error = 0;

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
    state = TCP_STATE_READY;
  } else if (errno == EINPROGRESS) {
    state = TCP_STATE_CONNECTING;
  } else {
    state    = TCP_STATE_FAILED;
    io_error = (u32)errno;
    close(fd);
    fd = -1;
  }

  pthread_mutex_lock(&TCP_LOCK);
  u32 id = TCP_NEXT_ID;
  if (id >= TCP_CAP) {
    pthread_mutex_unlock(&TCP_LOCK);
    if (fd >= 0) {
      close(fd);
    }
    return tcp_new_error(TCP_ERR_FULL);
  }

  TCP_NEXT_ID = id + 1;
  TCP_SLOTS[id].expected_seq = 0;
  TCP_SLOTS[id].fd           = fd;
  TCP_SLOTS[id].state        = state;
  TCP_SLOTS[id].io_error     = io_error;
  pthread_mutex_unlock(&TCP_LOCK);

  return tcp_new_handle(id, 0);
}

fn Term tcp_core_poll(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(arg_wnf, &id, &seq)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  int fd = -1;
  u8  state = TCP_STATE_FAILED;
  u32 io_error = 0;
  if (!tcp_claim(id, seq, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_STALE);
  }

  switch (state) {
    case TCP_STATE_READY: {
      return tcp_new_ready(id, seq + 1);
    }
    case TCP_STATE_CONNECTING: {
      int st = tcp_finish_connect(id, 0);
      if (st == 1) {
        return tcp_new_ready(id, seq + 1);
      }
      if (st == 0) {
        return tcp_new_pending(id, seq + 1);
      }
      if (st == -2) {
        return tcp_new_error(TCP_ERR_CLOSED);
      }
      return tcp_new_error(TCP_ERR_IO);
    }
    case TCP_STATE_CLOSED: {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    case TCP_STATE_FAILED: {
      return tcp_new_error(TCP_ERR_IO);
    }
    default: {
      return tcp_new_error(TCP_ERR_IO);
    }
  }
}

fn Term tcp_core_wait(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(arg_wnf, &id, &seq)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  int fd = -1;
  u8  state = TCP_STATE_FAILED;
  u32 io_error = 0;
  if (!tcp_claim(id, seq, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_STALE);
  }

  switch (state) {
    case TCP_STATE_READY: {
      return tcp_new_ready(id, seq + 1);
    }
    case TCP_STATE_CONNECTING: {
      int st = tcp_finish_connect(id, -1);
      if (st == 1) {
        return tcp_new_ready(id, seq + 1);
      }
      if (st == -2) {
        return tcp_new_error(TCP_ERR_CLOSED);
      }
      return tcp_new_error(TCP_ERR_IO);
    }
    case TCP_STATE_CLOSED: {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    case TCP_STATE_FAILED: {
      return tcp_new_error(TCP_ERR_IO);
    }
    default: {
      return tcp_new_error(TCP_ERR_IO);
    }
  }
}

fn Term tcp_core_send(Term handle_wnf, Term data_arg) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(handle_wnf, &id, &seq)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  int fd = -1;
  u8  state = TCP_STATE_FAILED;
  u32 io_error = 0;
  if (!tcp_claim(id, seq, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_STALE);
  }

  switch (state) {
    case TCP_STATE_READY: {
      break;
    }
    case TCP_STATE_CONNECTING: {
      int st = tcp_finish_connect(id, -1);
      if (st == 1) {
        break;
      }
      if (st == -2) {
        return tcp_new_error(TCP_ERR_CLOSED);
      }
      return tcp_new_error(TCP_ERR_IO);
    }
    case TCP_STATE_CLOSED: {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    case TCP_STATE_FAILED: {
      return tcp_new_error(TCP_ERR_IO);
    }
    default: {
      return tcp_new_error(TCP_ERR_IO);
    }
  }

  if (!tcp_snapshot(id, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }
  if (state != TCP_STATE_READY || fd < 0) {
    if (state == TCP_STATE_CLOSED) {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    return tcp_new_error(TCP_ERR_IO);
  }

  Term data_wnf = wnf(data_arg);
  u8  *bytes = NULL;
  u32  len   = 0;
  if (!tcp_decode_bytes(data_wnf, &bytes, &len)) {
    return tcp_new_error(TCP_ERR_BAD_ARG);
  }

  u32 sent = 0;
  while (sent < len) {
    ssize_t wr = 0;
    while (1) {
#ifdef MSG_NOSIGNAL
      wr = send(fd, bytes + sent, len - sent, MSG_NOSIGNAL);
#else
      wr = send(fd, bytes + sent, len - sent, 0);
#endif
      if (wr >= 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        short revents = 0;
        int wait_ret = tcp_wait_events(fd, POLLOUT | POLLERR | POLLHUP, -1, &revents);
        if (wait_ret < 0) {
          u32 err = (u32)errno;
          free(bytes);
          tcp_mark_failed(id, err);
          return tcp_new_error(TCP_ERR_IO);
        }
        continue;
      }
      u32 err = (u32)errno;
      free(bytes);
      tcp_mark_failed(id, err);
      return tcp_new_error(TCP_ERR_IO);
    }

    if (wr == 0) {
      free(bytes);
      tcp_mark_failed(id, 0);
      return tcp_new_error(TCP_ERR_IO);
    }
    sent += (u32)wr;
  }

  free(bytes);
  Term sent_tm = tcp_new_sent(sent);
  return tcp_new_ready_data(id, seq + 1, sent_tm);
}

fn Term tcp_core_recv_poll(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(arg_wnf, &id, &seq)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  int fd = -1;
  u8  state = TCP_STATE_FAILED;
  u32 io_error = 0;
  if (!tcp_claim(id, seq, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_STALE);
  }

  switch (state) {
    case TCP_STATE_READY: {
      break;
    }
    case TCP_STATE_CONNECTING: {
      int st = tcp_finish_connect(id, 0);
      if (st == 1) {
        break;
      }
      if (st == 0) {
        return tcp_new_pending(id, seq + 1);
      }
      if (st == -2) {
        return tcp_new_error(TCP_ERR_CLOSED);
      }
      return tcp_new_error(TCP_ERR_IO);
    }
    case TCP_STATE_CLOSED: {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    case TCP_STATE_FAILED: {
      return tcp_new_error(TCP_ERR_IO);
    }
    default: {
      return tcp_new_error(TCP_ERR_IO);
    }
  }

  if (!tcp_snapshot(id, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }
  if (state != TCP_STATE_READY || fd < 0) {
    if (state == TCP_STATE_CLOSED) {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    return tcp_new_error(TCP_ERR_IO);
  }

  short revents = 0;
  int wait_ret = tcp_wait_events(fd, POLLIN | POLLERR | POLLHUP, 0, &revents);
  if (wait_ret < 0) {
    tcp_mark_failed(id, (u32)errno);
    return tcp_new_error(TCP_ERR_IO);
  }
  if (wait_ret == 0) {
    return tcp_new_pending(id, seq + 1);
  }

  while (1) {
    u8 byte = 0;
    ssize_t rd = recv(fd, &byte, 1, 0);
    if (rd == 1) {
      Term chr = tcp_new_chr((u32)byte);
      return tcp_new_ready_data(id, seq + 1, chr);
    }
    if (rd == 0) {
      tcp_mark_closed(id);
      return tcp_new_ready_data(id, seq + 1, tcp_new_nil());
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return tcp_new_pending(id, seq + 1);
    }
    tcp_mark_failed(id, (u32)errno);
    return tcp_new_error(TCP_ERR_IO);
  }
}

fn Term tcp_core_recv_wait(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(arg_wnf, &id, &seq)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  int fd = -1;
  u8  state = TCP_STATE_FAILED;
  u32 io_error = 0;
  if (!tcp_claim(id, seq, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_STALE);
  }

  switch (state) {
    case TCP_STATE_READY: {
      break;
    }
    case TCP_STATE_CONNECTING: {
      int st = tcp_finish_connect(id, -1);
      if (st == 1) {
        break;
      }
      if (st == -2) {
        return tcp_new_error(TCP_ERR_CLOSED);
      }
      return tcp_new_error(TCP_ERR_IO);
    }
    case TCP_STATE_CLOSED: {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    case TCP_STATE_FAILED: {
      return tcp_new_error(TCP_ERR_IO);
    }
    default: {
      return tcp_new_error(TCP_ERR_IO);
    }
  }

  if (!tcp_snapshot(id, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }
  if (state != TCP_STATE_READY || fd < 0) {
    if (state == TCP_STATE_CLOSED) {
      return tcp_new_error(TCP_ERR_CLOSED);
    }
    return tcp_new_error(TCP_ERR_IO);
  }

  while (1) {
    u8 byte = 0;
    ssize_t rd = recv(fd, &byte, 1, 0);
    if (rd == 1) {
      Term chr = tcp_new_chr((u32)byte);
      return tcp_new_ready_data(id, seq + 1, chr);
    }
    if (rd == 0) {
      tcp_mark_closed(id);
      return tcp_new_ready_data(id, seq + 1, tcp_new_nil());
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      short revents = 0;
      int wait_ret = tcp_wait_events(fd, POLLIN | POLLERR | POLLHUP, -1, &revents);
      if (wait_ret < 0) {
        tcp_mark_failed(id, (u32)errno);
        return tcp_new_error(TCP_ERR_IO);
      }
      continue;
    }
    tcp_mark_failed(id, (u32)errno);
    return tcp_new_error(TCP_ERR_IO);
  }
}

fn Term tcp_core_close(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!tcp_parse_handle(arg_wnf, &id, &seq)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  if (!tcp_is_valid_id(id)) {
    return tcp_new_error(TCP_ERR_BAD_HANDLE);
  }

  int fd = -1;
  u8  state = TCP_STATE_FAILED;
  u32 io_error = 0;
  if (!tcp_claim(id, seq, &fd, &state, &io_error)) {
    return tcp_new_error(TCP_ERR_STALE);
  }

  tcp_mark_closed(id);
  return tcp_new_ready(id, seq + 1);
}

typedef Term (*TcpUnaryCoreFn)(Term arg_wnf);
typedef Term (*TcpBinaryCoreFn)(Term a_wnf, Term b_arg);

fn Term tcp_dispatch_unary(Term arg, u32 prim_id, TcpUnaryCoreFn core) {
  Term arg_wnf = wnf(arg);

  switch (term_tag(arg_wnf)) {
    case ERA: {
      return term_new_era();
    }
    case INC: {
      u32  inc_loc = term_val(arg_wnf);
      Term inner   = heap_read(inc_loc);
      Term args0[1] = {inner};
      Term call    = term_new_pri(prim_id, 1, args0);
      heap_set(inc_loc, call);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      u32  lab     = term_ext(arg_wnf);
      u32  sup_loc = term_val(arg_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term args0[1] = {x};
      Term args1[1] = {y};
      Term t0      = term_new_pri(prim_id, 1, args0);
      Term t1      = term_new_pri(prim_id, 1, args1);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      return core(arg_wnf);
    }
  }
}

fn Term tcp_dispatch_binary(Term a, Term b, u32 prim_id, TcpBinaryCoreFn core) {
  Term a_wnf = wnf(a);

  switch (term_tag(a_wnf)) {
    case ERA: {
      return term_new_era();
    }
    case INC: {
      u32  inc_loc = term_val(a_wnf);
      Term inner   = heap_read(inc_loc);
      Term args0[2] = {inner, b};
      Term call    = term_new_pri(prim_id, 2, args0);
      heap_set(inc_loc, call);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      u32  lab     = term_ext(a_wnf);
      u32  sup_loc = term_val(a_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Copy bcp     = term_clone(lab, b);
      Term args0[2] = {x, bcp.k0};
      Term args1[2] = {y, bcp.k1};
      Term t0      = term_new_pri(prim_id, 2, args0);
      Term t1      = term_new_pri(prim_id, 2, args1);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      return core(a_wnf, b);
    }
  }
}

fn Term prim_fn_tcp_connect(Term *args) {
  return tcp_dispatch_binary(args[0], args[1], TCP_PRIM_CONNECT, tcp_core_connect);
}

fn Term prim_fn_tcp_poll(Term *args) {
  return tcp_dispatch_unary(args[0], TCP_PRIM_POLL, tcp_core_poll);
}

fn Term prim_fn_tcp_wait(Term *args) {
  return tcp_dispatch_unary(args[0], TCP_PRIM_WAIT, tcp_core_wait);
}

fn Term prim_fn_tcp_send(Term *args) {
  return tcp_dispatch_binary(args[0], args[1], TCP_PRIM_SEND, tcp_core_send);
}

fn Term prim_fn_tcp_recv_poll(Term *args) {
  return tcp_dispatch_unary(args[0], TCP_PRIM_RECV_POLL, tcp_core_recv_poll);
}

fn Term prim_fn_tcp_recv_wait(Term *args) {
  return tcp_dispatch_unary(args[0], TCP_PRIM_RECV_WAIT, tcp_core_recv_wait);
}

fn Term prim_fn_tcp_close(Term *args) {
  return tcp_dispatch_unary(args[0], TCP_PRIM_CLOSE, tcp_core_close);
}

fn void prim_tcp_init(void) {
  TCP_PRIM_CONNECT   = prim_register("tcp_connect", 11, 2, prim_fn_tcp_connect);
  TCP_PRIM_POLL      = prim_register("tcp_poll", 8, 1, prim_fn_tcp_poll);
  TCP_PRIM_WAIT      = prim_register("tcp_wait", 8, 1, prim_fn_tcp_wait);
  TCP_PRIM_SEND      = prim_register("tcp_send", 8, 2, prim_fn_tcp_send);
  TCP_PRIM_RECV_POLL = prim_register("tcp_recv_poll", 13, 1, prim_fn_tcp_recv_poll);
  TCP_PRIM_RECV_WAIT = prim_register("tcp_recv_wait", 13, 1, prim_fn_tcp_recv_wait);
  TCP_PRIM_CLOSE     = prim_register("tcp_close", 9, 1, prim_fn_tcp_close);

  TCP_NAM_TCP     = nick_from_str("Tcp", 3);
  TCP_NAM_PENDING = nick_from_str("Pend", 4);
  TCP_NAM_READY   = nick_from_str("Rdy", 3);
  TCP_NAM_ERROR   = nick_from_str("Err", 3);
  TCP_NAM_SENT    = nick_from_str("Sent", 4);
}
