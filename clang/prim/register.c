typedef Term (*PrimFn)(Term *args);

typedef struct {
  PrimFn fun;
  u32 arity;
} PrimDef;

static PrimDef PRIM_DEFS[BOOK_CAP];

fn u32 prim_register(const char *name, u32 len, u32 arity, PrimFn fun) {
  u32 id = table_find(name, len);
  if (PRIM_DEFS[id].fun != NULL) {
    fprintf(stderr, "WARNING: overriding primitive '%%%.*s'\n", len, name);
  }
  PRIM_DEFS[id].fun = fun;
  PRIM_DEFS[id].arity = arity;
  return id;
}

fn PrimFn prim_fun(u32 id) {
  return PRIM_DEFS[id].fun;
}

fn u32 prim_arity(u32 id) {
  return PRIM_DEFS[id].arity;
}
