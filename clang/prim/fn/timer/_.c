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

static u32 TIMER_NAM_TIME = 0;
static u32 TIMER_NAM_PEND = 0;
static u32 TIMER_NAM_RDY  = 0;

fn Term wnf(Term term);

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

fn Term timer_new_err(const char *prim, u32 code, const char *msg) {
  Term txt = term_string_printf("ERROR(%s): E%u %s", prim, code, msg);
  return term_new_ctr(SYM_ERR, 1, &txt);
}

fn Term timer_new_ok(Term val) {
  return term_new_ctr(SYM_OK, 1, &val);
}

fn Term timer_new_time(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(TIMER_NAM_TIME, 2, args);
}

fn Term timer_new_pend(u32 id, u32 seq) {
  Term time = timer_new_time(id, seq);
  return term_new_ctr(TIMER_NAM_PEND, 1, &time);
}

fn Term timer_new_rdy(u32 id, u32 seq) {
  Term time = timer_new_time(id, seq);
  return term_new_ctr(TIMER_NAM_RDY, 1, &time);
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
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C02: {
      if (term_ext(val) != TIMER_NAM_TIME) {
        return 0;
      }

      u32  loc    = term_val(val);
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

fn u8 timer_is_valid_id(u32 id) {
  pthread_mutex_lock(&TIMER_LOCK);

  if (id == 0 || id >= TIMER_NEXT_ID || id >= TIMER_CAP) {
    pthread_mutex_unlock(&TIMER_LOCK);
    return 0;
  }

  pthread_mutex_unlock(&TIMER_LOCK);
  return 1;
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
  *due_ns            = slot->due_ns;
  pthread_mutex_unlock(&TIMER_LOCK);
  return 1;
}

#include "start.c"
#include "poll.c"
#include "wait.c"

fn void prim_timer_init(void) {
  TIMER_NAM_TIME = table_find("Time", 4);
  TIMER_NAM_PEND = table_find("Pend", 4);
  TIMER_NAM_RDY  = table_find("Rdy", 3);

  prim_timer_start_init();
  prim_timer_poll_init();
  prim_timer_wait_init();
}
