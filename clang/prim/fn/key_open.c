#include <poll.h>
#include <unistd.h>
#include <errno.h>

#define KEY_CAP (1u << 20)

#define KEY_ERR_BAD_HANDLE 2
#define KEY_ERR_STALE      3
#define KEY_ERR_FULL       4
#define KEY_ERR_IO         5

typedef struct {
  u32 expected_seq;
} KeySlot;

static KeySlot KEY_SLOTS[KEY_CAP];
static u32     KEY_NEXT_ID = 1;
static pthread_mutex_t KEY_LOCK    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t KEY_IO_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 KEY_PRIM_OPEN = 0;
static u32 KEY_PRIM_POLL = 0;
static u32 KEY_PRIM_WAIT = 0;

static u32 KEY_NAM_KEY     = 0;
static u32 KEY_NAM_PENDING = 0;
static u32 KEY_NAM_READY   = 0;
static u32 KEY_NAM_ERROR   = 0;

fn Term wnf(Term term);

fn u32 prim_register(const char *name, u32 len, u32 arity, Term (*fun)(Term *args));
fn u32 nick_from_str(const char *name, u32 len);

fn Term key_new_error(u32 code) {
  Term arg = term_new_num(code);
  return term_new_ctr(KEY_NAM_ERROR, 1, &arg);
}

fn Term key_new_handle(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(KEY_NAM_KEY, 2, args);
}

fn Term key_new_pending(u32 id, u32 seq) {
  Term handle = key_new_handle(id, seq);
  return term_new_ctr(KEY_NAM_PENDING, 1, &handle);
}

fn Term key_new_ready(u32 id, u32 seq, u32 code) {
  Term handle = key_new_handle(id, seq);
  Term num    = term_new_num(code);
  Term chr    = term_new_ctr(NAM_CHR, 1, &num);
  Term args[2] = {handle, chr};
  return term_new_ctr(KEY_NAM_READY, 2, args);
}

fn u8 key_parse_num(Term term, u32 *out) {
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

fn u8 key_parse_handle(Term term, u32 *id, u32 *seq) {
  switch (term_tag(term)) {
    case C02: {
      if (term_ext(term) != KEY_NAM_KEY) {
        return 0;
      }
      u32  loc    = term_val(term);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);
      if (!key_parse_num(id_tm, id)) {
        return 0;
      }
      if (!key_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 key_claim(u32 id, u32 seq) {
  pthread_mutex_lock(&KEY_LOCK);

  if (id == 0 || id >= KEY_NEXT_ID || id >= KEY_CAP) {
    pthread_mutex_unlock(&KEY_LOCK);
    return 0;
  }

  KeySlot *slot = &KEY_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&KEY_LOCK);
    return 0;
  }

  slot->expected_seq = seq + 1;
  pthread_mutex_unlock(&KEY_LOCK);
  return 1;
}

fn u8 key_is_valid_id(u32 id) {
  pthread_mutex_lock(&KEY_LOCK);
  if (id == 0 || id >= KEY_NEXT_ID || id >= KEY_CAP) {
    pthread_mutex_unlock(&KEY_LOCK);
    return 0;
  }
  pthread_mutex_unlock(&KEY_LOCK);
  return 1;
}

fn int key_read_byte(int timeout_ms, u32 *out_code) {
  struct pollfd pfd = {
    .fd      = 0,
    .events  = POLLIN,
    .revents = 0,
  };

  int poll_ret = 0;
  for (;;) {
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

  u8 byte = 0;
  ssize_t rd = 0;
  for (;;) {
    rd = read(0, &byte, 1);
    if (rd >= 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }

  if (rd == 1) {
    *out_code = (u32)byte;
    return 1;
  }

  if (rd == 0) {
    return -2;
  }

  return -1;
}

fn Term key_core_open(Term arg_wnf) {
  (void)arg_wnf;

  pthread_mutex_lock(&KEY_LOCK);
  u32 id = KEY_NEXT_ID;
  if (id >= KEY_CAP) {
    pthread_mutex_unlock(&KEY_LOCK);
    return key_new_error(KEY_ERR_FULL);
  }

  KEY_NEXT_ID = id + 1;
  KEY_SLOTS[id].expected_seq = 0;
  pthread_mutex_unlock(&KEY_LOCK);

  return key_new_handle(id, 0);
}

fn Term key_core_poll(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!key_parse_handle(arg_wnf, &id, &seq)) {
    return key_new_error(KEY_ERR_BAD_HANDLE);
  }

  if (!key_is_valid_id(id)) {
    return key_new_error(KEY_ERR_BAD_HANDLE);
  }

  if (!key_claim(id, seq)) {
    return key_new_error(KEY_ERR_STALE);
  }

  u32 code = 0;
  pthread_mutex_lock(&KEY_IO_LOCK);
  int read_ret = key_read_byte(0, &code);
  pthread_mutex_unlock(&KEY_IO_LOCK);

  switch (read_ret) {
    case 1: {
      return key_new_ready(id, seq + 1, code);
    }
    case 0:
    case -2: {
      return key_new_pending(id, seq + 1);
    }
    default: {
      return key_new_error(KEY_ERR_IO);
    }
  }
}

fn Term key_core_wait(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!key_parse_handle(arg_wnf, &id, &seq)) {
    return key_new_error(KEY_ERR_BAD_HANDLE);
  }

  if (!key_is_valid_id(id)) {
    return key_new_error(KEY_ERR_BAD_HANDLE);
  }

  if (!key_claim(id, seq)) {
    return key_new_error(KEY_ERR_STALE);
  }

  u32 code = 0;
  pthread_mutex_lock(&KEY_IO_LOCK);
  int read_ret = key_read_byte(-1, &code);
  pthread_mutex_unlock(&KEY_IO_LOCK);

  switch (read_ret) {
    case 1: {
      return key_new_ready(id, seq + 1, code);
    }
    default: {
      return key_new_error(KEY_ERR_IO);
    }
  }
}

typedef Term (*KeyUnaryCoreFn)(Term arg_wnf);

fn Term key_dispatch_unary(Term arg, u32 prim_id, KeyUnaryCoreFn core) {
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

fn Term prim_fn_key_open(Term *args) {
  return key_dispatch_unary(args[0], KEY_PRIM_OPEN, key_core_open);
}

fn Term prim_fn_key_poll(Term *args) {
  return key_dispatch_unary(args[0], KEY_PRIM_POLL, key_core_poll);
}

fn Term prim_fn_key_wait(Term *args) {
  return key_dispatch_unary(args[0], KEY_PRIM_WAIT, key_core_wait);
}

fn void prim_key_init(void) {
  KEY_PRIM_OPEN = prim_register("key_open", 8, 1, prim_fn_key_open);
  KEY_PRIM_POLL = prim_register("key_poll", 8, 1, prim_fn_key_poll);
  KEY_PRIM_WAIT = prim_register("key_wait", 8, 1, prim_fn_key_wait);

  KEY_NAM_KEY     = nick_from_str("Key", 3);
  KEY_NAM_PENDING = nick_from_str("Pend", 4);
  KEY_NAM_READY   = nick_from_str("Rdy", 3);
  KEY_NAM_ERROR   = nick_from_str("Err", 3);
}
