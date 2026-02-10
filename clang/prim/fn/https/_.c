#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#define HTTPS_CAP (1u << 18)
#define HTTPS_BODY_CAP (1u << 20)

#define HTTPS_ERR_BAD_ARG    1
#define HTTPS_ERR_BAD_HANDLE 2
#define HTTPS_ERR_STALE      3
#define HTTPS_ERR_FULL       4
#define HTTPS_ERR_IO         5

#define HTTPS_FAIL_CURL_EXIT   1
#define HTTPS_FAIL_CURL_SIGNAL 2
#define HTTPS_FAIL_PARSE       3
#define HTTPS_FAIL_BODY        4

typedef struct {
  u32   expected_seq;
  pid_t pid;
  u8    finished;
  u8    canceled;
  u8    signaled;
  u8    parsed;
  u32   code;
  Term  outcome;

  char *tmp_dir;
  char *hdr_path;
  char *body_path;
  char *meta_path;
  char *err_path;
} HttpsSlot;

static HttpsSlot HTTPS_SLOTS[HTTPS_CAP];
static u32       HTTPS_NEXT_ID = 1;
static pthread_mutex_t HTTPS_LOCK = PTHREAD_MUTEX_INITIALIZER;

static u32 HTTPS_NAM_HTTP     = 0;
static u32 HTTPS_NAM_PEND     = 0;
static u32 HTTPS_NAM_RDY      = 0;
static u32 HTTPS_NAM_RESP     = 0;
static u32 HTTPS_NAM_HDR      = 0;
static u32 HTTPS_NAM_FAIL     = 0;
static u32 HTTPS_NAM_CANCELED = 0;

fn Term wnf(Term term);

fn Term https_new_err(const char *prim, u32 code, const char *msg) {
  Term txt = term_string_printf("ERROR(%s): E%u %s", prim, code, msg);
  return term_new_ctr(NAM_ERR, 1, &txt);
}

fn Term https_new_ok(Term val) {
  return term_new_ctr(NAM_OK, 1, &val);
}

fn Term https_new_http(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(HTTPS_NAM_HTTP, 2, args);
}

fn Term https_new_pend(u32 id, u32 seq) {
  Term http = https_new_http(id, seq);
  return term_new_ctr(HTTPS_NAM_PEND, 1, &http);
}

fn Term https_new_rdy(u32 id, u32 seq, Term outcome) {
  Term http   = https_new_http(id, seq);
  Term args[2] = {http, outcome};
  return term_new_ctr(HTTPS_NAM_RDY, 2, args);
}

fn Term https_new_fail(u32 code, Term msg) {
  Term args[2] = {term_new_num(code), msg};
  return term_new_ctr(HTTPS_NAM_FAIL, 2, args);
}

fn Term https_new_canceled(void) {
  return term_new_ctr(HTTPS_NAM_CANCELED, 0, NULL);
}

fn Term https_new_resp(u32 status, Term headers, Term body) {
  Term args[3] = {term_new_num(status), headers, body};
  return term_new_ctr(HTTPS_NAM_RESP, 3, args);
}

fn u8 https_parse_num(Term term, u32 *out) {
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

fn u8 https_parse_handle(Term term, u32 *id, u32 *seq) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C02: {
      if (term_ext(val) != HTTPS_NAM_HTTP) {
        return 0;
      }

      u32  loc    = term_val(val);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);

      if (!https_parse_num(id_tm, id)) {
        return 0;
      }
      if (!https_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 https_is_valid_id(u32 id) {
  pthread_mutex_lock(&HTTPS_LOCK);

  if (id == 0 || id >= HTTPS_NEXT_ID || id >= HTTPS_CAP) {
    pthread_mutex_unlock(&HTTPS_LOCK);
    return 0;
  }

  pthread_mutex_unlock(&HTTPS_LOCK);
  return 1;
}

fn void https_set_finished(u32 id, u8 signaled, u32 code) {
  pthread_mutex_lock(&HTTPS_LOCK);

  if (id != 0 && id < HTTPS_NEXT_ID && id < HTTPS_CAP) {
    HttpsSlot *slot = &HTTPS_SLOTS[id];
    slot->finished  = 1;
    slot->signaled  = signaled;
    slot->code      = code;
  }

  pthread_mutex_unlock(&HTTPS_LOCK);
}

fn void https_set_canceled(u32 id) {
  pthread_mutex_lock(&HTTPS_LOCK);

  if (id != 0 && id < HTTPS_NEXT_ID && id < HTTPS_CAP) {
    HTTPS_SLOTS[id].canceled = 1;
  }

  pthread_mutex_unlock(&HTTPS_LOCK);
}

fn void https_status_from_wait(int status, u8 *signaled, u32 *code) {
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

fn char *https_strdup(const char *str) {
  size_t len = strlen(str);
  char  *out = malloc(len + 1);
  if (!out) {
    return NULL;
  }

  memcpy(out, str, len);
  out[len] = '\0';
  return out;
}

fn char *https_join_path(const char *dir, const char *name) {
  size_t dlen = strlen(dir);
  size_t nlen = strlen(name);
  char  *out  = malloc(dlen + 1 + nlen + 1);
  if (!out) {
    return NULL;
  }

  memcpy(out, dir, dlen);
  out[dlen] = '/';
  memcpy(out + dlen + 1, name, nlen);
  out[dlen + 1 + nlen] = '\0';
  return out;
}

fn void https_paths_free(
  char **tmp_dir,
  char **hdr_path,
  char **body_path,
  char **meta_path,
  char **err_path
) {
  if (*hdr_path) {
    unlink(*hdr_path);
    free(*hdr_path);
    *hdr_path = NULL;
  }
  if (*body_path) {
    unlink(*body_path);
    free(*body_path);
    *body_path = NULL;
  }
  if (*meta_path) {
    unlink(*meta_path);
    free(*meta_path);
    *meta_path = NULL;
  }
  if (*err_path) {
    unlink(*err_path);
    free(*err_path);
    *err_path = NULL;
  }
  if (*tmp_dir) {
    rmdir(*tmp_dir);
    free(*tmp_dir);
    *tmp_dir = NULL;
  }
}

fn void https_slot_cleanup(HttpsSlot *slot) {
  https_paths_free(
    &slot->tmp_dir,
    &slot->hdr_path,
    &slot->body_path,
    &slot->meta_path,
    &slot->err_path
  );
}

fn u8 https_make_paths(
  char **tmp_dir,
  char **hdr_path,
  char **body_path,
  char **meta_path,
  char **err_path
) {
  char tmpl[] = "/tmp/hvm4_https_XXXXXX";
  char *dir   = https_strdup(tmpl);
  if (!dir) {
    return 0;
  }

  if (!mkdtemp(dir)) {
    free(dir);
    return 0;
  }

  char *hdr  = https_join_path(dir, "headers.txt");
  char *body = https_join_path(dir, "body.bin");
  char *meta = https_join_path(dir, "meta.txt");
  char *err  = https_join_path(dir, "err.txt");
  if (!hdr || !body || !meta || !err) {
    https_paths_free(&dir, &hdr, &body, &meta, &err);
    return 0;
  }

  *tmp_dir   = dir;
  *hdr_path  = hdr;
  *body_path = body;
  *meta_path = meta;
  *err_path  = err;
  return 1;
}

fn u8 https_read_status_code(const char *meta_path, u32 *status) {
  FILE *f = fopen(meta_path, "rb");
  if (!f) {
    return 0;
  }

  char   buf[64];
  size_t got = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[got] = '\0';

  char *p = buf;
  while (*p && !isdigit((unsigned char)*p)) {
    p++;
  }

  if (!*p) {
    return 0;
  }

  long code = strtol(p, NULL, 10);
  if (code < 0 || code > 9999) {
    return 0;
  }

  *status = (u32)code;
  return 1;
}

fn Term https_read_stderr_msg(const char *err_path, const char *fallback) {
  FILE *f = fopen(err_path, "rb");
  if (!f) {
    return term_string_from_utf8(fallback);
  }

  char   buf[1024];
  size_t got = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);

  while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) {
    got--;
  }
  buf[got] = '\0';

  if (got == 0) {
    return term_string_from_utf8(fallback);
  }

  return term_string_from_utf8(buf);
}

fn u8 https_read_body_bytes(const char *body_path, Term *body_out, Term *err_msg_out) {
  FILE *f = fopen(body_path, "rb");
  if (!f) {
    int err     = errno;
    *err_msg_out = term_string_printf(
      "failed to open body file '%s': %s (errno=%d)",
      body_path,
      strerror(err),
      err
    );
    return 0;
  }

  Term nil = term_new_ctr(NAM_NIL, 0, NULL);
  unsigned char c = 0;
  if (fread(&c, 1, 1, f) != 1) {
    if (ferror(f)) {
      int err     = errno;
      fclose(f);
      *err_msg_out = term_string_printf(
        "failed to read body file '%s': %s (errno=%d)",
        body_path,
        strerror(err),
        err
      );
      return 0;
    }
    fclose(f);
    *body_out = nil;
    return 1;
  }

  u32 count = 1;
  Term byt[1]    = {term_new_num(c)};
  Term head_tail[2] = {term_new_ctr(NAM_BYT, 1, byt), nil};
  Term result    = term_new_ctr(NAM_CON, 2, head_tail);
  Term curr      = result;

  while (fread(&c, 1, 1, f) == 1) {
    count++;
    if (count > HTTPS_BODY_CAP) {
      fclose(f);
      *err_msg_out = term_string_printf("response body too large (max %u bytes)", HTTPS_BODY_CAP);
      return 0;
    }

    byt[0]       = term_new_num(c);
    head_tail[0] = term_new_ctr(NAM_BYT, 1, byt);
    heap_set(term_val(curr) + 1, term_new_ctr(NAM_CON, 2, head_tail));
    curr = heap_read(term_val(curr) + 1);
  }

  if (ferror(f)) {
    int err     = errno;
    fclose(f);
    *err_msg_out = term_string_printf(
      "failed to read body file '%s': %s (errno=%d)",
      body_path,
      strerror(err),
      err
    );
    return 0;
  }

  fclose(f);
  *body_out = result;
  return 1;
}

fn Term https_build_outcome(
  u8 canceled,
  u8 signaled,
  u32 code,
  const char *meta_path,
  const char *body_path,
  const char *err_path
) {
  if (canceled) {
    return https_new_canceled();
  }

  if (signaled) {
    Term msg = term_string_printf("curl terminated by signal %u", code);
    return https_new_fail(HTTPS_FAIL_CURL_SIGNAL, msg);
  }

  if (code != 0) {
    Term msg = https_read_stderr_msg(err_path, "curl request failed");
    return https_new_fail(HTTPS_FAIL_CURL_EXIT, msg);
  }

  u32 status = 0;
  if (!https_read_status_code(meta_path, &status)) {
    Term msg = term_string_printf("failed to parse status from '%s'", meta_path);
    return https_new_fail(HTTPS_FAIL_PARSE, msg);
  }

  Term body    = term_new_era();
  Term body_err = term_new_era();
  if (!https_read_body_bytes(body_path, &body, &body_err)) {
    return https_new_fail(HTTPS_FAIL_BODY, body_err);
  }

  Term headers = term_new_ctr(NAM_NIL, 0, NULL);
  return https_new_resp(status, headers, body);
}

fn void https_set_outcome(u32 id, Term outcome) {
  pthread_mutex_lock(&HTTPS_LOCK);

  if (id != 0 && id < HTTPS_NEXT_ID && id < HTTPS_CAP) {
    HttpsSlot *slot = &HTTPS_SLOTS[id];
    slot->outcome   = outcome;
    slot->parsed    = 1;
    https_slot_cleanup(slot);
  }

  pthread_mutex_unlock(&HTTPS_LOCK);
}

fn pid_t https_waitpid_retry(pid_t pid, int *status, int opts) {
  while (1) {
    pid_t got = waitpid(pid, status, opts);
    if (got >= 0) {
      return got;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }
}

fn Term https_parse_and_store_outcome(
  u32 id,
  u8 canceled,
  u8 signaled,
  u32 code,
  const char *meta_path,
  const char *body_path,
  const char *err_path
) {
  Term outcome = https_build_outcome(canceled, signaled, code, meta_path, body_path, err_path);
  https_set_outcome(id, outcome);
  return outcome;
}

fn u8 https_claim(
  u32 id,
  u32 seq,
  pid_t *pid,
  u8 *finished,
  u8 *parsed,
  u8 *canceled,
  u8 *signaled,
  u32 *code,
  Term *outcome,
  char **body_path,
  char **meta_path,
  char **err_path
) {
  pthread_mutex_lock(&HTTPS_LOCK);

  if (id == 0 || id >= HTTPS_NEXT_ID || id >= HTTPS_CAP) {
    pthread_mutex_unlock(&HTTPS_LOCK);
    return 0;
  }

  HttpsSlot *slot = &HTTPS_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&HTTPS_LOCK);
    return 0;
  }

  slot->expected_seq = seq + 1;
  *pid               = slot->pid;
  *finished          = slot->finished;
  *parsed            = slot->parsed;
  *canceled          = slot->canceled;
  *signaled          = slot->signaled;
  *code              = slot->code;
  *outcome           = slot->outcome;
  *body_path         = slot->body_path;
  *meta_path         = slot->meta_path;
  *err_path          = slot->err_path;

  pthread_mutex_unlock(&HTTPS_LOCK);
  return 1;
}

#include "get.c"
#include "poll.c"
#include "wait.c"
#include "cancel.c"

fn void prim_https_init(void) {
  HTTPS_NAM_HTTP     = nick_from_str("Http", 4);
  HTTPS_NAM_PEND     = nick_from_str("Pend", 4);
  HTTPS_NAM_RDY      = nick_from_str("Rdy", 3);
  HTTPS_NAM_RESP     = nick_from_str("Resp", 4);
  HTTPS_NAM_HDR      = nick_from_str("Hdr", 3);
  HTTPS_NAM_FAIL     = nick_from_str("Fail", 4);
  HTTPS_NAM_CANCELED = nick_from_str("Canceled", 8);

  prim_https_get_init();
  prim_https_poll_init();
  prim_https_wait_init();
  prim_https_cancel_init();
}
