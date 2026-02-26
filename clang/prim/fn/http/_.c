#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#define HTTP_CAP             (1u << 18)
#define HTTP_ARG_CAP         512
#define HTTP_STR_CAP         8192
#define HTTP_HEADER_CAP      4096
#define HTTP_REASON_MSG_CAP  1024
#define HTTP_BODY_HARD_CAP   (1u << 24)
#define HTTP_DEFAULT_BODY_CAP (1u << 20)

#define HTTP_ERR_BAD_ARG    1
#define HTTP_ERR_BAD_HANDLE 2
#define HTTP_ERR_STALE      3
#define HTTP_ERR_FULL       4
#define HTTP_ERR_IO         5

typedef struct {
  char  method[16];
  char  url[HTTP_STR_CAP];
  char **headers;
  u32   headers_len;
  u32   headers_cap;
  u8    body_kind;
  char *body_text;
  u8   *body_bytes;
  u32   body_size;
  u32   body_cap;
  u32   timeout_ms;
  u32   connect_timeout_ms;
  u8    follow_redirects;
  u32   max_redirects;
  u8    verify_tls;
  u32   max_body_bytes;
} HttpReq;

typedef struct {
  u32   expected_seq;
  pid_t pid;
  u8    finished;
  u8    canceled;
  u8    signaled;
  u8    parsed;
  u32   code;
  u32   max_body_bytes;
  Term  outcome;

  char *tmp_dir;
  char *hdr_path;
  char *body_path;
  char *meta_path;
  char *err_path;
  char *req_path;
} HttpSlot;

typedef struct {
  char *name;
  char *value;
} HttpHdrPair;

static HttpSlot HTTP_SLOTS[HTTP_CAP];
static u32      HTTP_NEXT_ID = 1;
static pthread_mutex_t HTTP_LOCK = PTHREAD_MUTEX_INITIALIZER;

// Core async/result constructors
static u32 HTTP_NAM_HTTP     = 0;
static u32 HTTP_NAM_PEND     = 0;
static u32 HTTP_NAM_RDY      = 0;
static u32 HTTP_NAM_RESP     = 0;
static u32 HTTP_NAM_HDR      = 0;
static u32 HTTP_NAM_FAIL     = 0;
static u32 HTTP_NAM_CANCELED = 0;

// Fail reason constructors
static u32 HTTP_NAM_TIMEOUT       = 0;
static u32 HTTP_NAM_DNS           = 0;
static u32 HTTP_NAM_CONNECT       = 0;
static u32 HTTP_NAM_TLS           = 0;
static u32 HTTP_NAM_PROTOCOL      = 0;
static u32 HTTP_NAM_CURL_EXIT     = 0;
static u32 HTTP_NAM_CURL_SIGNAL   = 0;
static u32 HTTP_NAM_PARSE         = 0;
static u32 HTTP_NAM_BODY_TOO_LARGE = 0;
static u32 HTTP_NAM_IO            = 0;

// Request constructors
static u32 HTTP_NAM_REQ       = 0;
static u32 HTTP_NAM_GET       = 0;
static u32 HTTP_NAM_POST      = 0;
static u32 HTTP_NAM_PUT       = 0;
static u32 HTTP_NAM_PATCH     = 0;
static u32 HTTP_NAM_DELETE    = 0;
static u32 HTTP_NAM_HEAD      = 0;
static u32 HTTP_NAM_OPTIONS   = 0;
static u32 HTTP_NAM_NOBODY    = 0;
static u32 HTTP_NAM_BODY_TEXT = 0;
static u32 HTTP_NAM_BODY_BYTES = 0;
static u32 HTTP_NAM_OPTS      = 0;
static u32 HTTP_NAM_T         = 0;
static u32 HTTP_NAM_F         = 0;

#define HTTP_BODY_NONE 0
#define HTTP_BODY_TEXT 1
#define HTTP_BODY_BYTES 2

fn Term wnf(Term term);

fn Term http_new_err(const char *prim, u32 code, const char *msg) {
  Term txt = term_string_printf("ERROR(%s): E%u %s", prim, code, msg);
  return term_new_ctr(SYM_ERR, 1, &txt);
}

fn Term http_new_ok(Term val) {
  return term_new_ctr(SYM_OK, 1, &val);
}

fn Term http_new_http(u32 id, u32 seq) {
  Term args[2] = {term_new_num(id), term_new_num(seq)};
  return term_new_ctr(HTTP_NAM_HTTP, 2, args);
}

fn Term http_new_pend(u32 id, u32 seq) {
  Term http = http_new_http(id, seq);
  return term_new_ctr(HTTP_NAM_PEND, 1, &http);
}

fn Term http_new_rdy(u32 id, u32 seq, Term outcome) {
  Term http   = http_new_http(id, seq);
  Term args[2] = {http, outcome};
  return term_new_ctr(HTTP_NAM_RDY, 2, args);
}

fn Term http_new_reason0(u32 nam) {
  return term_new_ctr(nam, 0, NULL);
}

fn Term http_new_reason1(u32 nam, u32 val) {
  Term arg = term_new_num(val);
  return term_new_ctr(nam, 1, &arg);
}

fn Term http_new_fail(Term reason, Term msg) {
  Term args[2] = {reason, msg};
  return term_new_ctr(HTTP_NAM_FAIL, 2, args);
}

fn Term http_new_canceled(void) {
  return term_new_ctr(HTTP_NAM_CANCELED, 0, NULL);
}

fn Term http_new_hdr(Term name, Term value) {
  Term args[2] = {name, value};
  return term_new_ctr(HTTP_NAM_HDR, 2, args);
}

fn Term http_new_resp(u32 status, Term headers, Term body) {
  Term args[3] = {term_new_num(status), headers, body};
  return term_new_ctr(HTTP_NAM_RESP, 3, args);
}

fn u8 http_parse_num(Term term, u32 *out) {
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

fn u8 http_parse_bool(Term term, u8 *out) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C00: {
      u32 ext = term_ext(val);
      if (ext == HTTP_NAM_T) {
        *out = 1;
        return 1;
      }
      if (ext == HTTP_NAM_F) {
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

fn u8 http_parse_handle(Term term, u32 *id, u32 *seq) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C02: {
      if (term_ext(val) != HTTP_NAM_HTTP) {
        return 0;
      }

      u32  loc    = term_val(val);
      Term id_tm  = heap_read(loc + 0);
      Term seq_tm = heap_read(loc + 1);

      if (!http_parse_num(id_tm, id)) {
        return 0;
      }
      if (!http_parse_num(seq_tm, seq)) {
        return 0;
      }
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn u8 http_is_valid_id(u32 id) {
  pthread_mutex_lock(&HTTP_LOCK);

  if (id == 0 || id >= HTTP_NEXT_ID || id >= HTTP_CAP) {
    pthread_mutex_unlock(&HTTP_LOCK);
    return 0;
  }

  pthread_mutex_unlock(&HTTP_LOCK);
  return 1;
}

fn void http_set_finished(u32 id, u8 signaled, u32 code) {
  pthread_mutex_lock(&HTTP_LOCK);

  if (id != 0 && id < HTTP_NEXT_ID && id < HTTP_CAP) {
    HttpSlot *slot = &HTTP_SLOTS[id];
    slot->finished = 1;
    slot->signaled = signaled;
    slot->code     = code;
  }

  pthread_mutex_unlock(&HTTP_LOCK);
}

fn void http_set_canceled(u32 id) {
  pthread_mutex_lock(&HTTP_LOCK);

  if (id != 0 && id < HTTP_NEXT_ID && id < HTTP_CAP) {
    HTTP_SLOTS[id].canceled = 1;
  }

  pthread_mutex_unlock(&HTTP_LOCK);
}

fn void http_status_from_wait(int status, u8 *signaled, u32 *code) {
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

fn char *http_strdup(const char *str) {
  size_t len = strlen(str);
  char  *out = malloc(len + 1);
  if (!out) {
    return NULL;
  }

  memcpy(out, str, len);
  out[len] = '\0';
  return out;
}

fn char *http_join_path(const char *dir, const char *name) {
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

fn void http_paths_free(
  char **tmp_dir,
  char **hdr_path,
  char **body_path,
  char **meta_path,
  char **err_path,
  char **req_path
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
  if (*req_path) {
    unlink(*req_path);
    free(*req_path);
    *req_path = NULL;
  }
  if (*tmp_dir) {
    rmdir(*tmp_dir);
    free(*tmp_dir);
    *tmp_dir = NULL;
  }
}

fn void http_slot_cleanup(HttpSlot *slot) {
  http_paths_free(
    &slot->tmp_dir,
    &slot->hdr_path,
    &slot->body_path,
    &slot->meta_path,
    &slot->err_path,
    &slot->req_path
  );
}

fn u8 http_make_paths(
  char **tmp_dir,
  char **hdr_path,
  char **body_path,
  char **meta_path,
  char **err_path,
  char **req_path
) {
  char tmpl[] = "/tmp/hvm4_http_XXXXXX";
  char *dir   = http_strdup(tmpl);
  if (!dir) {
    return 0;
  }

  if (!mkdtemp(dir)) {
    free(dir);
    return 0;
  }

  char *hdr = http_join_path(dir, "headers.txt");
  char *bod = http_join_path(dir, "body.bin");
  char *met = http_join_path(dir, "meta.txt");
  char *err = http_join_path(dir, "err.txt");
  char *req = http_join_path(dir, "request.bin");
  if (!hdr || !bod || !met || !err || !req) {
    http_paths_free(&dir, &hdr, &bod, &met, &err, &req);
    return 0;
  }

  *tmp_dir   = dir;
  *hdr_path  = hdr;
  *body_path = bod;
  *meta_path = met;
  *err_path  = err;
  *req_path  = req;
  return 1;
}

fn u8 http_read_status_code(const char *meta_path, u32 *status) {
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

fn Term http_read_stderr_msg(const char *err_path, const char *fallback) {
  FILE *f = fopen(err_path, "rb");
  if (!f) {
    return term_string_from_utf8(fallback);
  }

  char   buf[HTTP_REASON_MSG_CAP];
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

fn u8 http_req_init(HttpReq *req) {
  req->method[0]          = '\0';
  req->url[0]             = '\0';
  req->headers            = NULL;
  req->headers_len        = 0;
  req->headers_cap        = 0;
  req->body_kind          = HTTP_BODY_NONE;
  req->body_text          = NULL;
  req->body_bytes         = NULL;
  req->body_size          = 0;
  req->body_cap           = 0;
  req->timeout_ms         = 0;
  req->connect_timeout_ms = 0;
  req->follow_redirects   = 0;
  req->max_redirects      = 0;
  req->verify_tls         = 1;
  req->max_body_bytes     = HTTP_DEFAULT_BODY_CAP;
  return 1;
}

fn void http_req_free(HttpReq *req) {
  if (req->headers) {
    for (u32 i = 0; i < req->headers_len; ++i) {
      free(req->headers[i]);
    }
    free(req->headers);
    req->headers = NULL;
  }

  if (req->body_text) {
    free(req->body_text);
    req->body_text = NULL;
  }

  if (req->body_bytes) {
    free(req->body_bytes);
    req->body_bytes = NULL;
  }

  req->headers_len = 0;
  req->headers_cap = 0;
  req->body_size   = 0;
  req->body_cap    = 0;
}

fn u8 http_req_push_header(HttpReq *req, char *line) {
  if (req->headers_len == req->headers_cap) {
    u32 next_cap = req->headers_cap == 0 ? 8 : req->headers_cap * 2;
    char **next  = realloc(req->headers, (size_t)next_cap * sizeof(char *));
    if (!next) {
      return 0;
    }
    req->headers     = next;
    req->headers_cap = next_cap;
  }

  req->headers[req->headers_len] = line;
  req->headers_len               = req->headers_len + 1;
  return 1;
}

fn u8 http_req_push_body_byte(HttpReq *req, u8 byte) {
  u32 len = req->body_size;

  if (len >= HTTP_BODY_HARD_CAP) {
    return 0;
  }

  if (len == req->body_cap) {
    u32 next_cap = req->body_cap == 0 ? 256 : req->body_cap * 2;
    if (next_cap > HTTP_BODY_HARD_CAP) {
      next_cap = HTTP_BODY_HARD_CAP;
    }
    if (next_cap == req->body_cap) {
      return 0;
    }

    u8 *next = realloc(req->body_bytes, (size_t)next_cap);
    if (!next) {
      return 0;
    }
    req->body_bytes = next;
    req->body_cap   = next_cap;
  }

  req->body_bytes[len] = byte;
  req->body_size       = len + 1;
  return 1;
}

fn u8 http_parse_method(Term term, char out[16]) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C00: {
      u32 ext = term_ext(val);
      if (ext == HTTP_NAM_GET) {
        strcpy(out, "GET");
        return 1;
      }
      if (ext == HTTP_NAM_POST) {
        strcpy(out, "POST");
        return 1;
      }
      if (ext == HTTP_NAM_PUT) {
        strcpy(out, "PUT");
        return 1;
      }
      if (ext == HTTP_NAM_PATCH) {
        strcpy(out, "PATCH");
        return 1;
      }
      if (ext == HTTP_NAM_DELETE) {
        strcpy(out, "DELETE");
        return 1;
      }
      if (ext == HTTP_NAM_HEAD) {
        strcpy(out, "HEAD");
        return 1;
      }
      if (ext == HTTP_NAM_OPTIONS) {
        strcpy(out, "OPTIONS");
        return 1;
      }
      return 0;
    }
    default: {
      return 0;
    }
  }
}

fn u8 http_parse_header_item(Term term, HttpReq *req, Term *err_out) {
  Term val = wnf(term);

  if (term_tag(val) != C02 || term_ext(val) != HTTP_NAM_HDR) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `headers`; expected List<#Hdr{name,value}>");
    return 0;
  }

  u32  loc      = term_val(val);
  Term name_tm  = heap_read(loc + 0);
  Term value_tm = heap_read(loc + 1);

  char name[HTTP_HEADER_CAP];
  char value[HTTP_HEADER_CAP];

  HStrErr name_err;
  if (!term_string_to_utf8_cstr(name_tm, name, HTTP_HEADER_CAP, NULL, &name_err)) {
    *err_out = term_string_from_hstrerr("http_request", "header_name", HTTP_HEADER_CAP, name_err);
    return 0;
  }

  HStrErr value_err;
  if (!term_string_to_utf8_cstr(value_tm, value, HTTP_HEADER_CAP, NULL, &value_err)) {
    *err_out = term_string_from_hstrerr("http_request", "header_value", HTTP_HEADER_CAP, value_err);
    return 0;
  }

  size_t nlen = strlen(name);
  size_t vlen = strlen(value);
  char  *line = malloc(nlen + 2 + vlen + 1);
  if (!line) {
    *err_out = http_new_err("http_request", HTTP_ERR_IO, "out of memory while building headers");
    return 0;
  }

  memcpy(line, name, nlen);
  line[nlen + 0] = ':';
  line[nlen + 1] = ' ';
  memcpy(line + nlen + 2, value, vlen);
  line[nlen + 2 + vlen] = '\0';

  if (!http_req_push_header(req, line)) {
    free(line);
    *err_out = http_new_err("http_request", HTTP_ERR_IO, "out of memory while storing headers");
    return 0;
  }

  return 1;
}

fn u8 http_parse_headers(Term term, HttpReq *req, Term *err_out) {
  Term cur = wnf(term);

  while (term_tag(cur) == C02 && term_ext(cur) == SYM_CON) {
    u32  loc  = term_val(cur);
    Term head = heap_read(loc + 0);
    Term tail = heap_read(loc + 1);

    if (!http_parse_header_item(head, req, err_out)) {
      return 0;
    }

    cur = wnf(tail);
  }

  if (term_tag(cur) != C00 || term_ext(cur) != SYM_NIL) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `headers`; expected List<#Hdr{name,value}>");
    return 0;
  }

  return 1;
}

fn u8 http_parse_body_bytes_list(Term term, HttpReq *req, Term *err_out) {
  Term cur = wnf(term);

  while (term_tag(cur) == C02 && term_ext(cur) == SYM_CON) {
    u32  loc  = term_val(cur);
    Term head = wnf(heap_read(loc + 0));
    Term tail = heap_read(loc + 1);

    if (term_tag(head) != C01 || term_ext(head) != SYM_BYT) {
      *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `body`; expected List<#BYT{n}>");
      return 0;
    }

    Term num = wnf(heap_read(term_val(head)));
    if (term_tag(num) != NUM) {
      *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `body`; expected #BYT{NUM}");
      return 0;
    }

    u32 val = term_val(num);
    if (val > 255) {
      *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `body`; byte must be in [0,255]");
      return 0;
    }

    if (!http_req_push_body_byte(req, (u8)val)) {
      *err_out = http_new_err("http_request", HTTP_ERR_IO, "out of memory while decoding body bytes");
      return 0;
    }

    cur = wnf(tail);
  }

  if (term_tag(cur) != C00 || term_ext(cur) != SYM_NIL) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `body`; expected List<#BYT{n}>");
    return 0;
  }

  return 1;
}

fn u8 http_parse_body(Term term, HttpReq *req, Term *err_out) {
  Term val = wnf(term);

  switch (term_tag(val)) {
    case C00: {
      if (term_ext(val) != HTTP_NAM_NOBODY) {
        *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `body`; expected #NoBody|#BodyText|#BodyBytes");
        return 0;
      }
      req->body_kind = HTTP_BODY_NONE;
      return 1;
    }
    case C01: {
      u32 ext = term_ext(val);
      u32 loc = term_val(val);

      if (ext == HTTP_NAM_BODY_TEXT) {
        char *text = malloc(HTTP_BODY_HARD_CAP);
        if (!text) {
          *err_out = http_new_err("http_request", HTTP_ERR_IO, "out of memory while decoding body text");
          return 0;
        }

        HStrErr text_err;
        if (!term_string_to_utf8_cstr(heap_read(loc), text, HTTP_BODY_HARD_CAP, NULL, &text_err)) {
          free(text);
          *err_out = term_string_from_hstrerr("http_request", "body_text", HTTP_BODY_HARD_CAP, text_err);
          return 0;
        }

        req->body_text = text;

        req->body_kind = HTTP_BODY_TEXT;
        req->body_size = (u32)strlen(req->body_text);
        return 1;
      }

      if (ext == HTTP_NAM_BODY_BYTES) {
        if (!http_parse_body_bytes_list(heap_read(loc), req, err_out)) {
          return 0;
        }
        req->body_kind = HTTP_BODY_BYTES;
        return 1;
      }

      *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `body`; expected #NoBody|#BodyText|#BodyBytes");
      return 0;
    }
    default: {
      *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `body`; expected #NoBody|#BodyText|#BodyBytes");
      return 0;
    }
  }
}

fn u8 http_parse_opts(Term term, HttpReq *req, Term *err_out) {
  Term val = wnf(term);

  if (term_tag(val) != C06 || term_ext(val) != HTTP_NAM_OPTS) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts`; expected #Opts{timeout_ms,connect_timeout_ms,follow_redirects,max_redirects,verify_tls,max_body_bytes}");
    return 0;
  }

  u32 loc = term_val(val);

  if (!http_parse_num(heap_read(loc + 0), &req->timeout_ms)) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts.timeout_ms`; expected NUM");
    return 0;
  }
  if (!http_parse_num(heap_read(loc + 1), &req->connect_timeout_ms)) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts.connect_timeout_ms`; expected NUM");
    return 0;
  }
  if (!http_parse_bool(heap_read(loc + 2), &req->follow_redirects)) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts.follow_redirects`; expected #T{}|#F{}");
    return 0;
  }
  if (!http_parse_num(heap_read(loc + 3), &req->max_redirects)) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts.max_redirects`; expected NUM");
    return 0;
  }
  if (!http_parse_bool(heap_read(loc + 4), &req->verify_tls)) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts.verify_tls`; expected #T{}|#F{}");
    return 0;
  }
  if (!http_parse_num(heap_read(loc + 5), &req->max_body_bytes)) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts.max_body_bytes`; expected NUM");
    return 0;
  }

  if (req->max_body_bytes == 0 || req->max_body_bytes > HTTP_BODY_HARD_CAP) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `opts.max_body_bytes`; must be in [1,HTTP_BODY_HARD_CAP]");
    return 0;
  }

  return 1;
}

fn u8 http_parse_request(Term req_tm, HttpReq *req, Term *err_out) {
  Term val = wnf(req_tm);

  if (term_tag(val) != C05 || term_ext(val) != HTTP_NAM_REQ) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `req`; expected #Req{method,url,headers,body,opts}");
    return 0;
  }

  u32  loc      = term_val(val);
  Term method   = heap_read(loc + 0);
  Term url      = heap_read(loc + 1);
  Term headers  = heap_read(loc + 2);
  Term body     = heap_read(loc + 3);
  Term opts     = heap_read(loc + 4);

  if (!http_parse_method(method, req->method)) {
    *err_out = http_new_err("http_request", HTTP_ERR_BAD_ARG, "invalid `req.method`; expected #Get|#Post|#Put|#Patch|#Delete|#Head|#Options");
    return 0;
  }

  HStrErr url_err;
  if (!term_string_to_utf8_cstr(url, req->url, HTTP_STR_CAP, NULL, &url_err)) {
    *err_out = term_string_from_hstrerr("http_request", "url", HTTP_STR_CAP, url_err);
    return 0;
  }

  if (!http_parse_headers(headers, req, err_out)) {
    return 0;
  }

  if (!http_parse_body(body, req, err_out)) {
    return 0;
  }

  if (!http_parse_opts(opts, req, err_out)) {
    return 0;
  }

  return 1;
}

fn u8 http_write_all(int fd, const u8 *buf, u32 len) {
  u32 off = 0;
  while (off < len) {
    ssize_t wrote = write(fd, buf + off, (size_t)(len - off));
    if (wrote > 0) {
      off = off + (u32)wrote;
      continue;
    }
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    return 0;
  }

  return 1;
}

fn u8 http_write_body_file(const char *path, const HttpReq *req) {
  if (req->body_kind != HTTP_BODY_BYTES) {
    return 1;
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return 0;
  }

  u8 ok = http_write_all(fd, req->body_bytes, req->body_size);
  int close_ret = close(fd);
  if (!ok || close_ret < 0) {
    return 0;
  }

  return 1;
}

fn int http_open_trunc_file(const char *path) {
  while (1) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      return fd;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }
}

fn int http_dup2_retry(int old_fd, int new_fd) {
  while (1) {
    if (dup2(old_fd, new_fd) >= 0) {
      return 1;
    }
    if (errno == EINTR) {
      continue;
    }
    return 0;
  }
}

fn u8 http_argv_push(char **argv, u32 *argc, char *arg) {
  if (*argc >= HTTP_ARG_CAP - 1) {
    return 0;
  }

  argv[*argc] = arg;
  *argc       = *argc + 1;
  return 1;
}

fn void http_child_exec_request(
  const HttpReq *req,
  const char *hdr_path,
  const char *body_path,
  const char *meta_path,
  const char *err_path,
  const char *req_path
) {
  int meta_fd = http_open_trunc_file(meta_path);
  if (meta_fd < 0) {
    _exit(127);
  }

  int err_fd = http_open_trunc_file(err_path);
  if (err_fd < 0) {
    close(meta_fd);
    _exit(127);
  }

  if (!http_dup2_retry(meta_fd, STDOUT_FILENO)) {
    close(meta_fd);
    close(err_fd);
    _exit(127);
  }

  if (!http_dup2_retry(err_fd, STDERR_FILENO)) {
    close(meta_fd);
    close(err_fd);
    _exit(127);
  }

  close(meta_fd);
  close(err_fd);

  char timeout_arg[64];
  char connect_timeout_arg[64];
  char max_redirs_arg[32];
  char data_path_arg[HTTP_STR_CAP + 16];

  snprintf(timeout_arg, sizeof(timeout_arg), "%.3f", (double)req->timeout_ms / 1000.0);
  snprintf(connect_timeout_arg, sizeof(connect_timeout_arg), "%.3f", (double)req->connect_timeout_ms / 1000.0);
  snprintf(max_redirs_arg, sizeof(max_redirs_arg), "%u", req->max_redirects);
  snprintf(data_path_arg, sizeof(data_path_arg), "@%s", req_path);

  char *argv[HTTP_ARG_CAP];
  u32   argc = 0;

  if (!http_argv_push(argv, &argc, "curl")) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, "-sS")) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, "-X")) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, (char *)req->method)) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, "-D")) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, (char *)hdr_path)) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, "-o")) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, (char *)body_path)) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, "-w")) {
    _exit(127);
  }
  if (!http_argv_push(argv, &argc, "%{http_code}\n")) {
    _exit(127);
  }

  if (req->timeout_ms > 0) {
    if (!http_argv_push(argv, &argc, "--max-time")) {
      _exit(127);
    }
    if (!http_argv_push(argv, &argc, timeout_arg)) {
      _exit(127);
    }
  }

  if (req->connect_timeout_ms > 0) {
    if (!http_argv_push(argv, &argc, "--connect-timeout")) {
      _exit(127);
    }
    if (!http_argv_push(argv, &argc, connect_timeout_arg)) {
      _exit(127);
    }
  }

  if (req->follow_redirects) {
    if (!http_argv_push(argv, &argc, "-L")) {
      _exit(127);
    }
    if (!http_argv_push(argv, &argc, "--max-redirs")) {
      _exit(127);
    }
    if (!http_argv_push(argv, &argc, max_redirs_arg)) {
      _exit(127);
    }
  }

  if (!req->verify_tls) {
    if (!http_argv_push(argv, &argc, "-k")) {
      _exit(127);
    }
  }

  for (u32 i = 0; i < req->headers_len; ++i) {
    if (!http_argv_push(argv, &argc, "-H")) {
      _exit(127);
    }
    if (!http_argv_push(argv, &argc, req->headers[i])) {
      _exit(127);
    }
  }

  if (req->body_kind == HTTP_BODY_TEXT) {
    if (!http_argv_push(argv, &argc, "--data-binary")) {
      _exit(127);
    }
    if (!http_argv_push(argv, &argc, req->body_text)) {
      _exit(127);
    }
  }

  if (req->body_kind == HTTP_BODY_BYTES) {
    if (!http_argv_push(argv, &argc, "--data-binary")) {
      _exit(127);
    }
    if (!http_argv_push(argv, &argc, data_path_arg)) {
      _exit(127);
    }
  }

  if (!http_argv_push(argv, &argc, (char *)req->url)) {
    _exit(127);
  }

  argv[argc] = NULL;
  execvp("curl", argv);
  _exit(127);
}

fn Term http_reason_from_curl_exit(u32 code) {
  switch (code) {
    case 6: {
      return http_new_reason0(HTTP_NAM_DNS);
    }
    case 7: {
      return http_new_reason0(HTTP_NAM_CONNECT);
    }
    case 28: {
      return http_new_reason0(HTTP_NAM_TIMEOUT);
    }

    case 35:
    case 51:
    case 53:
    case 54:
    case 58:
    case 59:
    case 60:
    case 64:
    case 66:
    case 77:
    case 80:
    case 82:
    case 83:
    case 90:
    case 91:
    case 95:
    case 96: {
      return http_new_reason0(HTTP_NAM_TLS);
    }

    case 1:
    case 3:
    case 4:
    case 5:
    case 8:
    case 9:
    case 16:
    case 22:
    case 47:
    case 52:
    case 56: {
      return http_new_reason0(HTTP_NAM_PROTOCOL);
    }

    default: {
      return http_new_reason1(HTTP_NAM_CURL_EXIT, code);
    }
  }
}

fn int http_read_header_pairs(const char *hdr_path, HttpHdrPair **pairs_out, u32 *len_out, int *io_err_out) {
  FILE *f = fopen(hdr_path, "rb");
  if (!f) {
    *io_err_out = errno;
    return 0;
  }

  HttpHdrPair *pairs = NULL;
  u32          len   = 0;
  u32          cap   = 0;
  char        *line  = NULL;
  size_t       line_cap = 0;

  while (1) {
    ssize_t got = getline(&line, &line_cap, f);
    if (got < 0) {
      break;
    }

    while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) {
      line[got - 1] = '\0';
      got--;
    }

    if (got == 0) {
      continue;
    }

    if (strncmp(line, "HTTP/", 5) == 0) {
      for (u32 i = 0; i < len; ++i) {
        free(pairs[i].name);
        free(pairs[i].value);
      }
      len = 0;
      continue;
    }

    char *colon = strchr(line, ':');
    if (!colon) {
      continue;
    }

    *colon = '\0';
    char *name  = line;
    char *value = colon + 1;

    while (*value == ' ' || *value == '\t') {
      value++;
    }

    if (*name == '\0') {
      continue;
    }

    char *name_copy  = http_strdup(name);
    char *value_copy = http_strdup(value);
    if (!name_copy || !value_copy) {
      free(name_copy);
      free(value_copy);
      free(line);
      for (u32 i = 0; i < len; ++i) {
        free(pairs[i].name);
        free(pairs[i].value);
      }
      free(pairs);
      fclose(f);
      *io_err_out = ENOMEM;
      return 0;
    }

    if (len == cap) {
      u32 next_cap = cap == 0 ? 8 : cap * 2;
      HttpHdrPair *next = realloc(pairs, (size_t)next_cap * sizeof(HttpHdrPair));
      if (!next) {
        free(name_copy);
        free(value_copy);
        free(line);
        for (u32 i = 0; i < len; ++i) {
          free(pairs[i].name);
          free(pairs[i].value);
        }
        free(pairs);
        fclose(f);
        *io_err_out = ENOMEM;
        return 0;
      }
      pairs = next;
      cap   = next_cap;
    }

    pairs[len].name  = name_copy;
    pairs[len].value = value_copy;
    len = len + 1;
  }

  free(line);

  if (ferror(f)) {
    int err = errno;
    for (u32 i = 0; i < len; ++i) {
      free(pairs[i].name);
      free(pairs[i].value);
    }
    free(pairs);
    fclose(f);
    *io_err_out = err;
    return 0;
  }

  fclose(f);
  *pairs_out = pairs;
  *len_out   = len;
  return 1;
}

fn Term http_pairs_to_term(HttpHdrPair *pairs, u32 len) {
  Term out = term_new_ctr(SYM_NIL, 0, NULL);

  for (u32 i = len; i > 0; --i) {
    Term name = term_string_from_utf8(pairs[i - 1].name);
    Term val  = term_string_from_utf8(pairs[i - 1].value);
    Term hdr  = http_new_hdr(name, val);
    Term args[2] = {hdr, out};
    out = term_new_ctr(SYM_CON, 2, args);
  }

  return out;
}

fn int http_read_body_bytes(const char *body_path, u32 cap, Term *body_out, int *io_err_out) {
  FILE *f = fopen(body_path, "rb");
  if (!f) {
    *io_err_out = errno;
    return -1;
  }

  Term nil = term_new_ctr(SYM_NIL, 0, NULL);
  u8   byte = 0;
  int  first = fgetc(f);
  if (first == EOF) {
    if (ferror(f)) {
      int err = errno;
      fclose(f);
      *io_err_out = err;
      return -1;
    }
    fclose(f);
    *body_out = nil;
    return 1;
  }

  byte = (u8)first;
  u32  count = 1;

  Term byt_num[1] = {term_new_num(byte)};
  Term node[2]    = {term_new_ctr(SYM_BYT, 1, byt_num), nil};
  Term out        = term_new_ctr(SYM_CON, 2, node);
  Term curr       = out;

  while (1) {
    int got = fgetc(f);
    if (got == EOF) {
      break;
    }

    count = count + 1;
    if (count > cap) {
      fclose(f);
      return 0;
    }

    byt_num[0] = term_new_num((u8)got);
    node[0]    = term_new_ctr(SYM_BYT, 1, byt_num);
    heap_set(term_val(curr) + 1, term_new_ctr(SYM_CON, 2, node));
    curr = heap_read(term_val(curr) + 1);
  }

  if (ferror(f)) {
    int err = errno;
    fclose(f);
    *io_err_out = err;
    return -1;
  }

  fclose(f);
  *body_out = out;
  return 1;
}

fn Term http_build_outcome(
  u8 canceled,
  u8 signaled,
  u32 code,
  u32 max_body_bytes,
  const char *hdr_path,
  const char *meta_path,
  const char *body_path,
  const char *err_path
) {
  if (canceled) {
    return http_new_canceled();
  }

  if (signaled) {
    Term rsn = http_new_reason1(HTTP_NAM_CURL_SIGNAL, code);
    Term msg = term_string_printf("curl terminated by signal %u", code);
    return http_new_fail(rsn, msg);
  }

  if (code != 0) {
    Term rsn = http_reason_from_curl_exit(code);
    Term msg = http_read_stderr_msg(err_path, "curl request failed");
    return http_new_fail(rsn, msg);
  }

  u32 status = 0;
  if (!http_read_status_code(meta_path, &status)) {
    Term rsn = http_new_reason0(HTTP_NAM_PARSE);
    Term msg = term_string_printf("failed to parse HTTP status from '%s'", meta_path);
    return http_new_fail(rsn, msg);
  }

  HttpHdrPair *pairs  = NULL;
  u32          pair_n = 0;
  int          hdr_io_err = 0;
  if (!http_read_header_pairs(hdr_path, &pairs, &pair_n, &hdr_io_err)) {
    Term rsn = http_new_reason1(HTTP_NAM_IO, (u32)hdr_io_err);
    Term msg = term_string_printf(
      "failed to read response headers '%s': %s (errno=%d)",
      hdr_path,
      strerror(hdr_io_err),
      hdr_io_err
    );
    return http_new_fail(rsn, msg);
  }

  Term headers = http_pairs_to_term(pairs, pair_n);

  for (u32 i = 0; i < pair_n; ++i) {
    free(pairs[i].name);
    free(pairs[i].value);
  }
  free(pairs);

  Term body = term_new_era();
  int  body_io_err = 0;
  int  body_ret = http_read_body_bytes(body_path, max_body_bytes, &body, &body_io_err);
  if (body_ret == 0) {
    Term rsn = http_new_reason1(HTTP_NAM_BODY_TOO_LARGE, max_body_bytes);
    Term msg = term_string_printf("response body too large (limit=%u bytes)", max_body_bytes);
    return http_new_fail(rsn, msg);
  }
  if (body_ret < 0) {
    Term rsn = http_new_reason1(HTTP_NAM_IO, (u32)body_io_err);
    Term msg = term_string_printf(
      "failed to read response body '%s': %s (errno=%d)",
      body_path,
      strerror(body_io_err),
      body_io_err
    );
    return http_new_fail(rsn, msg);
  }

  return http_new_resp(status, headers, body);
}

fn void http_set_outcome(u32 id, Term outcome) {
  pthread_mutex_lock(&HTTP_LOCK);

  if (id != 0 && id < HTTP_NEXT_ID && id < HTTP_CAP) {
    HttpSlot *slot = &HTTP_SLOTS[id];
    slot->outcome  = outcome;
    slot->parsed   = 1;
    http_slot_cleanup(slot);
  }

  pthread_mutex_unlock(&HTTP_LOCK);
}

fn pid_t http_waitpid_retry(pid_t pid, int *status, int opts) {
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

fn Term http_parse_and_store_outcome(
  u32 id,
  u8 canceled,
  u8 signaled,
  u32 code,
  u32 max_body_bytes,
  const char *hdr_path,
  const char *meta_path,
  const char *body_path,
  const char *err_path
) {
  Term outcome = http_build_outcome(
    canceled,
    signaled,
    code,
    max_body_bytes,
    hdr_path,
    meta_path,
    body_path,
    err_path
  );

  http_set_outcome(id, outcome);
  return outcome;
}

fn u8 http_claim(
  u32 id,
  u32 seq,
  pid_t *pid,
  u8 *finished,
  u8 *parsed,
  u8 *canceled,
  u8 *signaled,
  u32 *code,
  u32 *max_body_bytes,
  Term *outcome,
  char **hdr_path,
  char **meta_path,
  char **body_path,
  char **err_path
) {
  pthread_mutex_lock(&HTTP_LOCK);

  if (id == 0 || id >= HTTP_NEXT_ID || id >= HTTP_CAP) {
    pthread_mutex_unlock(&HTTP_LOCK);
    return 0;
  }

  HttpSlot *slot = &HTTP_SLOTS[id];
  if (slot->expected_seq != seq) {
    pthread_mutex_unlock(&HTTP_LOCK);
    return 0;
  }

  slot->expected_seq = seq + 1;
  *pid               = slot->pid;
  *finished          = slot->finished;
  *parsed            = slot->parsed;
  *canceled          = slot->canceled;
  *signaled          = slot->signaled;
  *code              = slot->code;
  *max_body_bytes    = slot->max_body_bytes;
  *outcome           = slot->outcome;
  *hdr_path          = slot->hdr_path;
  *meta_path         = slot->meta_path;
  *body_path         = slot->body_path;
  *err_path          = slot->err_path;

  pthread_mutex_unlock(&HTTP_LOCK);
  return 1;
}

#include "request.c"
#include "poll.c"
#include "wait.c"
#include "cancel.c"

fn void prim_http_init(void) {
  HTTP_NAM_HTTP      = table_find("Http", 4);
  HTTP_NAM_PEND      = table_find("Pend", 4);
  HTTP_NAM_RDY       = table_find("Rdy", 3);
  HTTP_NAM_RESP      = table_find("Resp", 4);
  HTTP_NAM_HDR       = table_find("Hdr", 3);
  HTTP_NAM_FAIL      = table_find("HttpFail", 8);
  HTTP_NAM_CANCELED  = table_find("Canceled", 8);

  HTTP_NAM_TIMEOUT        = table_find("HttpTimeout", 11);
  HTTP_NAM_DNS            = table_find("HttpDns", 7);
  HTTP_NAM_CONNECT        = table_find("Connect", 7);
  HTTP_NAM_TLS            = table_find("Tls", 3);
  HTTP_NAM_PROTOCOL       = table_find("HttpProtocol", 12);
  HTTP_NAM_CURL_EXIT      = table_find("CurlExit", 8);
  HTTP_NAM_CURL_SIGNAL    = table_find("CurlSignal", 10);
  HTTP_NAM_PARSE          = table_find("Parse", 5);
  HTTP_NAM_BODY_TOO_LARGE = table_find("BodyTooLarge", 12);
  HTTP_NAM_IO             = table_find("Io", 2);

  HTTP_NAM_REQ        = table_find("Req", 3);
  HTTP_NAM_GET        = table_find("Get", 3);
  HTTP_NAM_POST       = table_find("Post", 4);
  HTTP_NAM_PUT        = table_find("Put", 3);
  HTTP_NAM_PATCH      = table_find("Patch", 5);
  HTTP_NAM_DELETE     = table_find("Delete", 6);
  HTTP_NAM_HEAD       = table_find("Head", 4);
  HTTP_NAM_OPTIONS    = table_find("Options", 7);
  HTTP_NAM_NOBODY     = table_find("NoBody", 6);
  HTTP_NAM_BODY_TEXT  = table_find("BodyText", 8);
  HTTP_NAM_BODY_BYTES = table_find("BodyBytes", 9);
  HTTP_NAM_OPTS       = table_find("Opts", 4);
  HTTP_NAM_T          = table_find("T", 1);
  HTTP_NAM_F          = table_find("F", 1);

  prim_http_request_init();
  prim_http_poll_init();
  prim_http_wait_init();
  prim_http_cancel_init();
}
