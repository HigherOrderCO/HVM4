#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#define STREAM_CAP (1u << 20)

#define STREAM_ERR_BAD_ARG    1
#define STREAM_ERR_BAD_HANDLE 2
#define STREAM_ERR_STALE      3
#define STREAM_ERR_FULL       4
#define STREAM_ERR_IO         5

#define STREAM_KIND_STDIN 1
#define STREAM_KIND_FILE  2

typedef struct {
  u32 expected_seq;
  u8  kind;
  u8  closed;
  int fd;
} StreamSlot;

static StreamSlot STREAM_SLOTS[STREAM_CAP];
static u32        STREAM_NEXT_ID = 1;
static pthread_mutex_t STREAM_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 STREAM_NAM_STRM = 0;
static u32 STREAM_NAM_PEND = 0;
static u32 STREAM_NAM_RDY  = 0;
static u32 STREAM_NAM_BYT  = 0;
static u32 STREAM_NAM_EOF  = 0;

fn Term wnf(Term term);

fn Term stream_new_err(const char *prim, u32 code, const char *msg) {
  Term txt = term_string_printf("ERROR(%s): E%u %s", prim, code, msg);
  return term_new_ctr(NAM_ERR, 1, &txt);
}

fn Term stream_new_ok(Term val) {
  return term_new_ctr(NAM_OK, 1, &val);
}

fn Term stream_new_handle(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(STREAM_NAM_STRM, 2, args);
}

fn Term stream_new_pend(u32 id, u32 seq) {
  Term strm = stream_new_handle(id, seq);
  return term_new_ctr(STREAM_NAM_PEND, 1, &strm);
}

fn Term stream_new_byt(u32 byt) {
  Term arg = term_new_num(byt);
  return term_new_ctr(STREAM_NAM_BYT, 1, &arg);
}

fn Term stream_new_eof(void) {
  return term_new_ctr(STREAM_NAM_EOF, 0, NULL);
}

fn Term stream_new_rdy_payload(u32 id, u32 seq, Term payload) {
  Term strm    = stream_new_handle(id, seq);
  Term args[2] = {strm, payload};
  return term_new_ctr(STREAM_NAM_RDY, 2, args);
}

fn Term stream_new_rdy_byt(u32 id, u32 seq, u32 byt) {
  return stream_new_rdy_payload(id, seq, stream_new_byt(byt));
}

fn Term stream_new_rdy_eof(u32 id, u32 seq) {
  return stream_new_rdy_payload(id, seq, stream_new_eof());
}

fn u8 stream_parse_num(Term term, u32 *out) {
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

fn u8 stream_parse_handle(Term term, u32 *id, u32 *seq) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C02: {
      if (term_ext(val) != STREAM_NAM_STRM) {
        return 0;
      }

      u32  loc    = term_val(val);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);

      if (!stream_parse_num(id_tm, id)) {
        return 0;
      }
      if (!stream_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 stream_is_valid_id(u32 id) {
  pthread_mutex_lock(&STREAM_LOCK);

  if (id == 0 || id >= STREAM_NEXT_ID || id >= STREAM_CAP) {
    pthread_mutex_unlock(&STREAM_LOCK);
    return 0;
  }

  pthread_mutex_unlock(&STREAM_LOCK);
  return 1;
}

fn u8 stream_claim(u32 id, u32 seq, u8 *kind, u8 *closed, int *fd) {
  pthread_mutex_lock(&STREAM_LOCK);

  if (id == 0 || id >= STREAM_NEXT_ID || id >= STREAM_CAP) {
    pthread_mutex_unlock(&STREAM_LOCK);
    return 0;
  }

  StreamSlot *slot = &STREAM_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&STREAM_LOCK);
    return 0;
  }

  slot->expected_seq = seq + 1;
  *kind              = slot->kind;
  *closed            = slot->closed;
  *fd                = slot->fd;

  pthread_mutex_unlock(&STREAM_LOCK);
  return 1;
}

fn void stream_set_closed(u32 id) {
  pthread_mutex_lock(&STREAM_LOCK);

  if (id != 0 && id < STREAM_NEXT_ID && id < STREAM_CAP) {
    STREAM_SLOTS[id].closed = 1;
  }

  pthread_mutex_unlock(&STREAM_LOCK);
}

// Returns:
//  1 : ready (byte or eof)
//  0 : pending
// -1 : io error
fn int stream_stdin_read(int fd, int timeout_ms, u8 *out_byt, u8 *out_eof) {
  *out_eof = 0;

  struct pollfd pfd = {
    .fd      = fd,
    .events  = POLLIN,
    .revents = 0,
  };

  int poll_ret = 0;
  while (1) {
    poll_ret = poll(&pfd, 1, timeout_ms);
    if (poll_ret >= 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }

  if (poll_ret == 0) {
    return 0;
  }

  if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    return 0;
  }

  while (1) {
    u8      byt = 0;
    ssize_t rd  = read(fd, &byt, 1);

    if (rd == 1) {
      *out_byt = byt;
      return 1;
    }
    if (rd == 0) {
      *out_eof = 1;
      return 1;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return -1;
  }
}

#include "stdin_open.c"
#include "file_open.c"
#include "poll.c"
#include "wait.c"
#include "close.c"

fn void prim_stream_init(void) {
  STREAM_NAM_STRM = nick_from_str("Strm", 4);
  STREAM_NAM_PEND = nick_from_str("Pend", 4);
  STREAM_NAM_RDY  = nick_from_str("Rdy", 3);
  STREAM_NAM_BYT  = nick_from_str("Byt", 3);
  STREAM_NAM_EOF  = nick_from_str("Eof", 3);

  prim_stream_stdin_open_init();
  prim_stream_file_open_init();
  prim_stream_poll_init();
  prim_stream_wait_init();
  prim_stream_close_init();
}
