#define TIMER_CAP (1u << 20)

#define TIMER_ERR_BAD_ARG    1
#define TIMER_ERR_BAD_HANDLE 2
#define TIMER_ERR_STALE      3
#define TIMER_ERR_FULL       4

typedef struct {
  u32 expected_seq;
  u64 due_ns;
} TimerSlot;

static TimerSlot TIMER_SLOTS[TIMER_CAP];
static u32       TIMER_NEXT_ID = 1;
static pthread_mutex_t TIMER_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 TIMER_PRIM_START = 0;
static u32 TIMER_PRIM_POLL  = 0;
static u32 TIMER_PRIM_WAIT  = 0;

static u32 TIMER_NAM_TIMER   = 0;
static u32 TIMER_NAM_PENDING = 0;
static u32 TIMER_NAM_READY   = 0;
static u32 TIMER_NAM_ERROR   = 0;

fn Term wnf(Term term);

fn u32  prim_register(const char *name, u32 len, u32 arity, Term (*fun)(Term *args));
fn u32  nick_from_str(const char *name, u32 len);

fn u64 timer_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

fn void timer_sleep_ns(u64 ns) {
  struct timespec req = {
    .tv_sec  = (time_t)(ns / 1000000000ull),
    .tv_nsec = (long)(ns % 1000000000ull),
  };

  while (req.tv_sec != 0 || req.tv_nsec != 0) {
    if (nanosleep(&req, &req) == 0) {
      return;
    }
  }
}

fn Term timer_new_error(u32 code) {
  Term arg = term_new_num(code);
  return term_new_ctr(TIMER_NAM_ERROR, 1, &arg);
}

fn Term timer_new_handle(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(TIMER_NAM_TIMER, 2, args);
}

fn Term timer_new_pending(u32 id, u32 seq) {
  Term handle = timer_new_handle(id, seq);
  return term_new_ctr(TIMER_NAM_PENDING, 1, &handle);
}

fn Term timer_new_ready(u32 id, u32 seq) {
  Term handle = timer_new_handle(id, seq);
  return term_new_ctr(TIMER_NAM_READY, 1, &handle);
}

fn u8 timer_parse_num(Term term, u32 *out) {
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

fn u8 timer_parse_handle(Term term, u32 *id, u32 *seq) {
  switch (term_tag(term)) {
    case C02: {
      if (term_ext(term) != TIMER_NAM_TIMER) {
        return 0;
      }
      u32  loc    = term_val(term);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);
      if (!timer_parse_num(id_tm, id)) {
        return 0;
      }
      if (!timer_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 timer_claim(u32 id, u32 seq, u64 *due_ns) {
  pthread_mutex_lock(&TIMER_LOCK);

  if (id == 0 || id >= TIMER_NEXT_ID || id >= TIMER_CAP) {
    pthread_mutex_unlock(&TIMER_LOCK);
    return 0;
  }

  TimerSlot *slot = &TIMER_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&TIMER_LOCK);
    return 0;
  }

  slot->expected_seq = seq + 1;
  *due_ns = slot->due_ns;
  pthread_mutex_unlock(&TIMER_LOCK);
  return 1;
}

fn u8 timer_is_valid_id(u32 id) {
  pthread_mutex_lock(&TIMER_LOCK);
  if (id == 0 || id >= TIMER_NEXT_ID || id >= TIMER_CAP) {
    pthread_mutex_unlock(&TIMER_LOCK);
    return 0;
  }
  pthread_mutex_unlock(&TIMER_LOCK);
  return 1;
}

fn Term timer_core_start(Term arg_wnf) {
  u32 ms = 0;
  if (!timer_parse_num(arg_wnf, &ms)) {
    return timer_new_error(TIMER_ERR_BAD_ARG);
  }

  u64 now = timer_now_ns();
  u64 due = now + (u64)ms * 1000000ull;
  if (due < now) {
    due = UINT64_MAX;
  }

  pthread_mutex_lock(&TIMER_LOCK);
  u32 id = TIMER_NEXT_ID;
  if (id >= TIMER_CAP) {
    pthread_mutex_unlock(&TIMER_LOCK);
    return timer_new_error(TIMER_ERR_FULL);
  }
  TIMER_NEXT_ID = id + 1;
  TIMER_SLOTS[id].expected_seq = 0;
  TIMER_SLOTS[id].due_ns       = due;
  pthread_mutex_unlock(&TIMER_LOCK);

  return timer_new_handle(id, 0);
}

fn Term timer_core_poll(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!timer_parse_handle(arg_wnf, &id, &seq)) {
    return timer_new_error(TIMER_ERR_BAD_HANDLE);
  }

  if (!timer_is_valid_id(id)) {
    return timer_new_error(TIMER_ERR_BAD_HANDLE);
  }

  u64 due_ns = 0;
  if (!timer_claim(id, seq, &due_ns)) {
    return timer_new_error(TIMER_ERR_STALE);
  }

  u64 now = timer_now_ns();
  if (now >= due_ns) {
    return timer_new_ready(id, seq + 1);
  }
  return timer_new_pending(id, seq + 1);
}

fn Term timer_core_wait(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!timer_parse_handle(arg_wnf, &id, &seq)) {
    return timer_new_error(TIMER_ERR_BAD_HANDLE);
  }

  if (!timer_is_valid_id(id)) {
    return timer_new_error(TIMER_ERR_BAD_HANDLE);
  }

  u64 due_ns = 0;
  if (!timer_claim(id, seq, &due_ns)) {
    return timer_new_error(TIMER_ERR_STALE);
  }

  u64 now = timer_now_ns();
  if (now < due_ns) {
    timer_sleep_ns(due_ns - now);
  }

  return timer_new_ready(id, seq + 1);
}

typedef Term (*TimerUnaryCoreFn)(Term arg_wnf);

fn Term timer_dispatch_unary(Term arg, u32 prim_id, TimerUnaryCoreFn core) {
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

fn Term prim_fn_timer_start(Term *args) {
  return timer_dispatch_unary(args[0], TIMER_PRIM_START, timer_core_start);
}

fn Term prim_fn_timer_poll(Term *args) {
  return timer_dispatch_unary(args[0], TIMER_PRIM_POLL, timer_core_poll);
}

fn Term prim_fn_timer_wait(Term *args) {
  return timer_dispatch_unary(args[0], TIMER_PRIM_WAIT, timer_core_wait);
}

fn void prim_timer_init(void) {
  TIMER_PRIM_START = prim_register("timer_start", 11, 1, prim_fn_timer_start);
  TIMER_PRIM_POLL  = prim_register("timer_poll", 10, 1, prim_fn_timer_poll);
  TIMER_PRIM_WAIT  = prim_register("timer_wait", 10, 1, prim_fn_timer_wait);

  TIMER_NAM_TIMER   = nick_from_str("Time", 4);
  TIMER_NAM_PENDING = nick_from_str("Pend", 4);
  TIMER_NAM_READY   = nick_from_str("Rdy", 3);
  TIMER_NAM_ERROR   = nick_from_str("Err", 3);
}
