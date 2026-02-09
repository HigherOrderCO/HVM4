#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#define PROC_CAP (1u << 20)

#define PROC_ERR_BAD_ARG    1
#define PROC_ERR_BAD_HANDLE 2
#define PROC_ERR_STALE      3
#define PROC_ERR_FULL       4
#define PROC_ERR_IO         5

typedef struct {
  u32   expected_seq;
  pid_t pid;
  u8    finished;
  u32   exit_code;
} ProcSlot;

static ProcSlot PROC_SLOTS[PROC_CAP];
static u32      PROC_NEXT_ID = 1;
static pthread_mutex_t PROC_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 PROC_PRIM_SPAWN = 0;
static u32 PROC_PRIM_POLL  = 0;
static u32 PROC_PRIM_WAIT  = 0;
static u32 PROC_PRIM_KILL  = 0;

static u32 PROC_NAM_PROC    = 0;
static u32 PROC_NAM_PENDING = 0;
static u32 PROC_NAM_READY   = 0;
static u32 PROC_NAM_ERROR   = 0;
static u32 PROC_NAM_EXIT    = 0;

fn Term wnf(Term term);

fn u32 prim_register(const char *name, u32 len, u32 arity, Term (*fun)(Term *args));
fn u32 nick_from_str(const char *name, u32 len);

fn Term proc_new_error(u32 code) {
  Term arg = term_new_num(code);
  return term_new_ctr(PROC_NAM_ERROR, 1, &arg);
}

fn Term proc_new_handle(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(PROC_NAM_PROC, 2, args);
}

fn Term proc_new_pending(u32 id, u32 seq) {
  Term handle = proc_new_handle(id, seq);
  return term_new_ctr(PROC_NAM_PENDING, 1, &handle);
}

fn Term proc_new_exit(u32 code) {
  Term arg = term_new_num(code);
  return term_new_ctr(PROC_NAM_EXIT, 1, &arg);
}

fn Term proc_new_ready(u32 id, u32 seq, u32 code) {
  Term handle = proc_new_handle(id, seq);
  Term ex     = proc_new_exit(code);
  Term args[2] = {handle, ex};
  return term_new_ctr(PROC_NAM_READY, 2, args);
}

fn u8 proc_parse_num(Term term, u32 *out) {
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

fn u8 proc_parse_handle(Term term, u32 *id, u32 *seq) {
  switch (term_tag(term)) {
    case C02: {
      if (term_ext(term) != PROC_NAM_PROC) {
        return 0;
      }
      u32  loc    = term_val(term);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);
      if (!proc_parse_num(id_tm, id)) {
        return 0;
      }
      if (!proc_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u32 proc_status_code(int status) {
  if (WIFEXITED(status)) {
    return (u32)WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128u + (u32)WTERMSIG(status);
  }
  return 255;
}

fn u8 proc_poll_status_locked(ProcSlot *slot) {
  if (slot->finished) {
    return 1;
  }

  int status = 0;
  pid_t got = waitpid(slot->pid, &status, WNOHANG);
  if (got == 0) {
    return 0;
  }
  if (got < 0) {
    if (errno == EINTR) {
      return 0;
    }
    if (errno == ECHILD) {
      slot->finished  = 1;
      slot->exit_code = 255;
      return 1;
    }
    slot->finished  = 1;
    slot->exit_code = 255;
    return 1;
  }

  slot->finished  = 1;
  slot->exit_code = proc_status_code(status);
  return 1;
}

fn u8 proc_is_valid_id(u32 id) {
  pthread_mutex_lock(&PROC_LOCK);
  if (id == 0 || id >= PROC_NEXT_ID || id >= PROC_CAP) {
    pthread_mutex_unlock(&PROC_LOCK);
    return 0;
  }
  pthread_mutex_unlock(&PROC_LOCK);
  return 1;
}

fn u8 proc_claim(u32 id, u32 seq, u8 check_finished, u8 *finished, u32 *exit_code, pid_t *pid) {
  pthread_mutex_lock(&PROC_LOCK);

  if (id == 0 || id >= PROC_NEXT_ID || id >= PROC_CAP) {
    pthread_mutex_unlock(&PROC_LOCK);
    return 0;
  }

  ProcSlot *slot = &PROC_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&PROC_LOCK);
    return 0;
  }

  slot->expected_seq = seq + 1;
  if (check_finished) {
    proc_poll_status_locked(slot);
  }
  *finished  = slot->finished;
  *exit_code = slot->exit_code;
  *pid       = slot->pid;

  pthread_mutex_unlock(&PROC_LOCK);
  return 1;
}

fn u8 proc_decode_cmd(Term list_wnf, char **out) {
  u32 cap = 64;
  u32 len = 0;
  char *buf = malloc(cap);
  if (buf == NULL) {
    return 0;
  }

  Term cur = list_wnf;
  while (1) {
    cur = wnf(cur);

    switch (term_tag(cur)) {
      case C00 ... C16: {
        if (term_tag(cur) == C00 && term_ext(cur) == NAM_NIL) {
          buf[len] = '\0';
          *out = buf;
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

        if (len + 1 >= cap) {
          u32 new_cap = cap * 2;
          char *new_buf = realloc(buf, new_cap);
          if (new_buf == NULL) {
            free(buf);
            return 0;
          }
          buf = new_buf;
          cap = new_cap;
        }

        buf[len++] = (char)code;
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

fn Term proc_core_spawn(Term arg_wnf) {
  char *cmd = NULL;
  if (!proc_decode_cmd(arg_wnf, &cmd)) {
    return proc_new_error(PROC_ERR_BAD_ARG);
  }

  pid_t pid = fork();
  if (pid < 0) {
    free(cmd);
    return proc_new_error(PROC_ERR_IO);
  }

  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }

  free(cmd);

  pthread_mutex_lock(&PROC_LOCK);
  u32 id = PROC_NEXT_ID;
  if (id >= PROC_CAP) {
    pthread_mutex_unlock(&PROC_LOCK);
    kill(pid, SIGKILL);
    return proc_new_error(PROC_ERR_FULL);
  }

  PROC_NEXT_ID = id + 1;
  PROC_SLOTS[id].expected_seq = 0;
  PROC_SLOTS[id].pid          = pid;
  PROC_SLOTS[id].finished     = 0;
  PROC_SLOTS[id].exit_code    = 0;
  pthread_mutex_unlock(&PROC_LOCK);

  return proc_new_handle(id, 0);
}

fn Term proc_core_poll(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!proc_parse_handle(arg_wnf, &id, &seq)) {
    return proc_new_error(PROC_ERR_BAD_HANDLE);
  }

  if (!proc_is_valid_id(id)) {
    return proc_new_error(PROC_ERR_BAD_HANDLE);
  }

  u8    finished  = 0;
  u32   exit_code = 0;
  pid_t pid       = 0;
  if (!proc_claim(id, seq, 1, &finished, &exit_code, &pid)) {
    return proc_new_error(PROC_ERR_STALE);
  }

  if (finished) {
    return proc_new_ready(id, seq + 1, exit_code);
  }

  return proc_new_pending(id, seq + 1);
}

fn Term proc_core_wait(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!proc_parse_handle(arg_wnf, &id, &seq)) {
    return proc_new_error(PROC_ERR_BAD_HANDLE);
  }

  if (!proc_is_valid_id(id)) {
    return proc_new_error(PROC_ERR_BAD_HANDLE);
  }

  u8    finished  = 0;
  u32   exit_code = 0;
  pid_t pid       = 0;
  if (!proc_claim(id, seq, 1, &finished, &exit_code, &pid)) {
    return proc_new_error(PROC_ERR_STALE);
  }

  if (!finished) {
    int status = 0;
    pid_t got  = 0;
    while (1) {
      got = waitpid(pid, &status, 0);
      if (got >= 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      return proc_new_error(PROC_ERR_IO);
    }

    if (got > 0) {
      exit_code = proc_status_code(status);
    } else {
      exit_code = 255;
    }

    pthread_mutex_lock(&PROC_LOCK);
    if (id < PROC_CAP) {
      PROC_SLOTS[id].finished  = 1;
      PROC_SLOTS[id].exit_code = exit_code;
    }
    pthread_mutex_unlock(&PROC_LOCK);
  }

  return proc_new_ready(id, seq + 1, exit_code);
}

fn Term proc_core_kill(Term arg_wnf) {
  u32 id  = 0;
  u32 seq = 0;
  if (!proc_parse_handle(arg_wnf, &id, &seq)) {
    return proc_new_error(PROC_ERR_BAD_HANDLE);
  }

  if (!proc_is_valid_id(id)) {
    return proc_new_error(PROC_ERR_BAD_HANDLE);
  }

  u8    finished  = 0;
  u32   exit_code = 0;
  pid_t pid       = 0;
  if (!proc_claim(id, seq, 1, &finished, &exit_code, &pid)) {
    return proc_new_error(PROC_ERR_STALE);
  }

  if (finished) {
    return proc_new_ready(id, seq + 1, exit_code);
  }

  if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
    return proc_new_error(PROC_ERR_IO);
  }

  return proc_new_pending(id, seq + 1);
}

typedef Term (*ProcUnaryCoreFn)(Term arg_wnf);

fn Term proc_dispatch_unary(Term arg, u32 prim_id, ProcUnaryCoreFn core) {
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

fn Term prim_fn_proc_spawn(Term *args) {
  return proc_dispatch_unary(args[0], PROC_PRIM_SPAWN, proc_core_spawn);
}

fn Term prim_fn_proc_poll(Term *args) {
  return proc_dispatch_unary(args[0], PROC_PRIM_POLL, proc_core_poll);
}

fn Term prim_fn_proc_wait(Term *args) {
  return proc_dispatch_unary(args[0], PROC_PRIM_WAIT, proc_core_wait);
}

fn Term prim_fn_proc_kill(Term *args) {
  return proc_dispatch_unary(args[0], PROC_PRIM_KILL, proc_core_kill);
}

fn void prim_proc_init(void) {
  PROC_PRIM_SPAWN = prim_register("proc_spawn", 10, 1, prim_fn_proc_spawn);
  PROC_PRIM_POLL  = prim_register("proc_poll", 9, 1, prim_fn_proc_poll);
  PROC_PRIM_WAIT  = prim_register("proc_wait", 9, 1, prim_fn_proc_wait);
  PROC_PRIM_KILL  = prim_register("proc_kill", 9, 1, prim_fn_proc_kill);

  PROC_NAM_PROC    = nick_from_str("Proc", 4);
  PROC_NAM_PENDING = nick_from_str("Pend", 4);
  PROC_NAM_READY   = nick_from_str("Rdy", 3);
  PROC_NAM_ERROR   = nick_from_str("Err", 3);
  PROC_NAM_EXIT    = nick_from_str("Exit", 4);
}
