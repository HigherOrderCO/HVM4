#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#define PROCESS_CAP (1u << 20)

#define PROCESS_ERR_BAD_ARG    1
#define PROCESS_ERR_BAD_HANDLE 2
#define PROCESS_ERR_STALE      3
#define PROCESS_ERR_FULL       4
#define PROCESS_ERR_IO         5

typedef struct {
  u32   expected_seq;
  pid_t pid;
  u8    finished;
  u8    signaled;
  u32   code;
} ProcessSlot;

static ProcessSlot PROCESS_SLOTS[PROCESS_CAP];
static u32         PROCESS_NEXT_ID = 1;
static pthread_mutex_t PROCESS_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 PROCESS_NAM_PROC = 0;
static u32 PROCESS_NAM_PEND = 0;
static u32 PROCESS_NAM_RDY  = 0;
static u32 PROCESS_NAM_EXIT = 0;
static u32 PROCESS_NAM_SIG  = 0;

fn Term wnf(Term term);

fn Term process_new_err(const char *prim, u32 code, const char *msg) {
  Term txt = term_string_printf("ERROR(%s): E%u %s", prim, code, msg);
  return term_new_ctr(SYM_ERR, 1, &txt);
}

fn Term process_new_ok(Term val) {
  return term_new_ctr(SYM_OK, 1, &val);
}

fn Term process_new_proc(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(PROCESS_NAM_PROC, 2, args);
}

fn Term process_new_pend(u32 id, u32 seq) {
  Term proc = process_new_proc(id, seq);
  return term_new_ctr(PROCESS_NAM_PEND, 1, &proc);
}

fn Term process_new_exit(u32 code) {
  Term arg = term_new_num(code);
  return term_new_ctr(PROCESS_NAM_EXIT, 1, &arg);
}

fn Term process_new_sig(u32 sig) {
  Term arg = term_new_num(sig);
  return term_new_ctr(PROCESS_NAM_SIG, 1, &arg);
}

fn Term process_new_rdy(u32 id, u32 seq, u8 signaled, u32 code) {
  Term proc = process_new_proc(id, seq);
  Term st   = signaled ? process_new_sig(code) : process_new_exit(code);
  Term args[2] = {proc, st};
  return term_new_ctr(PROCESS_NAM_RDY, 2, args);
}

fn u8 process_parse_num(Term term, u32 *out) {
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

fn u8 process_parse_handle(Term term, u32 *id, u32 *seq) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C02: {
      if (term_ext(val) != PROCESS_NAM_PROC) {
        return 0;
      }

      u32  loc    = term_val(val);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);

      if (!process_parse_num(id_tm, id)) {
        return 0;
      }
      if (!process_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 process_is_valid_id(u32 id) {
  pthread_mutex_lock(&PROCESS_LOCK);

  if (id == 0 || id >= PROCESS_NEXT_ID || id >= PROCESS_CAP) {
    pthread_mutex_unlock(&PROCESS_LOCK);
    return 0;
  }

  pthread_mutex_unlock(&PROCESS_LOCK);
  return 1;
}

fn void process_set_finished(u32 id, u8 signaled, u32 code) {
  pthread_mutex_lock(&PROCESS_LOCK);

  if (id != 0 && id < PROCESS_NEXT_ID && id < PROCESS_CAP) {
    ProcessSlot *slot = &PROCESS_SLOTS[id];
    slot->finished    = 1;
    slot->signaled    = signaled;
    slot->code        = code;
  }

  pthread_mutex_unlock(&PROCESS_LOCK);
}

fn u8 process_claim(
  u32 id,
  u32 seq,
  pid_t *pid,
  u8 *finished,
  u8 *signaled,
  u32 *code
) {
  pthread_mutex_lock(&PROCESS_LOCK);

  if (id == 0 || id >= PROCESS_NEXT_ID || id >= PROCESS_CAP) {
    pthread_mutex_unlock(&PROCESS_LOCK);
    return 0;
  }

  ProcessSlot *slot = &PROCESS_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&PROCESS_LOCK);
    return 0;
  }

  slot->expected_seq = seq + 1;
  *pid               = slot->pid;
  *finished          = slot->finished;
  *signaled          = slot->signaled;
  *code              = slot->code;

  pthread_mutex_unlock(&PROCESS_LOCK);
  return 1;
}

fn void process_status_from_wait(int status, u8 *signaled, u32 *code) {
  if (WIFEXITED(status)) {
    *signaled = 0;
    *code     = (u32)WEXITSTATUS(status);
    return;
  }

  if (WIFSIGNALED(status)) {
    *signaled = 1;
    *code     = (u32)WTERMSIG(status);
    return;
  }

  *signaled = 0;
  *code     = 255;
}

#include "spawn.c"
#include "poll.c"
#include "wait.c"
#include "kill.c"

fn void prim_process_init(void) {
  PROCESS_NAM_PROC = table_find("Proc", 4);
  PROCESS_NAM_PEND = table_find("Pend", 4);
  PROCESS_NAM_RDY  = table_find("Rdy", 3);
  PROCESS_NAM_EXIT = table_find("Exit", 4);
  PROCESS_NAM_SIG  = table_find("Sig", 3);

  prim_process_spawn_init();
  prim_process_poll_init();
  prim_process_wait_init();
  prim_process_kill_init();
}
