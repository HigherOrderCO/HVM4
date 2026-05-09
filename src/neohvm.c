// NeoHVM
// ======
// A separate experimental runtime. It does not include or modify hvm.c.

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define MAX_NAMES 16384
#define MAX_BIND  4096
#define MAX_ARGS  4

typedef struct Term Term;
typedef struct Env Env;
typedef struct Val Val;
typedef struct Code Code;
typedef struct Arg Arg;
typedef struct Cases Cases;
typedef struct LamDup LamDup;

enum {
  T_VAR,
  T_DP0,
  T_DP1,
  T_REF,
  T_NUM,
  T_CTR,
  T_LAM,
  T_APP,
  T_MAT,
  T_DUP,
  T_SUP,
  T_ERA
};

enum {
  M_CTR,
  M_NUM
};

enum {
  V_THUNK,
  V_LTHUNK,
  V_NUM,
  V_CTR,
  V_LAM,
  V_ELAM,
  V_SLAM,
  V_MAT,
  V_SUP,
  V_VAR,
  V_APP,
  V_PRJ,
  V_DLAM,
  V_PLAM,
  V_BOX,
  V_ERA
};

enum {
  BC_ARG,
  BC_ARGS,
  BC_VAR,
  BC_DP0,
  BC_DP1,
  BC_REF,
  BC_CALL_REF,
  BC_NUM,
  BC_CTR,
  BC_LAM,
  BC_ELAM,
  BC_SLAM,
  BC_MAT,
  BC_MAT_CTR,
  BC_DUP,
  BC_SUP,
  BC_ERA
};

enum {
  CK_NONE,
  CK_NUM,
  CK_VAR
};

struct Term {
  u8    tag;
  u8    aux;
  u32   ext;
  u32   arity;
  Term *kid[16];
  Code *code;
};

struct Env {
  Val      *val;
  uintptr_t next_span;
};

typedef struct EnvBlock EnvBlock;
typedef struct ValBlock ValBlock;
typedef struct ItemBlock ItemBlock;
typedef struct CodeBlock CodeBlock;
typedef struct LamDupBlock LamDupBlock;

struct EnvBlock {
  EnvBlock *next;
  u32       used;
  Env       item[65536];
};

struct Val {
  u8    tag;
  u8    arity;
  u16   pad;
  u32   ext;
  union {
    Val **item;
    struct {
      Val *fst;
      Val *snd;
    };
    struct {
      Code *code;
      Env  *env;
    };
  };
};

struct Code {
  u8    op;
  u8    aux;
  u8    sup_has;
  u8    pad;
  u32   ext;
  u32   arity;
  u32   sup_lab;
  Term *term;
  Cases *cases;
  Code *sub;
  Code *next;
  void *jump;
};

struct Cases {
  Term *chain;
  Code *ctr;
  Code *num0;
  Code *num1;
  Code *dft;
  u32   ctr_ext;
};

struct Arg {
  Val  *val;
  Code *code;
  Env  *env;
  u32   gap;
};

struct LamDup {
  Val *lam;
  Val *box[2];
  u32  lab;
};

struct ValBlock {
  ValBlock *next;
  u32       used;
  Val       item[65536];
};

struct ItemBlock {
  ItemBlock *next;
  u32        used;
  Val       *item[1 << 20];
};

struct CodeBlock {
  CodeBlock *next;
  u32        used;
  Code       item[65536];
};

struct LamDupBlock {
  LamDupBlock *next;
  u32          used;
  LamDup       item[65536];
};

typedef struct {
  char *src;
  u32   pos;
  u32   len;
  u32   depth;
} Parser;

typedef struct {
  u32 name;
  u32 depth;
  u32 lab;
  u32 uses;
  int side;
  u8  dup;
} Bind;

static char  NAMES[MAX_NAMES][64];
static u32   NAME_LEN = 0;
static Term *DEFS[MAX_NAMES];
static Val  *REF_CACHE[MAX_NAMES];
static Bind  BINDS[MAX_BIND];
static u32   BIND_LEN = 0;
static u64   ITRS = 0;
static u32   FRESH_LAB = 0;
static EnvBlock *ENV_BLOCK = NULL;
static ValBlock *VAL_BLOCK = NULL;
static ItemBlock *ITEM_BLOCK = NULL;
static CodeBlock *CODE_BLOCK = NULL;
static LamDupBlock *LAMDUP_BLOCK = NULL;
static Val *VAL_FREE = NULL;
static Val NUM_CACHE[2] = {
  {.tag = V_NUM, .ext = 0, .arity = 0, .env = NULL, .code = NULL},
  {.tag = V_NUM, .ext = 1, .arity = 0, .env = NULL, .code = NULL},
};

static void die(const char *msg) {
  fprintf(stderr, "neohvm: %s\n", msg);
  exit(1);
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char*)malloc((size_t)len + 1);
  if (!buf) die("out of memory");
  if (fread(buf, 1, (size_t)len, f) != (size_t)len) die("read failed");
  buf[len] = 0;
  fclose(f);
  return buf;
}

static int name_eq(u32 id, const char *s, u32 len) {
  return strlen(NAMES[id]) == len && memcmp(NAMES[id], s, len) == 0;
}

static u32 name_intern(const char *s, u32 len) {
  for (u32 i = 0; i < NAME_LEN; i++) {
    if (name_eq(i, s, len)) return i;
  }
  if (NAME_LEN >= MAX_NAMES) die("name table full");
  if (len >= sizeof(NAMES[0])) die("name too long");
  memcpy(NAMES[NAME_LEN], s, len);
  NAMES[NAME_LEN][len] = 0;
  return NAME_LEN++;
}

static Term *term_new(u8 tag) {
  Term *t = (Term*)calloc(1, sizeof(Term));
  if (!t) die("out of memory");
  return (t->tag = tag), t;
}

ALWAYS_INLINE Val *val_new(u8 tag) {
  Val *v;
  if (__builtin_expect(VAL_FREE != NULL, 1)) {
    v = VAL_FREE;
    VAL_FREE = VAL_FREE->fst;
  } else if (__builtin_expect(VAL_BLOCK == NULL || VAL_BLOCK->used >= 65536, 0)) {
    ValBlock *block = (ValBlock*)malloc(sizeof(ValBlock));
    if (!block) die("out of memory");
    block->next = VAL_BLOCK;
    block->used = 0;
    VAL_BLOCK = block;
    v = &VAL_BLOCK->item[VAL_BLOCK->used++];
  } else {
    v = &VAL_BLOCK->item[VAL_BLOCK->used++];
  }
  v->tag = tag;
  return v;
}

ALWAYS_INLINE void val_free(Val *v) {
  v->fst = VAL_FREE;
  VAL_FREE = v;
}

static u64 val_live_count(void) {
  u64 live = 0;
  for (ValBlock *block = VAL_BLOCK; block != NULL; block = block->next) {
    live += block->used;
  }
  for (Val *v = VAL_FREE; v != NULL; v = v->fst) {
    live--;
  }
  return live;
}

ALWAYS_INLINE Val *share_value(Val *v);

ALWAYS_INLINE void val_free_ctr(Val *v) {
  if (v->tag != V_CTR) return;
  if (__builtin_expect(v->pad == 0, 1)) {
    val_free(v);
    return;
  }
  if (v->arity == 2) {
    share_value(v->fst);
    share_value(v->snd);
  } else if (v->arity == 1) {
    share_value(v->fst);
  } else if (v->arity > 2) {
    for (u32 i = 0; i < v->arity; i++) share_value(v->item[i]);
  }
  v->pad = 0;
}

ALWAYS_INLINE Val *share_value(Val *v) {
  if (v->tag == V_CTR || v->tag == V_LTHUNK || v->tag == V_THUNK) v->pad = 1;
  return v;
}

ALWAYS_INLINE int is_lam(Val *v) {
  return v->tag == V_LAM || v->tag == V_ELAM || v->tag == V_SLAM;
}

static Code *code_new(u8 op) {
  if (CODE_BLOCK == NULL || CODE_BLOCK->used >= 65536) {
    CodeBlock *block = (CodeBlock*)malloc(sizeof(CodeBlock));
    if (!block) die("out of memory");
    block->next = CODE_BLOCK;
    block->used = 0;
    CODE_BLOCK = block;
  }
  Code *c = &CODE_BLOCK->item[CODE_BLOCK->used++];
  c->op = op;
  c->aux = 0;
  c->sup_has = 0;
  c->pad = 0;
  c->ext = 0;
  c->arity = 0;
  c->sup_lab = UINT32_MAX;
  c->term = NULL;
  c->cases = NULL;
  c->sub = NULL;
  c->next = NULL;
  c->jump = NULL;
  return c;
}

static Cases *cases_new(Term *chain) {
  Cases *cases = (Cases*)calloc(1, sizeof(Cases));
  if (!cases) die("out of memory");
  cases->chain = chain;
  cases->ctr_ext = UINT32_MAX;
  return cases;
}

static Val **items_new(u32 len) {
  if (len == 0) return NULL;
  if (len > (1u << 20)) die("constructor too wide");
  if (ITEM_BLOCK == NULL || ITEM_BLOCK->used + len > (1u << 20)) {
    ItemBlock *block = (ItemBlock*)malloc(sizeof(ItemBlock));
    if (!block) die("out of memory");
    block->next = ITEM_BLOCK;
    block->used = 0;
    ITEM_BLOCK = block;
  }
  Val **items = &ITEM_BLOCK->item[ITEM_BLOCK->used];
  ITEM_BLOCK->used += len;
  return items;
}

static LamDup *lamdup_new(void) {
  if (LAMDUP_BLOCK == NULL || LAMDUP_BLOCK->used >= 65536) {
    LamDupBlock *block = (LamDupBlock*)malloc(sizeof(LamDupBlock));
    if (!block) die("out of memory");
    block->next = LAMDUP_BLOCK;
    block->used = 0;
    LAMDUP_BLOCK = block;
  }
  return &LAMDUP_BLOCK->item[LAMDUP_BLOCK->used++];
}

ALWAYS_INLINE Env *env_cell(Val *val, Env *next, u32 span) {
  if (__builtin_expect(ENV_BLOCK == NULL || ENV_BLOCK->used >= 65536, 0)) {
    EnvBlock *block = (EnvBlock*)malloc(sizeof(EnvBlock));
    if (!block) die("out of memory");
    block->next = ENV_BLOCK;
    block->used = 0;
    ENV_BLOCK = block;
  }
  Env *e = &ENV_BLOCK->item[ENV_BLOCK->used++];
  e->val = val;
  e->next_span = ((uintptr_t)next) | (uintptr_t)(span - 1);
  return e;
}

ALWAYS_INLINE Env *env_push(Val *val, Env *next, u32 span) {
  if (span == 0) die("bad environment span");
  if (span <= 16) return env_cell(val, next, span);
  u32 rest = span - 16;
  Env *tail = next;
  while (rest > 0) {
    u32 chunk = rest > 16 ? 16 : rest;
    tail = env_cell(NULL, tail, chunk);
    rest -= chunk;
  }
  return env_cell(val, tail, 16);
}

ALWAYS_INLINE Env *env_next(Env *env) {
  return (Env*)(env->next_span & ~(uintptr_t)15);
}

ALWAYS_INLINE u32 env_span(Env *env) {
  return (u32)(env->next_span & (uintptr_t)15) + 1;
}

ALWAYS_INLINE Env *env_at(Env *env, u32 lvl, u32 gap) {
  if (lvl == 0) die("bad variable level");
  if (lvl <= gap) die("erased variable reached");
  if (gap == 0 && lvl == 2 && env != NULL && env_span(env) == 1) {
    env = env_next(env);
    if (!env) die("unbound variable");
    if (!env->val) die("erased variable reached");
    return env;
  }
  lvl -= gap;
  if (lvl == 1) {
    if (!env) die("unbound variable");
    if (!env->val) die("erased variable reached");
    return env;
  }
  for (;;) {
    if (!env) die("unbound variable");
    u32 span = env_span(env);
    if (lvl <= span) die("erased variable reached");
    lvl -= span;
    env = env_next(env);
    if (lvl == 1) {
      if (!env) die("unbound variable");
      if (!env->val) die("erased variable reached");
      return env;
    }
  }
}

ALWAYS_INLINE Val *env_get(Env *env, u32 lvl, u32 gap) {
  if (gap == 0) {
    if (lvl == 1) {
      if (!env) die("unbound variable");
      if (!env->val) die("erased variable reached");
      return env->val;
    }
    if (lvl == 2 && env != NULL && env_span(env) == 1) {
      env = env_next(env);
      if (!env) die("unbound variable");
      if (!env->val) die("erased variable reached");
      return env->val;
    }
  }
  return env_at(env, lvl, gap)->val;
}

static void skip(Parser *p) {
  for (;;) {
    while (p->pos < p->len) {
      char c = p->src[p->pos];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        p->pos++;
      } else {
        break;
      }
    }
    if (p->pos + 1 < p->len && p->src[p->pos] == '/' && p->src[p->pos + 1] == '/') {
      p->pos += 2;
      while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
      continue;
    }
    return;
  }
}

static int starts(Parser *p, const char *s) {
  skip(p);
  u32 n = (u32)strlen(s);
  return p->pos + n <= p->len && memcmp(p->src + p->pos, s, n) == 0;
}

static int take(Parser *p, const char *s) {
  if (!starts(p, s)) return 0;
  p->pos += (u32)strlen(s);
  return 1;
}

static void need(Parser *p, const char *s) {
  if (!take(p, s)) {
    fprintf(stderr, "neohvm: expected '%s' near byte %u\n", s, p->pos);
    exit(1);
  }
}

static int name_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int name_char(char c) {
  return name_start(c) || (c >= '0' && c <= '9') || c == '$';
}

static u32 parse_name(Parser *p) {
  skip(p);
  if (p->pos >= p->len || !name_start(p->src[p->pos])) die("expected name");
  u32 beg = p->pos++;
  while (p->pos < p->len && name_char(p->src[p->pos])) p->pos++;
  return name_intern(p->src + beg, p->pos - beg);
}

static u32 fresh_label(void) {
  char buf[32];
  snprintf(buf, sizeof(buf), "_L%u", FRESH_LAB++);
  return name_intern(buf, (u32)strlen(buf));
}

static u32 parse_optional_label(Parser *p) {
  skip(p);
  if (p->pos >= p->len) return fresh_label();
  char c = p->src[p->pos];
  if (c == '=' || c == '.' || c == ',' || c == ';' || c == '{') {
    return fresh_label();
  }
  return parse_name(p);
}

static int sep(Parser *p) {
  skip(p);
  if (p->pos < p->len && (p->src[p->pos] == ',' || p->src[p->pos] == ';')) {
    p->pos++;
    return 1;
  }
  return 0;
}

static int sub0(Parser *p) {
  return take(p, "\xE2\x82\x80");
}

static int sub1(Parser *p) {
  return take(p, "\xE2\x82\x81");
}

static void bind_push(u32 name, u32 lab, u8 dup) {
  if (BIND_LEN >= MAX_BIND) die("too many binders");
  u32 depth = BIND_LEN + 1;
  BINDS[BIND_LEN++] = (Bind){name, depth, lab, 0, -1, dup};
}

static void bind_push_side(u32 name, u32 lab, int side) {
  if (BIND_LEN >= MAX_BIND) die("too many binders");
  u32 depth = BIND_LEN + 1;
  BINDS[BIND_LEN++] = (Bind){name, depth, lab, 0, side, 1};
}

static void bind_pop(void) {
  if (BIND_LEN == 0) die("binder underflow");
  BIND_LEN--;
}

static Bind *bind_find(u32 name) {
  for (int i = (int)BIND_LEN - 1; i >= 0; i--) {
    if (BINDS[i].name == name) return &BINDS[i];
  }
  return NULL;
}

static Term *parse_term(Parser *p);

static Term *parse_lam(Parser *p) {
  if (take(p, "{")) {
    skip(p);
    if (take(p, "}")) return term_new(T_ERA);
    Term *head = NULL;
    Term **tail = &head;
    while (!starts(p, "}")) {
      Parser save = *p;
      Term *m = term_new(T_MAT);
      if (take(p, "#")) {
        m->aux = M_CTR;
        m->ext = parse_name(p);
      } else if (isdigit((unsigned char)p->src[p->pos])) {
        m->aux = M_NUM;
        u32 n = 0;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
          n = n * 10 + (u32)(p->src[p->pos++] - '0');
        }
        m->ext = n;
      } else if (take(p, "_")) {
        skip(p);
        if (!take(p, ":")) {
          *p = save;
          *tail = parse_term(p);
          sep(p);
          need(p, "}");
          return head ? head : *tail;
        }
        *tail = parse_term(p);
        sep(p);
        need(p, "}");
        return head ? head : *tail;
      } else {
        *p = save;
        *tail = parse_term(p);
        sep(p);
        need(p, "}");
        return head ? head : *tail;
      }
      skip(p);
      if (!take(p, ":")) {
        *p = save;
        *tail = parse_term(p);
        sep(p);
        need(p, "}");
        return head ? head : *tail;
      }
      m->kid[0] = parse_term(p);
      *tail = m;
      tail = &m->kid[1];
      sep(p);
    }
    need(p, "}");
    return head ? head : term_new(T_ERA);
  }

  take(p, "&");
  u32 name = parse_name(p);
  Term *lam = term_new(T_LAM);
  bind_push(name, 0, 0);
  if (take(p, "&")) {
    u32 lab = parse_optional_label(p);
    Term *val = term_new(T_VAR);
    val->arity = 1;
    val->ext = 0;
    BINDS[BIND_LEN - 1].uses++;
    Term *dup = term_new(T_DUP);
    dup->ext = lab;
    dup->kid[0] = val;
    bind_push(name, lab, 1);
    if (sep(p)) {
      dup->kid[1] = parse_lam(p);
    } else {
      need(p, ".");
      dup->kid[1] = parse_term(p);
    }
    bind_pop();
    lam->kid[0] = dup;
  } else {
    if (sep(p)) {
      lam->kid[0] = parse_lam(p);
    } else {
      need(p, ".");
      lam->kid[0] = parse_term(p);
    }
  }
  lam->arity = BINDS[BIND_LEN - 1].uses;
  if (BINDS[BIND_LEN - 1].uses == 0) {
    lam->aux = 1;
  }
  bind_pop();
  return lam;
}

static Term *parse_dup(Parser *p) {
  if (take(p, "&")) {
    u32 first = starts(p, "{") ? fresh_label() : parse_name(p);
    if (!take(p, "{")) {
      u32 name = first;
      need(p, "&");
      u32 lab = parse_optional_label(p);
      need(p, "=");
      Term *val = parse_term(p);
      sep(p);
      Term *dup = term_new(T_DUP);
      dup->ext = lab;
      dup->kid[0] = val;
      bind_push(name, lab, 1);
      dup->kid[1] = parse_term(p);
      bind_pop();
      return dup;
    }
    u32 lab = first;
    take(p, "&");
    u32 name0 = parse_name(p);
    sep(p);
    take(p, "&");
    u32 name1 = parse_name(p);
    sep(p);
    need(p, "}");
    need(p, "=");
    Term *val = parse_term(p);
    sep(p);
    Term *dup = term_new(T_DUP);
    dup->ext = lab;
    dup->kid[0] = val;
    bind_push_side(name0, lab, 0);
    bind_push_side(name1, lab, 1);
    BINDS[BIND_LEN - 2].depth = BIND_LEN;
    BINDS[BIND_LEN - 1].depth = BIND_LEN;
    dup->kid[1] = parse_term(p);
    bind_pop();
    bind_pop();
    return dup;
  }
  u32 name = parse_name(p);
  need(p, "&");
  u32 lab = parse_optional_label(p);
  need(p, "=");
  Term *val = parse_term(p);
  sep(p);
  Term *dup = term_new(T_DUP);
  dup->ext = lab;
  dup->kid[0] = val;
  bind_push(name, lab, 1);
  dup->kid[1] = parse_term(p);
  bind_pop();
  return dup;
}

static Term *parse_ctr(Parser *p) {
  Term *ctr = term_new(T_CTR);
  ctr->ext = parse_name(p);
  if (take(p, "{")) {
    while (!starts(p, "}")) {
      if (ctr->arity >= 16) die("constructor arity > 16");
      ctr->kid[ctr->arity++] = parse_term(p);
      sep(p);
    }
    need(p, "}");
  }
  return ctr;
}

static Term *parse_ref(Parser *p) {
  Term *r = term_new(T_REF);
  r->ext = parse_name(p);
  return r;
}

static Term *parse_num(Parser *p) {
  Term *n = term_new(T_NUM);
  while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
    n->ext = n->ext * 10 + (u32)(p->src[p->pos++] - '0');
  }
  return n;
}

static Term *parse_var(Parser *p) {
  u32 name = parse_name(p);
  int side = -1;
  if (sub0(p)) side = 0;
  else if (sub1(p)) side = 1;
  Bind *b = bind_find(name);
  if (!b) {
    fprintf(stderr, "neohvm: unbound variable '%s'\n", NAMES[name]);
    exit(1);
  }
  Term *v = term_new(side == 0 ? T_DP0 : side == 1 ? T_DP1 : T_VAR);
  if (side < 0 && b->side >= 0) {
    v->tag = b->side == 0 ? T_DP0 : T_DP1;
  }
  v->ext = b->lab;
  v->arity = BIND_LEN - b->depth + 1;
  b->uses++;
  return v;
}

static Term *parse_atom(Parser *p) {
  skip(p);
  if (take(p, "\xCE\xBB")) return parse_lam(p);
  if (take(p, "!")) return parse_dup(p);
  if (take(p, "&")) {
    Term *s = term_new(T_SUP);
    s->ext = parse_name(p);
    need(p, "{");
    s->kid[0] = parse_term(p);
    sep(p);
    s->kid[1] = parse_term(p);
    sep(p);
    need(p, "}");
    return s;
  }
  if (take(p, "#")) return parse_ctr(p);
  if (take(p, "@")) return parse_ref(p);
  if (take(p, "(")) {
    Term *t = parse_term(p);
    need(p, ")");
    return t;
  }
  if (isdigit((unsigned char)p->src[p->pos])) return parse_num(p);
  return parse_var(p);
}

static Term *parse_term(Parser *p) {
  Term *t = parse_atom(p);
  for (;;) {
    skip(p);
    if (!take(p, "(")) return t;
    if (take(p, ")")) continue;
    while (1) {
      Term *app = term_new(T_APP);
      app->kid[0] = t;
      app->kid[1] = parse_term(p);
      t = app;
      sep(p);
      if (take(p, ")")) break;
    }
  }
}

static void parse_program(char *src) {
  Parser p = {.src = src, .len = (u32)strlen(src)};
  while (1) {
    skip(&p);
    if (p.pos >= p.len) break;
    need(&p, "@");
    u32 name = parse_name(&p);
    need(&p, "=");
    BIND_LEN = 0;
    DEFS[name] = parse_term(&p);
  }
}

static Code *compile_term(Term *term);
static Code *compile_term_ctx(Term *term, int tail);
static Code *compile_app(Term *term, int tail);
static u8 compile_ctr_field_kind(Code *code);
static Val *eval_code_into(Code *pc, Env *env, u32 gap, Arg *args, u32 argc, Val *dst);
static inline Val *eval_code(Code *pc, Env *env, u32 gap, Arg *args, u32 argc) {
  return eval_code_into(pc, env, gap, args, argc, NULL);
}
ALWAYS_INLINE Val *force(Val *v);
ALWAYS_INLINE Val *mk_lam_tag(u8 tag, Code *code, Env *env, u32 gap);
static inline Val *mk_lam(Code *code, Env *env, u32 gap);
static inline Val *mk_mat(Code *code, Env *env, u32 gap);
static void link_refs_code(Code *code, u32 depth);
static void thread_code(Code *code, void **dispatch, u32 depth);

static Code *compile_term(Term *term) {
  return compile_term_ctx(term, 0);
}

static Code *compile_term_ctx(Term *term, int tail) {
  if (term->code) return term->code;
  Code *c = NULL;
  switch (term->tag) {
    case T_APP:
      c = compile_app(term, tail);
      break;
    case T_VAR:
      c = code_new(BC_VAR);
      c->ext = term->arity;
      break;
    case T_DP0:
      c = code_new(BC_DP0);
      c->ext = term->arity;
      c->arity = term->ext;
      break;
    case T_DP1:
      c = code_new(BC_DP1);
      c->ext = term->arity;
      c->arity = term->ext;
      break;
    case T_REF:
      c = code_new(BC_REF);
      c->ext = term->ext;
      break;
    case T_NUM:
      c = code_new(BC_NUM);
      c->ext = term->ext;
      break;
    case T_CTR:
      c = code_new(BC_CTR);
      c->ext = term->ext;
      c->arity = term->arity;
      c->term = term;
      if (term->arity > 0) c->sub = compile_term_ctx(term->kid[0], 0);
      if (term->arity > 1) c->next = compile_term_ctx(term->kid[1], 0);
      for (u32 i = 2; i < term->arity; i++) compile_term_ctx(term->kid[i], 0);
      if (term->arity == 2) {
        c->aux = compile_ctr_field_kind(c->sub) | (compile_ctr_field_kind(c->next) << 2);
      }
      break;
    case T_LAM:
      c = code_new(term->aux ? BC_ELAM : term->arity > 1 ? BC_SLAM : BC_LAM);
      c->aux = term->aux;
      c->arity = term->arity;
      c->sub = compile_term_ctx(term->kid[0], 1);
      break;
    case T_MAT:
      c = code_new(BC_MAT);
      c->term = term;
      c->cases = cases_new(term);
      Term *m = term;
      for (; m && m->tag == T_MAT; m = m->kid[1]) {
        compile_term_ctx(m->kid[0], 1);
        if (m->aux == M_NUM && m->ext == 0) {
          c->cases->num0 = m->kid[0]->code;
        } else if (m->aux == M_NUM && m->ext == 1) {
          c->cases->num1 = m->kid[0]->code;
        } else if (m->aux == M_CTR && c->cases->ctr == NULL) {
          c->cases->ctr_ext = m->ext;
          c->cases->ctr = m->kid[0]->code;
        }
      }
      if (m != NULL) {
        c->cases->dft = compile_term_ctx(m, 1);
      }
      if (term->aux == M_CTR && term->kid[1] == NULL) {
        c->op = BC_MAT_CTR;
        c->ext = term->ext;
        c->sub = term->kid[0]->code;
      }
      break;
    case T_DUP:
      c = code_new(BC_DUP);
      c->ext = term->ext;
      c->sub = compile_term_ctx(term->kid[0], 0);
      c->next = compile_term_ctx(term->kid[1], 1);
      break;
    case T_SUP:
      c = code_new(BC_SUP);
      c->ext = term->ext;
      c->term = term;
      compile_term_ctx(term->kid[0], 0);
      compile_term_ctx(term->kid[1], 0);
      break;
    case T_ERA:
      c = code_new(BC_ERA);
      break;
    default:
      die("bad term during compile");
  }
  term->code = c;
  return c;
}

static u8 compile_ctr_field_kind(Code *code) {
  if (code->op == BC_NUM) return CK_NUM;
  if (code->op == BC_VAR) return CK_VAR;
  return CK_NONE;
}

static Code *compile_app(Term *term, int tail) {
  Term *args[MAX_ARGS];
  u32 argc = 0;
  Term *fun = term;
  while (fun->tag == T_APP && argc < MAX_ARGS) {
    args[argc++] = fun->kid[1];
    fun = fun->kid[0];
  }
  if (argc == 1) {
    if (fun->tag == T_REF) {
      Code *c = code_new(BC_CALL_REF);
      c->ext = fun->ext;
      c->arity = 1;
      c->sub = compile_term_ctx(args[0], 0);
      return c;
    }
    Code *c = code_new(BC_ARG);
    c->sub = compile_term_ctx(args[0], 0);
    c->next = compile_term_ctx(fun, 0);
    return c;
  }
  Code *c = code_new(BC_ARGS);
  c->arity = argc;
  c->term = term_new(T_APP);
  for (u32 i = 0; i < argc; i++) {
    c->term->kid[i] = args[i];
    compile_term_ctx(args[i], 0);
  }
  c->next = compile_term_ctx(fun, 0);
  return c;
}

static void compile_program_terms(void) {
  for (u32 i = 0; i < NAME_LEN; i++) {
    if (DEFS[i]) compile_term_ctx(DEFS[i], 1);
  }
  for (u32 i = 0; i < NAME_LEN; i++) {
    if (DEFS[i]) link_refs_code(DEFS[i]->code, 0);
  }
  for (u32 i = 0; i < NAME_LEN; i++) {
    if (!DEFS[i]) continue;
    u8 op = DEFS[i]->code->op;
    if (op == BC_MAT || op == BC_MAT_CTR) {
      REF_CACHE[i] = mk_mat(DEFS[i]->code, NULL, 0);
    } else if (op == BC_LAM || op == BC_ELAM || op == BC_SLAM) {
      u8 tag = op == BC_ELAM ? V_ELAM : op == BC_SLAM ? V_SLAM : V_LAM;
      REF_CACHE[i] = mk_lam_tag(tag, DEFS[i]->code->sub, NULL, 0);
    }
  }
}

static void link_refs_code(Code *code, u32 depth) {
  if (code == NULL || depth > 64) return;
  switch (code->op) {
    case BC_ARGS:
      link_refs_code(code->next, depth + 1);
      for (u32 i = 0; i < code->arity; i++) {
        link_refs_code(code->term->kid[i]->code, depth + 1);
      }
      return;
    case BC_ARG:
    case BC_DUP:
      link_refs_code(code->sub, depth + 1);
      link_refs_code(code->next, depth + 1);
      return;
    case BC_CTR:
      for (u32 i = 0; i < code->arity; i++) {
        link_refs_code(code->term->kid[i]->code, depth + 1);
      }
      return;
    case BC_LAM:
    case BC_ELAM:
    case BC_SLAM:
      link_refs_code(code->sub, depth + 1);
      return;
    case BC_MAT:
    case BC_MAT_CTR:
      if (code->cases != NULL) {
        link_refs_code(code->cases->ctr, depth + 1);
        link_refs_code(code->cases->num0, depth + 1);
        link_refs_code(code->cases->num1, depth + 1);
        link_refs_code(code->cases->dft, depth + 1);
      }
      return;
    case BC_REF:
      code->sub = code->ext < MAX_NAMES && DEFS[code->ext] != NULL ? DEFS[code->ext]->code : NULL;
      return;
    case BC_CALL_REF:
      link_refs_code(code->sub, depth + 1);
      code->next = code->ext < MAX_NAMES && DEFS[code->ext] != NULL ? DEFS[code->ext]->code : NULL;
      return;
    case BC_SUP:
      link_refs_code(code->term->kid[0]->code, depth + 1);
      link_refs_code(code->term->kid[1]->code, depth + 1);
      return;
    default:
      return;
  }
}

static void thread_code(Code *code, void **dispatch, u32 depth) {
  if (code == NULL || depth > 64 || code->jump != NULL) return;
  code->jump = dispatch[code->op];
  switch (code->op) {
    case BC_ARGS:
      thread_code(code->next, dispatch, depth + 1);
      for (u32 i = 0; i < code->arity; i++) {
        thread_code(code->term->kid[i]->code, dispatch, depth + 1);
      }
      return;
    case BC_ARG:
    case BC_DUP:
      thread_code(code->sub, dispatch, depth + 1);
      thread_code(code->next, dispatch, depth + 1);
      return;
    case BC_CALL_REF:
      thread_code(code->sub, dispatch, depth + 1);
      thread_code(code->next, dispatch, depth + 1);
      return;
    case BC_CTR:
      for (u32 i = 0; i < code->arity; i++) {
        thread_code(code->term->kid[i]->code, dispatch, depth + 1);
      }
      return;
    case BC_LAM:
    case BC_ELAM:
    case BC_SLAM:
      thread_code(code->sub, dispatch, depth + 1);
      return;
    case BC_MAT:
    case BC_MAT_CTR:
      if (code->cases != NULL) {
        thread_code(code->cases->ctr, dispatch, depth + 1);
        thread_code(code->cases->num0, dispatch, depth + 1);
        thread_code(code->cases->num1, dispatch, depth + 1);
        thread_code(code->cases->dft, dispatch, depth + 1);
      }
      return;
    case BC_SUP:
      thread_code(code->term->kid[0]->code, dispatch, depth + 1);
      thread_code(code->term->kid[1]->code, dispatch, depth + 1);
      return;
    default:
      return;
  }
}

static int code_has_sup_label(Code *code, u32 lab, u32 depth) {
  if (code == NULL || depth > 64) return 0;
  if (code->sup_lab == lab) return code->sup_has;
  int has = 0;
  switch (code->op) {
    case BC_ARGS:
      has = code_has_sup_label(code->next, lab, depth + 1);
      for (u32 i = 0; !has && i < code->arity; i++) {
        has = code_has_sup_label(code->term->kid[i]->code, lab, depth + 1);
      }
      break;
    case BC_ARG:
    case BC_DUP:
      has = code_has_sup_label(code->sub, lab, depth + 1)
          || code_has_sup_label(code->next, lab, depth + 1);
      break;
    case BC_CTR:
      for (u32 i = 0; i < code->arity; i++) {
        if (code_has_sup_label(code->term->kid[i]->code, lab, depth + 1)) {
          has = 1;
          break;
        }
      }
      break;
    case BC_LAM:
    case BC_ELAM:
    case BC_SLAM:
      has = code_has_sup_label(code->sub, lab, depth + 1);
      break;
    case BC_MAT:
    case BC_MAT_CTR:
      if (code->cases != NULL) {
        has = code_has_sup_label(code->cases->ctr, lab, depth + 1)
           || code_has_sup_label(code->cases->num0, lab, depth + 1)
           || code_has_sup_label(code->cases->num1, lab, depth + 1)
           || code_has_sup_label(code->cases->dft, lab, depth + 1);
      }
      break;
    case BC_REF:
      has = code->ext < MAX_NAMES && DEFS[code->ext] != NULL
        ? code_has_sup_label(DEFS[code->ext]->code, lab, depth + 1)
        : 0;
      break;
    case BC_CALL_REF:
      has = code_has_sup_label(code->sub, lab, depth + 1)
         || (code->ext < MAX_NAMES && DEFS[code->ext] != NULL
          ? code_has_sup_label(DEFS[code->ext]->code, lab, depth + 1)
          : 0);
      break;
    case BC_SUP:
      has = code->ext == lab
          || code_has_sup_label(code->term->kid[0]->code, lab, depth + 1)
          || code_has_sup_label(code->term->kid[1]->code, lab, depth + 1);
      break;
    default:
      break;
  }
  code->sup_lab = lab;
  code->sup_has = (u8)has;
  return has;
}

ALWAYS_INLINE Val *mk_thunk(Code *code, Env *env, u32 gap) {
  Val *v = val_new(V_THUNK);
  v->code = code;
  v->env = env;
  v->ext = gap;
  v->pad = 0;
  return v;
}

ALWAYS_INLINE Val *mk_lthunk(Code *code, Env *env, u32 gap) {
  Val *v = val_new(V_LTHUNK);
  v->code = code;
  v->env = env;
  v->ext = gap;
  v->pad = 0;
  return v;
}

ALWAYS_INLINE Val *mk_num(u32 n) {
  if (n < 2) return &NUM_CACHE[n];
  Val *v = val_new(V_NUM);
  v->ext = n;
  return v;
}

ALWAYS_INLINE Val *mk_lam_tag(u8 tag, Code *code, Env *env, u32 gap) {
  Val *v = val_new(tag);
  v->code = code;
  v->env = env;
  v->ext = gap;
  v->arity = 0;
  v->pad = 0;
  return v;
}

ALWAYS_INLINE Val *mk_lam(Code *code, Env *env, u32 gap) {
  return mk_lam_tag(V_LAM, code, env, gap);
}

ALWAYS_INLINE Val *mk_mat(Code *code, Env *env, u32 gap) {
  Val *v = val_new(V_MAT);
  v->code = code;
  v->env = env;
  v->ext = gap;
  return v;
}

ALWAYS_INLINE Val *mk_sup(u32 lab, Val *a, Val *b) {
  Val *v = val_new(V_SUP);
  v->ext = lab;
  v->fst = a;
  v->snd = b;
  return v;
}

static inline Val *mk_var(u32 idx) {
  Val *v = val_new(V_VAR);
  v->ext = idx;
  return v;
}

static inline Val *mk_app(Val *fun, Val *arg) {
  Val *v = val_new(V_APP);
  v->fst = fun;
  v->snd = arg;
  return v;
}

static inline Val *mk_prj(Val *fun, u32 lab, u8 side) {
  Val *v = val_new(V_PRJ);
  v->ext = lab;
  v->arity = side;
  v->fst = fun;
  return v;
}

static inline Val *mk_box(void) {
  Val *v = val_new(V_BOX);
  v->fst = NULL;
  return v;
}

static Val *mk_dlam(Val *lam, u32 lab) {
  LamDup *dup = lamdup_new();
  dup->lam = lam;
  dup->lab = lab;
  dup->box[0] = mk_box();
  dup->box[1] = mk_box();
  Val *v = val_new(V_DLAM);
  v->ext = lab;
  v->fst = lam;
  v->snd = (Val*)dup;
  return v;
}

static inline Val *mk_plam(Val *lam, u32 lab, u8 side) {
  Val *v = val_new(V_PLAM);
  v->ext = lab;
  v->arity = side;
  v->fst = lam;
  return v;
}

ALWAYS_INLINE Val *force_fun_arg(Val *v) {
  if (v->tag != V_THUNK && v->tag != V_LTHUNK) return v;
  switch (v->code->op) {
    case BC_VAR:
    case BC_DP0:
    case BC_DP1:
    case BC_LAM:
    case BC_ELAM:
    case BC_SLAM:
    case BC_MAT:
    case BC_MAT_CTR:
      return force(v);
    case BC_REF:
    case BC_CALL_REF:
    case BC_ARG:
    case BC_ARGS:
    case BC_DUP:
      return force(v);
    default:
      return v;
  }
}

ALWAYS_INLINE Arg arg_code(Code *code, Env *env, u32 gap) {
  return (Arg){.val = NULL, .code = code, .env = env, .gap = gap};
}

ALWAYS_INLINE Arg arg_val(Val *val) {
  return (Arg){.val = val, .code = NULL, .env = NULL, .gap = 0};
}

static inline Val *make_ctr(Code *pc, Env *env, u32 gap);

ALWAYS_INLINE Val *force_arg(Arg *arg) {
  if (arg->val != NULL) return force(arg->val);
  Arg none[MAX_ARGS];
  return eval_code(arg->code, arg->env, arg->gap, none, 0);
}

ALWAYS_INLINE Val *bind_arg(Arg *arg) {
  if (arg->val != NULL) return force_fun_arg(arg->val);
  switch (arg->code->op) {
    case BC_NUM:
      return mk_num(arg->code->ext);
    case BC_VAR:
      return env_get(arg->env, arg->code->ext, arg->gap);
    case BC_CTR:
      return mk_lthunk(arg->code, arg->env, arg->gap);
    case BC_REF:
      if (arg->code->ext < MAX_NAMES && REF_CACHE[arg->code->ext] != NULL) return REF_CACHE[arg->code->ext];
      return force_arg(arg);
    case BC_SUP:
    case BC_ERA:
      return mk_lthunk(arg->code, arg->env, arg->gap);
    default:
      return force_arg(arg);
  }
}

ALWAYS_INLINE Val *project(Val *v, u32 lab, u8 side) {
  v = force(v);
  if (v->tag == V_SUP) {
    ITRS++;
    if (v->ext == lab) return side == 0 ? v->fst : v->snd;
    return mk_sup(v->ext, project(v->fst, lab, side), project(v->snd, lab, side));
  }
  if (v->tag == V_CTR) {
    ITRS++;
    Val *ctr = val_new(V_CTR);
    ctr->ext = v->ext;
    ctr->arity = v->arity;
    ctr->pad = 0;
    if (v->arity == 2) {
      ctr->fst = project(v->fst, lab, side);
      ctr->snd = project(v->snd, lab, side);
    } else if (v->arity == 1) {
      ctr->fst = project(v->fst, lab, side);
    } else if (v->arity > 2) {
      ctr->item = items_new(v->arity);
      for (u32 i = 0; i < v->arity; i++) {
        ctr->item[i] = project(v->item[i], lab, side);
      }
    }
    return ctr;
  }
  if (v->tag == V_MAT) {
    ITRS++;
    return mk_prj(v, lab, side);
  }
  if (v->tag == V_DLAM) {
    ITRS++;
    return mk_plam(v, lab, side);
  }
  if (is_lam(v)) {
    if (v->code->sup_lab == lab) {
      if (!v->code->sup_has) return v;
      ITRS++;
      return mk_plam(mk_dlam(v, lab), lab, side);
    }
    if (code_has_sup_label(v->code, lab, 0)) {
      ITRS++;
      return mk_plam(mk_dlam(v, lab), lab, side);
    }
    return v;
  }
  return v;
}

ALWAYS_INLINE int mat_hits(Term *m, Val *arg) {
  return (m->aux == M_NUM && arg->tag == V_NUM && m->ext == arg->ext)
      || (m->aux == M_CTR && arg->tag == V_CTR && m->ext == arg->ext);
}

ALWAYS_INLINE Val *ctr_get(Val *ctr, u32 idx) {
  if (idx == 0 && ctr->arity <= 2) return ctr->fst;
  if (idx == 1 && ctr->arity <= 2) return ctr->snd;
  return ctr->item[idx];
}

static inline Term *mat_find(Term *mat, Val *arg) {
  if (mat_hits(mat, arg)) return mat;
  Term *m = mat->kid[1];
  if (m == NULL || m->tag != T_MAT) return NULL;
  if (mat_hits(m, arg)) return m;
  for (m = m->kid[1]; m != NULL && m->tag == T_MAT; m = m->kid[1]) {
    if (m->aux == M_NUM && arg->tag == V_NUM && m->ext == arg->ext) return m;
    if (m->aux == M_CTR && arg->tag == V_CTR && m->ext == arg->ext) return m;
  }
  return NULL;
}

ALWAYS_INLINE Code *mat_pick(Code *mat, Val *arg) {
  Cases *cases = mat->cases;
  if (arg->tag == V_NUM) {
    if (arg->ext == 0 && cases->num0) return cases->num0;
    if (arg->ext == 1 && cases->num1) return cases->num1;
  } else if (arg->tag == V_CTR && cases->ctr && arg->ext == cases->ctr_ext) {
    return cases->ctr;
  }
  Term *m = mat_find(cases->chain, arg);
  return m ? m->kid[0]->code : NULL;
}

ALWAYS_INLINE Code *mat_pick_code(Code *mat, Code *arg) {
  if (arg->op == BC_NUM) {
    if (arg->ext == 0 && mat->cases->num0) return mat->cases->num0;
    if (arg->ext == 1 && mat->cases->num1) return mat->cases->num1;
  } else if (arg->op == BC_CTR) {
    if (mat->cases->ctr && arg->ext == mat->cases->ctr_ext) return mat->cases->ctr;
  } else {
    return NULL;
  }
  for (Term *m = mat->cases->chain; m != NULL && m->tag == T_MAT; m = m->kid[1]) {
    if (m->aux == M_NUM && arg->op == BC_NUM && m->ext == arg->ext) return m->kid[0]->code;
    if (m->aux == M_CTR && arg->op == BC_CTR && m->ext == arg->ext) return m->kid[0]->code;
  }
  return NULL;
}

ALWAYS_INLINE void push_ctr_code_args(Code *ctr, Env *env, u32 gap, Arg *args, u32 *argc) {
  if (ctr->arity == 2) {
    if (*argc + 2 > MAX_ARGS) die("argument stack overflow");
    args[(*argc)++] = arg_code(ctr->next, env, gap);
    args[(*argc)++] = arg_code(ctr->sub, env, gap);
    return;
  }
  if (ctr->arity == 1) {
    if (*argc >= MAX_ARGS) die("argument stack overflow");
    args[(*argc)++] = arg_code(ctr->sub, env, gap);
    return;
  }
  for (u32 i = ctr->arity; i > 0; i--) {
    if (*argc >= MAX_ARGS) die("argument stack overflow");
    args[(*argc)++] = arg_code(ctr->term->kid[i - 1]->code, env, gap);
  }
}

ALWAYS_INLINE void push_ctr_val_args(Val *ctr, Arg *args, u32 *argc) {
  if (ctr->arity == 2) {
    if (*argc + 2 > MAX_ARGS) die("argument stack overflow");
    args[(*argc)++] = arg_val(ctr->snd);
    args[(*argc)++] = arg_val(ctr->fst);
    return;
  }
  if (ctr->arity == 1) {
    if (*argc >= MAX_ARGS) die("argument stack overflow");
    args[(*argc)++] = arg_val(ctr->fst);
    return;
  }
  for (u32 i = ctr->arity; i > 0; i--) {
    if (*argc >= MAX_ARGS) die("argument stack overflow");
    args[(*argc)++] = arg_val(ctr_get(ctr, i - 1));
  }
}

ALWAYS_INLINE int is_lam_code(Code *code) {
  return code->op == BC_LAM || code->op == BC_ELAM || code->op == BC_SLAM;
}

ALWAYS_INLINE Code *enter_one_lam(Code *lam, Arg *arg, Env **env, u32 *gap) {
  if (lam->op == BC_ELAM) {
    (*gap)++;
    return lam->sub;
  }
  if (lam->op == BC_SLAM) {
    *env = env_push(share_value(bind_arg(arg)), *env, *gap + 1);
  } else {
    *env = env_push(bind_arg(arg), *env, *gap + 1);
  }
  *gap = 0;
  return lam->sub;
}

ALWAYS_INLINE int enter_ctr2_code_lams(Code **body, Code *ctr, Env *ctr_env, u32 ctr_gap, Env **env, u32 *gap) {
  if (ctr->arity != 2 || !is_lam_code(*body) || !is_lam_code((*body)->sub)) return 0;
  ITRS += 2;
  Arg fst = arg_code(ctr->sub, ctr_env, ctr_gap);
  Code *next = enter_one_lam(*body, &fst, env, gap);
  Arg snd = arg_code(ctr->next, ctr_env, ctr_gap);
  *body = enter_one_lam(next, &snd, env, gap);
  return 1;
}

ALWAYS_INLINE int enter_ctr2_val_lams(Code **body, Val *ctr, Env **env, u32 *gap) {
  if (ctr->arity != 2 || !is_lam_code(*body) || !is_lam_code((*body)->sub)) return 0;
  ITRS += 2;
  Arg fst = arg_val(ctr->fst);
  Code *next = enter_one_lam(*body, &fst, env, gap);
  Arg snd = arg_val(ctr->snd);
  *body = enter_one_lam(next, &snd, env, gap);
  return 1;
}

ALWAYS_INLINE int enter_ctr2_code_num_mat(Code **body, Code *ctr, Env *ctr_env, u32 ctr_gap, Arg *args, u32 *argc) {
  if (ctr->arity != 2 || (*body)->op != BC_MAT || ctr->sub->op != BC_NUM) return 0;
  Code *next = mat_pick_code(*body, ctr->sub);
  if (next == NULL) return 0;
  ITRS++;
  if (*argc >= MAX_ARGS) die("argument stack overflow");
  args[(*argc)++] = arg_code(ctr->next, ctr_env, ctr_gap);
  *body = next;
  return 1;
}

ALWAYS_INLINE int enter_ctr2_val_num_mat(Code **body, Val *ctr, Arg *args, u32 *argc) {
  if (ctr->arity != 2 || (*body)->op != BC_MAT || ctr->fst->tag != V_NUM) return 0;
  Code *next = mat_pick(*body, ctr->fst);
  if (next == NULL) return 0;
  ITRS++;
  if (*argc >= MAX_ARGS) die("argument stack overflow");
  args[(*argc)++] = arg_val(ctr->snd);
  *body = next;
  return 1;
}

static Val *apply_fun(Val *fun, Arg *arg);
static Val *apply_sup(Val *sup, Arg *arg);

ALWAYS_INLINE Code *matchable_val_code(Val *v, Env **env, u32 *gap) {
  if ((v->tag == V_THUNK || v->tag == V_LTHUNK)
  &&  (v->code->op == BC_CTR || v->code->op == BC_NUM)) {
    *env = v->env;
    *gap = v->ext;
    return v->code;
  }
  return NULL;
}

ALWAYS_INLINE Code *matchable_arg_code(Arg *arg, Env **env, u32 *gap, Val **seen) {
  *seen = NULL;
  if (arg->val != NULL) {
    *seen = arg->val;
    return matchable_val_code(arg->val, env, gap);
  }
  if (arg->code->op == BC_CTR || arg->code->op == BC_NUM) {
    *env = arg->env;
    *gap = arg->gap;
    return arg->code;
  }
  if (arg->code->op == BC_VAR) {
    Val *val = env_get(arg->env, arg->code->ext, arg->gap);
    *seen = val;
    return matchable_val_code(val, env, gap);
  }
  return NULL;
}

ALWAYS_INLINE void consume_matchable_seen(Val *seen) {
  if (seen->tag == V_THUNK || seen->tag == V_LTHUNK) {
    if (seen->pad != 0) {
      seen->pad = 0;
    } else {
      val_free(seen);
    }
  }
}

static Val *apply_default(Code *dft, Env *env, u32 gap, Val *arg) {
  if (dft == NULL) return mk_num(0);
  Arg none[MAX_ARGS];
  Val *fun = eval_code(dft, env, gap, none, 0);
  switch (fun->tag) {
    case V_LAM:
    case V_ELAM:
    case V_SLAM:
    case V_MAT:
    case V_SUP:
    case V_VAR:
    case V_APP: {
      Arg one = arg_val(arg);
      return apply_fun(fun, &one);
    }
    default:
      return fun;
  }
}

static inline Val *apply_lam(Val *lam, Arg *arg) {
  ITRS++;
  Env *env = lam->env;
  u32 gap = lam->ext;
  if (lam->tag == V_ELAM) {
    gap++;
  } else if (lam->tag == V_SLAM) {
    env = env_push(share_value(bind_arg(arg)), env, gap + 1);
    gap = 0;
  } else {
    env = env_push(bind_arg(arg), env, gap + 1);
    gap = 0;
  }
  Arg none[MAX_ARGS];
  return eval_code(lam->code, env, gap, none, 0);
}

static Val *apply_plam(Val *plam, Arg *arg) {
  ITRS++;
  Val *src = force(plam->fst);
  LamDup *dup = NULL;
  Val *lam = src;
  if (src->tag == V_DLAM) {
    dup = (LamDup*)src->snd;
    lam = dup->lam;
  }
  lam = force(lam);
  if (!is_lam(lam)) die("projected non-lambda");
  Env *env = lam->env;
  u32 gap = lam->ext;
  if (lam->tag == V_ELAM) {
    gap++;
  } else {
    Val *got = lam->tag == V_SLAM ? share_value(bind_arg(arg)) : bind_arg(arg);
    Val *var = dup ? dup->box[1 - plam->arity] : mk_var(0);
    if (dup) dup->box[plam->arity]->fst = got;
    Val *sup = plam->arity == 0 ? mk_sup(plam->ext, got, var) : mk_sup(plam->ext, var, got);
    env = env_push(sup, env, gap + 1);
    gap = 0;
  }
  Arg none[MAX_ARGS];
  Val *body = eval_code(lam->code, env, gap, none, 0);
  return project(body, plam->ext, plam->arity);
}

static inline Val *apply_mat(Code *mat, Env *env, u32 gap, Val *arg) {
  if (arg->tag == V_SUP) {
    Arg one[1];
    one[0] = arg_val(arg->fst);
    Val *fst = eval_code(mat, env, gap, one, 1);
    one[0] = arg_val(arg->snd);
    Val *snd = eval_code(mat, env, gap, one, 1);
    return mk_sup(arg->ext, fst, snd);
  }
  Code *body = mat_pick(mat, arg);
  if (!body) return apply_default(mat->cases->dft, env, gap, arg);
  Arg args[MAX_ARGS];
  u32 argc = 0;
  if (arg->tag == V_CTR) {
    if (!enter_ctr2_val_num_mat(&body, arg, args, &argc)
    &&  !enter_ctr2_val_lams(&body, arg, &env, &gap)) {
      push_ctr_val_args(arg, args, &argc);
    }
    val_free_ctr(arg);
  }
  return eval_code(body, env, gap, args, argc);
}

ALWAYS_INLINE Val *apply_fun(Val *fun, Arg *arg) {
  fun = force(fun);
  switch (fun->tag) {
    case V_LAM:
    case V_ELAM:
    case V_SLAM:
      return apply_lam(fun, arg);
    case V_MAT: {
      Env *arg_env;
      u32 arg_gap;
      Val *seen;
      Code *arg_code = matchable_arg_code(arg, &arg_env, &arg_gap, &seen);
      if (arg_code != NULL) {
        Code *body = mat_pick_code(fun->code, arg_code);
        if (body) {
          ITRS++;
          if (seen) consume_matchable_seen(seen);
          Arg args[MAX_ARGS];
          u32 argc = 0;
          Env *body_env = fun->env;
          u32 body_gap = fun->ext;
          if (arg_code->op == BC_CTR) {
            if (!enter_ctr2_code_num_mat(&body, arg_code, arg_env, arg_gap, args, &argc)
            &&  !enter_ctr2_code_lams(&body, arg_code, arg_env, arg_gap, &body_env, &body_gap)) {
              push_ctr_code_args(arg_code, arg_env, arg_gap, args, &argc);
            }
          }
          return eval_code(body, body_env, body_gap, args, argc);
        }
      }
      ITRS++;
      Val *val = seen ? force(seen) : force_arg(arg);
      return apply_mat(fun->code, fun->env, fun->ext, val);
    }
    case V_SUP: {
      Arg shared;
      if (arg->val == NULL) {
        shared = arg_val(mk_thunk(arg->code, arg->env, arg->gap));
        arg = &shared;
      }
      Val *fst = apply_fun(fun->fst, arg);
      Val *snd = apply_fun(fun->snd, arg);
      return mk_sup(fun->ext, fst, snd);
    }
    case V_VAR:
    case V_APP:
      return mk_app(fun, arg->val != NULL ? arg->val : mk_thunk(arg->code, arg->env, arg->gap));
    case V_PRJ: {
      Val *res = apply_fun(fun->fst, arg);
      return project(res, fun->ext, fun->arity);
    }
    case V_PLAM:
      return apply_plam(fun, arg);
    case V_BOX:
      if (fun->fst) return apply_fun(fun->fst, arg);
      return mk_app(fun, arg->val != NULL ? arg->val : mk_thunk(arg->code, arg->env, arg->gap));
    default:
      die("cannot apply value");
  }
  return fun;
}

static __attribute__((noinline)) Val *apply_sup(Val *sup, Arg *arg) {
  if ((sup->fst->tag == V_THUNK || sup->fst->tag == V_LTHUNK)
  &&  (sup->snd->tag == V_THUNK || sup->snd->tag == V_LTHUNK)) {
    if (arg->val == NULL) *arg = arg_val(mk_thunk(arg->code, arg->env, arg->gap));
    Arg one[1] = {*arg};
    Val *fst = eval_code(sup->fst->code, sup->fst->env, sup->fst->ext, one, 1);
    one[0] = *arg;
    Val *snd = eval_code(sup->snd->code, sup->snd->env, sup->snd->ext, one, 1);
    return mk_sup(sup->ext, fst, snd);
  }
  if (is_lam(sup->fst) && is_lam(sup->snd)) {
    if (arg->val == NULL) *arg = arg_val(mk_thunk(arg->code, arg->env, arg->gap));
    Val *fst = apply_lam(sup->fst, arg);
    Val *snd = apply_lam(sup->snd, arg);
    return mk_sup(sup->ext, fst, snd);
  }
  return apply_fun(sup, arg);
}

ALWAYS_INLINE Val *make_field(Code *kid, Env *env, u32 gap);

ALWAYS_INLINE Val *make_atom_field(Code *kid, Env *env, u32 gap, u8 kind) {
  if (kind == CK_NUM) return mk_num(kid->ext);
  if (kind == CK_VAR) return env_get(env, kid->ext, gap);
  return make_field(kid, env, gap);
}

ALWAYS_INLINE void init_ctr(Val *val, Code *pc, Env *env, u32 gap) {
  val->tag = V_CTR;
  val->ext = pc->ext;
  val->arity = pc->arity;
  val->pad = 0;
  if (val->arity == 2) {
    val->fst = make_atom_field(pc->sub, env, gap, pc->aux & 3);
    val->snd = make_atom_field(pc->next, env, gap, pc->aux >> 2);
  } else if (val->arity == 1) {
    val->fst = make_field(pc->sub, env, gap);
  } else if (val->arity > 2) {
    val->item = items_new(val->arity);
    for (u32 i = 0; i < val->arity; i++) {
      val->item[i] = make_field(pc->term->kid[i]->code, env, gap);
    }
  }
}

ALWAYS_INLINE Val *make_ctr(Code *pc, Env *env, u32 gap) {
  Val *val = val_new(V_CTR);
  init_ctr(val, pc, env, gap);
  return val;
}

ALWAYS_INLINE Val *make_field(Code *kid, Env *env, u32 gap) {
  if (kid->op == BC_NUM) return mk_num(kid->ext);
  if (kid->op == BC_VAR) return env_get(env, kid->ext, gap);
  if (kid->op == BC_CTR) return make_ctr(kid, env, gap);
  return mk_lthunk(kid, env, gap);
}

static Val *eval_code_into(Code *pc, Env *env, u32 gap, Arg *args, u32 argc, Val *dst) {
  static void *dispatch[] = {
    &&do_arg, &&do_args, &&do_var, &&do_dp0, &&do_dp1, &&do_ref, &&do_call_ref, &&do_num,
    &&do_ctr, &&do_lam, &&do_elam, &&do_slam, &&do_mat, &&do_mat_ctr, &&do_dup, &&do_sup, &&do_era
  };
  static int threaded = 0;
  if (!threaded) {
    for (u32 i = 0; i < NAME_LEN; i++) {
      if (DEFS[i]) thread_code(DEFS[i]->code, dispatch, 0);
    }
    threaded = 1;
  }
  Val *val = NULL;
  goto *pc->jump;

do_arg:
  if (argc >= MAX_ARGS) die("argument stack overflow");
  args[argc++] = arg_code(pc->sub, env, gap);
  pc = pc->next;
  goto *pc->jump;

do_args:
  if (argc + pc->arity > MAX_ARGS) die("argument stack overflow");
  if (pc->arity == 4) {
    args[argc++] = arg_code(pc->term->kid[0]->code, env, gap);
    args[argc++] = arg_code(pc->term->kid[1]->code, env, gap);
    args[argc++] = arg_code(pc->term->kid[2]->code, env, gap);
    args[argc++] = arg_code(pc->term->kid[3]->code, env, gap);
  } else if (pc->arity == 3) {
    args[argc++] = arg_code(pc->term->kid[0]->code, env, gap);
    args[argc++] = arg_code(pc->term->kid[1]->code, env, gap);
    args[argc++] = arg_code(pc->term->kid[2]->code, env, gap);
  } else if (pc->arity == 2) {
    args[argc++] = arg_code(pc->term->kid[0]->code, env, gap);
    args[argc++] = arg_code(pc->term->kid[1]->code, env, gap);
  } else if (pc->arity == 1) {
    args[argc++] = arg_code(pc->term->kid[0]->code, env, gap);
  } else {
    for (u32 i = 0; i < pc->arity; i++) {
      args[argc++] = arg_code(pc->term->kid[i]->code, env, gap);
    }
  }
  pc = pc->next;
  goto *pc->jump;

do_ref:
  if (pc->sub == NULL) die("unknown reference");
  if (argc == 0 && REF_CACHE[pc->ext] != NULL) {
    val = REF_CACHE[pc->ext];
    goto apply_ready;
  }
  pc = pc->sub;
  env = NULL;
  gap = 0;
  goto *pc->jump;

do_call_ref:
  if (pc->next == NULL) die("unknown reference");
  if (argc >= MAX_ARGS) die("argument stack overflow");
  if ((pc->next->op == BC_MAT || pc->next->op == BC_MAT_CTR) && (pc->sub->op == BC_CTR || pc->sub->op == BC_NUM)) {
    Code *arg_code = pc->sub;
    Code *body = NULL;
    if (arg_code->op == BC_CTR) {
      Cases *cases = pc->next->cases;
      if (cases->ctr && arg_code->ext == cases->ctr_ext) body = cases->ctr;
    } else {
      if (arg_code->ext == 0) body = pc->next->cases->num0;
      else if (arg_code->ext == 1) body = pc->next->cases->num1;
    }
    if (body != NULL) {
      ITRS++;
      Env *body_env = NULL;
      u32 body_gap = 0;
      if (arg_code->op == BC_CTR) {
        if (!enter_ctr2_code_num_mat(&body, arg_code, env, gap, args, &argc)
        &&  !enter_ctr2_code_lams(&body, arg_code, env, gap, &body_env, &body_gap)) {
          push_ctr_code_args(arg_code, env, gap, args, &argc);
        }
      }
      pc = body;
      env = body_env;
      gap = body_gap;
      goto *pc->jump;
    }
  }
  args[argc++] = arg_code(pc->sub, env, gap);
  pc = pc->next;
  env = NULL;
  gap = 0;
  goto *pc->jump;

do_var:
  if (pc->ext == 1 && gap == 0) {
    if (!env) die("unbound variable");
    if (!env->val) die("erased variable reached");
    val = env->val;
  } else if (pc->ext == 2 && gap == 0 && env != NULL && env_span(env) == 1) {
    Env *next = env_next(env);
    if (!next) die("unbound variable");
    if (!next->val) die("erased variable reached");
    val = next->val;
  } else {
    val = env_get(env, pc->ext, gap);
  }
  if (__builtin_expect(val->tag == V_THUNK || val->tag == V_LTHUNK, 0)) goto apply_value;
  goto apply_ready;

do_dp0:
do_dp1: {
    val = env_get(env, pc->ext, gap);
    if (!is_lam(val) || val->code->sup_lab != pc->arity || val->code->sup_has) {
      val = project(val, pc->arity, pc->op == BC_DP0 ? 0 : 1);
    }
    goto apply_ready;
  }

do_num:
  val = mk_num(pc->ext);
  goto apply_ready;

do_ctr:
  if (dst != NULL && argc == 0) {
    u16 refs = dst->pad;
    init_ctr(dst, pc, env, gap);
    dst->pad = refs;
    val = dst;
    dst = NULL;
  } else {
    val = make_ctr(pc, env, gap);
  }
  goto apply_ready;

do_lam:
  if (argc == 0) {
    val = mk_lam(pc->sub, env, gap);
    goto apply_ready;
  }
  ITRS++;
  {
    Arg *arg = &args[--argc];
    env = env_push(bind_arg(arg), env, gap + 1);
    gap = 0;
    pc = pc->sub;
    goto *pc->jump;
  }

do_elam:
  if (argc == 0) {
    val = mk_lam_tag(V_ELAM, pc->sub, env, gap);
    goto apply_ready;
  }
  ITRS++;
  {
    argc--;
    gap++;
    pc = pc->sub;
    goto *pc->jump;
  }

do_slam:
  if (argc == 0) {
    val = mk_lam_tag(V_SLAM, pc->sub, env, gap);
    goto apply_ready;
  }
  ITRS++;
  {
    Arg *arg = &args[--argc];
    env = env_push(share_value(bind_arg(arg)), env, gap + 1);
    gap = 0;
    pc = pc->sub;
    goto *pc->jump;
  }

do_mat:
  if (__builtin_expect(argc == 0, 0)) {
    val = mk_mat(pc, env, gap);
    goto apply_ready;
  }
  Arg *raw = &args[argc - 1];
  if (raw->val != NULL && raw->val->tag == V_NUM) {
    Code *body = mat_pick(pc, raw->val);
    if (body != NULL) {
      ITRS++;
      argc--;
      pc = body;
      goto *pc->jump;
    }
  }
  if (raw->val == NULL) {
    Code *arg_code = raw->code;
    Code *body = NULL;
    if (arg_code->op == BC_CTR) {
      Cases *cases = pc->cases;
      if (cases->ctr && arg_code->ext == cases->ctr_ext) body = cases->ctr;
    } else if (arg_code->op == BC_NUM) {
      if (arg_code->ext == 0) body = pc->cases->num0;
      else if (arg_code->ext == 1) body = pc->cases->num1;
    }
    if (body != NULL) {
      ITRS++;
      argc--;
      if (arg_code->op == BC_CTR) {
        if (!enter_ctr2_code_num_mat(&body, arg_code, raw->env, raw->gap, args, &argc)
        &&  !enter_ctr2_code_lams(&body, arg_code, raw->env, raw->gap, &env, &gap)) {
          push_ctr_code_args(arg_code, raw->env, raw->gap, args, &argc);
        }
      }
      pc = body;
      goto *pc->jump;
    }
  }
  Env *arg_env;
  u32 arg_gap;
  Val *seen;
  Code *arg_code = matchable_arg_code(raw, &arg_env, &arg_gap, &seen);
  if (arg_code != NULL) {
    Code *body = mat_pick_code(pc, arg_code);
    if (body) {
      ITRS++;
      if (seen) consume_matchable_seen(seen);
      argc--;
      if (arg_code->op == BC_CTR) {
        if (!enter_ctr2_code_num_mat(&body, arg_code, arg_env, arg_gap, args, &argc)
        &&  !enter_ctr2_code_lams(&body, arg_code, arg_env, arg_gap, &env, &gap)) {
          push_ctr_code_args(arg_code, arg_env, arg_gap, args, &argc);
        }
      }
      pc = body;
      goto *pc->jump;
    }
  }
  ITRS++;
  {
    argc--;
    Val *arg = seen ? force(seen) : force_arg(raw);
    if (arg->tag == V_SUP) {
      val = apply_mat(pc, env, gap, arg);
      goto apply_value;
    }
    Code *body = mat_pick(pc, arg);
    if (!body) {
      val = apply_default(pc->cases->dft, env, gap, arg);
      goto apply_value;
    }
    if (arg->tag == V_CTR) {
      if (!enter_ctr2_val_num_mat(&body, arg, args, &argc)
      &&  !enter_ctr2_val_lams(&body, arg, &env, &gap)) {
        push_ctr_val_args(arg, args, &argc);
      }
      val_free_ctr(arg);
    }
    pc = body;
    goto *pc->jump;
  }

do_mat_ctr:
  if (__builtin_expect(argc == 0, 0)) {
    val = mk_mat(pc, env, gap);
    goto apply_ready;
  }
  {
    Arg *raw = &args[argc - 1];
    if (raw->val == NULL) {
      Code *arg_code = raw->code;
      if (arg_code->op == BC_CTR && arg_code->ext == pc->ext) {
        ITRS++;
        argc--;
        Code *body = pc->sub;
        if (!enter_ctr2_code_num_mat(&body, arg_code, raw->env, raw->gap, args, &argc)
        &&  !enter_ctr2_code_lams(&body, arg_code, raw->env, raw->gap, &env, &gap)) {
          push_ctr_code_args(arg_code, raw->env, raw->gap, args, &argc);
        }
        pc = body;
        goto *pc->jump;
      }
    }
    Env *arg_env;
    u32 arg_gap;
    Val *seen;
    Code *arg_code = matchable_arg_code(raw, &arg_env, &arg_gap, &seen);
    if (arg_code != NULL && arg_code->op == BC_CTR && arg_code->ext == pc->ext) {
      ITRS++;
      if (seen) consume_matchable_seen(seen);
      argc--;
      Code *body = pc->sub;
      if (!enter_ctr2_code_num_mat(&body, arg_code, arg_env, arg_gap, args, &argc)
      &&  !enter_ctr2_code_lams(&body, arg_code, arg_env, arg_gap, &env, &gap)) {
        push_ctr_code_args(arg_code, arg_env, arg_gap, args, &argc);
      }
      pc = body;
      goto *pc->jump;
    }
    ITRS++;
    argc--;
    Val *arg = seen ? force(seen) : force_arg(raw);
    if (arg->tag == V_SUP) {
      val = apply_mat(pc, env, gap, arg);
      goto apply_value;
    }
    if (arg->tag == V_CTR && arg->ext == pc->ext) {
      Code *body = pc->sub;
      if (!enter_ctr2_val_num_mat(&body, arg, args, &argc)
      &&  !enter_ctr2_val_lams(&body, arg, &env, &gap)) {
        push_ctr_val_args(arg, args, &argc);
      }
      val_free_ctr(arg);
      pc = body;
      goto *pc->jump;
    }
    val = mk_num(0);
    goto apply_value;
  }

do_dup:
  ITRS++;
  {
    Val *v = force(mk_thunk(pc->sub, env, gap));
    if (is_lam(v) && code_has_sup_label(v->code, pc->ext, 0)) v = mk_dlam(v, pc->ext);
    env = env_push(v, env, gap + 1);
    gap = 0;
    pc = pc->next;
    goto *pc->jump;
  }

do_sup:
  val = mk_sup(pc->ext, mk_thunk(pc->term->kid[0]->code, env, gap), mk_thunk(pc->term->kid[1]->code, env, gap));
  goto apply_ready;

do_era:
  val = val_new(V_ERA);
  goto apply_ready;

apply_value:
  val = force(val);
apply_ready:
  if (argc == 0) return val;
  switch (val->tag) {
    case V_LAM: {
      ITRS++;
      Arg *arg = &args[--argc];
      env = val->env;
      gap = val->ext;
      env = env_push(bind_arg(arg), env, gap + 1);
      gap = 0;
      pc = val->code;
      goto *pc->jump;
    }
    case V_ELAM: {
      ITRS++;
      argc--;
      env = val->env;
      gap = val->ext + 1;
      pc = val->code;
      goto *pc->jump;
    }
    case V_SLAM: {
      ITRS++;
      Arg *arg = &args[--argc];
      env = val->env;
      gap = val->ext;
      env = env_push(share_value(bind_arg(arg)), env, gap + 1);
      gap = 0;
      pc = val->code;
      goto *pc->jump;
    }
    case V_MAT: {
      Arg *raw = &args[argc - 1];
      if (raw->val != NULL && raw->val->tag == V_NUM) {
        Code *body = mat_pick(val->code, raw->val);
        if (body != NULL) {
          ITRS++;
          argc--;
          Env *body_env = val->env;
          u32 body_gap = val->ext;
          env = body_env;
          gap = body_gap;
          pc = body;
          goto *pc->jump;
        }
      }
      if (raw->val == NULL) {
        Code *arg_code = raw->code;
        Code *body = NULL;
        if (arg_code->op == BC_CTR) {
          Cases *cases = val->code->cases;
          if (cases->ctr && arg_code->ext == cases->ctr_ext) body = cases->ctr;
        } else if (arg_code->op == BC_NUM) {
          if (arg_code->ext == 0) body = val->code->cases->num0;
          else if (arg_code->ext == 1) body = val->code->cases->num1;
        }
        if (body != NULL) {
          ITRS++;
          argc--;
          Env *body_env = val->env;
          u32 body_gap = val->ext;
          if (arg_code->op == BC_CTR) {
            if (!enter_ctr2_code_num_mat(&body, arg_code, raw->env, raw->gap, args, &argc)
            &&  !enter_ctr2_code_lams(&body, arg_code, raw->env, raw->gap, &body_env, &body_gap)) {
              push_ctr_code_args(arg_code, raw->env, raw->gap, args, &argc);
            }
          }
          env = body_env;
          gap = body_gap;
          pc = body;
          goto *pc->jump;
        }
      }
      Env *arg_env;
      u32 arg_gap;
      Val *seen;
      Code *arg_code = matchable_arg_code(raw, &arg_env, &arg_gap, &seen);
      if (arg_code != NULL) {
        Code *body = mat_pick_code(val->code, arg_code);
        if (body) {
          ITRS++;
          if (seen) consume_matchable_seen(seen);
          argc--;
          Env *body_env = val->env;
          u32 body_gap = val->ext;
          if (arg_code->op == BC_CTR) {
            if (!enter_ctr2_code_num_mat(&body, arg_code, arg_env, arg_gap, args, &argc)
            &&  !enter_ctr2_code_lams(&body, arg_code, arg_env, arg_gap, &body_env, &body_gap)) {
              push_ctr_code_args(arg_code, arg_env, arg_gap, args, &argc);
            }
          }
          env = body_env;
          gap = body_gap;
          pc = body;
          goto *pc->jump;
        }
      }
      ITRS++;
      argc--;
      Val *arg = seen ? force(seen) : force_arg(raw);
      if (arg->tag == V_SUP) {
        val = apply_mat(val->code, val->env, val->ext, arg);
        goto apply_value;
      }
      Code *body = mat_pick(val->code, arg);
      if (!body) {
        val = apply_default(val->code->cases->dft, val->env, val->ext, arg);
        goto apply_value;
      }
      Env *body_env = val->env;
      u32 body_gap = val->ext;
      if (arg->tag == V_CTR) {
        if (!enter_ctr2_val_num_mat(&body, arg, args, &argc)
        &&  !enter_ctr2_val_lams(&body, arg, &body_env, &body_gap)) {
          push_ctr_val_args(arg, args, &argc);
        }
        val_free_ctr(arg);
      }
      env = body_env;
      gap = body_gap;
      pc = body;
      goto *pc->jump;
    }
    case V_SUP: {
      Arg arg = args[--argc];
      val = apply_sup(val, &arg);
      goto apply_value;
    }
    case V_PRJ: {
      Arg arg = args[--argc];
      val = apply_fun(val, &arg);
      goto apply_value;
    }
    case V_PLAM: {
      Arg arg = args[--argc];
      val = apply_plam(val, &arg);
      goto apply_value;
    }
    default:
      if (val->tag == V_VAR || val->tag == V_APP || val->tag == V_BOX) {
        Arg arg = args[--argc];
        val = mk_app(val, arg.val != NULL ? arg.val : mk_thunk(arg.code, arg.env, arg.gap));
        goto apply_value;
      }
      die("cannot apply value");
  }
  return val;
}

static Val *eval_term(Term *term, Env *env) {
  Arg args[MAX_ARGS];
  return eval_code(term->code, env, 0, args, 0);
}

ALWAYS_INLINE Val *force(Val *v) {
  while (v->tag == V_THUNK || v->tag == V_LTHUNK) {
    if (v->tag == V_LTHUNK && v->code->op == BC_CTR) {
      u16 refs = v->pad;
      init_ctr(v, v->code, v->env, v->ext);
      v->pad = refs;
      return v;
    }
    u8 tag = v->tag;
    u16 refs = v->pad;
    Arg args[MAX_ARGS];
    Val *res = eval_code_into(v->code, v->env, v->ext, args, 0, tag == V_LTHUNK ? v : NULL);
    if (tag == V_LTHUNK && res != v) {
      *v = *res;
      if (v->tag == V_CTR) {
        v->pad = refs;
        val_free_ctr(res);
      }
    } else {
      v = res;
    }
  }
  return v;
}

ALWAYS_INLINE Val *force_read(Val *v) {
  return force(v);
}

static void print_var_name(u32 idx) {
  if (idx < 26) {
    putchar((char)('a' + idx));
  } else {
    printf("x%u", idx);
  }
}

static void print_val_at(Val *v, u32 depth);
static void normalize_val_at(Val *v, u32 depth);
static void normalize_forced_val_at(Val *v, u32 depth);

static void print_mat_val(Val *v, u32 depth) {
  printf("λ{");
  int first = 1;
  Term *m = v->code->cases->chain;
  for (; m && m->tag == T_MAT; m = m->kid[1]) {
    if (!first) putchar(';');
    first = 0;
    if (m->aux == M_CTR) {
      printf("#%s", NAMES[m->ext]);
    } else {
      printf("%u", m->ext);
    }
    putchar(':');
    Arg none[MAX_ARGS];
    Val *body = eval_code(m->kid[0]->code, v->env, v->ext, none, 0);
    print_val_at(body, depth);
  }
  if (m != NULL) {
    if (!first) putchar(';');
    Arg none[MAX_ARGS];
    Val *body = eval_code(m->code, v->env, v->ext, none, 0);
    print_val_at(body, depth);
  }
  putchar('}');
}

static void print_val_at(Val *v, u32 depth) {
  if (v->tag == V_BOX) {
    if (v->fst) {
      printf("@{");
      print_val_at(v->fst, depth);
      putchar('}');
    } else {
      print_var_name(depth);
    }
    return;
  }
  v = force_read(v);
  switch (v->tag) {
    case V_NUM:
      printf("%u", v->ext);
      return;
    case V_CTR:
      if (v->arity == 2) {
        v->fst = force_read(v->fst);
        v->snd = force_read(v->snd);
      } else if (v->arity == 1) {
        v->fst = force_read(v->fst);
      } else {
        for (u32 i = 0; i < v->arity; i++) {
          v->item[i] = force_read(v->item[i]);
        }
      }
      printf("#%s{", NAMES[v->ext]);
      for (u32 i = 0; i < v->arity; i++) {
        if (i) putchar(',');
        print_val_at(ctr_get(v, i), depth);
      }
      putchar('}');
      return;
    case V_LAM:
    case V_ELAM:
    case V_SLAM: {
      putchar('\xCE');
      putchar('\xBB');
      print_var_name(depth);
      putchar('.');
      Env *env = v->env;
      u32 gap = v->ext;
      if (v->tag == V_ELAM) {
        gap++;
      } else {
        env = env_push(mk_var(depth), env, gap + 1);
        gap = 0;
      }
      Arg none[MAX_ARGS];
      Val *body = eval_code(v->code, env, gap, none, 0);
      print_val_at(body, depth + 1);
      return;
    }
    case V_MAT:
      print_mat_val(v, depth);
      return;
    case V_SUP:
      printf("&%s{", NAMES[v->ext]);
      print_val_at(v->fst, depth);
      putchar(',');
      print_val_at(v->snd, depth);
      putchar('}');
      return;
    case V_VAR:
      print_var_name(v->ext);
      return;
    case V_APP:
      print_val_at(v->fst, depth);
      putchar('(');
      print_val_at(v->snd, depth);
      putchar(')');
      return;
    case V_PRJ:
      print_val_at(v->fst, depth);
      printf("%s", v->arity == 0 ? "₀" : "₁");
      return;
    case V_PLAM:
      print_val_at(v->fst, depth);
      printf("%s", v->arity == 0 ? "₀" : "₁");
      return;
    case V_ERA:
      printf("&{}");
      return;
    default:
      printf("<?>");
      return;
  }
}

static void print_val(Val *v) {
  print_val_at(v, 0);
}

static void normalize_mat_val(Val *v, u32 depth) {
  Term *m = v->code->cases->chain;
  for (; m && m->tag == T_MAT; m = m->kid[1]) {
    Arg none[MAX_ARGS];
    Val *body = eval_code(m->kid[0]->code, v->env, v->ext, none, 0);
    normalize_val_at(body, depth);
  }
  if (m != NULL) {
    Arg none[MAX_ARGS];
    Val *body = eval_code(m->code, v->env, v->ext, none, 0);
    normalize_val_at(body, depth);
  }
}

static void normalize_val_at(Val *v, u32 depth) {
  if (v->tag == V_BOX) {
    if (v->fst) normalize_val_at(v->fst, depth);
    return;
  }
  normalize_forced_val_at(force_read(v), depth);
}

static void normalize_forced_val_at(Val *v, u32 depth) {
  switch (v->tag) {
    case V_BOX:
      if (v->fst) normalize_val_at(v->fst, depth);
      return;
    case V_CTR:
      if (v->arity == 2) {
        v->fst = force_read(v->fst);
        v->snd = force_read(v->snd);
        normalize_forced_val_at(v->fst, depth);
        normalize_forced_val_at(v->snd, depth);
      } else if (v->arity == 1) {
        v->fst = force_read(v->fst);
        normalize_forced_val_at(v->fst, depth);
      } else {
        for (u32 i = 0; i < v->arity; i++) {
          v->item[i] = force_read(v->item[i]);
          normalize_forced_val_at(v->item[i], depth);
        }
      }
      return;
    case V_LAM:
    case V_ELAM:
    case V_SLAM: {
      Env *env = v->env;
      u32 gap = v->ext;
      if (v->tag == V_ELAM) {
        gap++;
      } else {
        env = env_push(mk_var(depth), env, gap + 1);
        gap = 0;
      }
      Arg none[MAX_ARGS];
      Val *body = eval_code(v->code, env, gap, none, 0);
      normalize_val_at(body, depth + 1);
      return;
    }
    case V_MAT:
      normalize_mat_val(v, depth);
      return;
    case V_SUP:
      normalize_val_at(v->fst, depth);
      normalize_val_at(v->snd, depth);
      return;
    case V_APP:
      normalize_val_at(v->fst, depth);
      normalize_val_at(v->snd, depth);
      return;
    case V_PRJ:
    case V_PLAM:
      normalize_val_at(v->fst, depth);
      return;
    default:
      return;
  }
}

static void normalize_val(Val *v) {
  normalize_val_at(v, 0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s file.hvm [-s] [-S]\n", argv[0]);
    return 1;
  }
  int stats = 0;
  int silent = 0;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stats") == 0) stats = 1;
    if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--silent") == 0) silent = 1;
  }
  char *src = read_file(argv[1]);
  if (!src) {
    fprintf(stderr, "Cannot open '%s'\n", argv[1]);
    return 1;
  }
  parse_program(src);
  compile_program_terms();
  u32 main_id = name_intern("main", 4);
  if (DEFS[main_id] == NULL) die("missing @main");

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  Val *res = force(eval_term(DEFS[main_id], NULL));
  normalize_val(res);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

  if (!silent) {
    print_val(res);
    putchar('\n');
  }
  if (stats) {
    fprintf(stderr, "- Itrs: %llu interactions\n", (unsigned long long)ITRS);
    fprintf(stderr, "- Heap: %llu nodes\n", (unsigned long long)val_live_count());
    fprintf(stderr, "- Time: %.3f seconds\n", elapsed);
    fprintf(stderr, "- Perf: %.2f M interactions/s\n", elapsed > 0 ? (double)ITRS / elapsed / 1000000.0 : 0.0);
  }
  free(src);
  return 0;
}
