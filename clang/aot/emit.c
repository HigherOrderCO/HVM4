// AOT Module: Program Emitter
// ---------------------------
// Emits a standalone C program with runtime include, compiled fast-path
// functions, registration, and a minimal `main` that runs @main.

fn void sys_error(const char *msg);
fn char *table_get(u32 id);
fn void print_term_quoted_ex(FILE *f, Term term, u32 depth);

// One fallback location with its minimum root-relative lambda depth.
typedef struct {
  u64 loc;
  u32 dep;
} AotLoc;

// One compiled fast-path state (LAM, MAT, or SWI).
typedef struct {
  u64 loc;
  u8  tag;
  u16 ext;
  u64 hit;
  u64 mis;
} AotState;

// Planning buffers collected from one root static location.
typedef struct {
  AotLoc   *locs;
  u32       loc_len;
  u32       loc_cap;
  AotState *sts;
  u32       st_len;
  u32       st_cap;
} AotPlan;

// Returns the index of a location in the fallback table, or -1 if absent.
fn int aot_emit_find_loc(const AotPlan *plan, u64 loc) {
  for (u32 i = 0; i < plan->loc_len; i++) {
    if (plan->locs[i].loc == loc) {
      return (int)i;
    }
  }
  return -1;
}

// Returns the index of a compiled state, or -1 if absent.
fn int aot_emit_find_state(const AotPlan *plan, u64 loc) {
  for (u32 i = 0; i < plan->st_len; i++) {
    if (plan->sts[i].loc == loc) {
      return (int)i;
    }
  }
  return -1;
}

// Grows a dynamic array with a doubling strategy.
fn void aot_emit_grow(void **buf, u32 *cap, size_t item_size, const char *what) {
  u32 new_cap  = *cap == 0 ? 16 : (*cap * 2);
  void *newbuf = realloc(*buf, (size_t)new_cap * item_size);
  if (newbuf == NULL) {
    sys_error(what);
  }
  *buf = newbuf;
  *cap = new_cap;
}

// Inserts one fallback location, keeping the minimum observed depth.
fn void aot_emit_add_loc(AotPlan *plan, u64 loc, u32 dep) {
  int idx = aot_emit_find_loc(plan, loc);
  if (idx >= 0) {
    if (dep < plan->locs[idx].dep) {
      plan->locs[idx].dep = dep;
    }
    return;
  }

  if (plan->loc_len >= plan->loc_cap) {
    aot_emit_grow((void **)&plan->locs, &plan->loc_cap, sizeof(AotLoc), "AOT location allocation failed");
  }

  plan->locs[plan->loc_len++] = (AotLoc){
    .loc = loc,
    .dep = dep,
  };
}

// Appends one compiled state.
fn void aot_emit_add_state(AotPlan *plan, AotState st) {
  if (plan->st_len >= plan->st_cap) {
    aot_emit_grow((void **)&plan->sts, &plan->st_cap, sizeof(AotState), "AOT state allocation failed");
  }
  plan->sts[plan->st_len++] = st;
}

// Forward declaration for the recursive planner walk.
fn void aot_emit_walk(AotPlan *plan, u64 loc, u32 dep);

// Collects all fast-path states and fallback locations in root-first order.
fn void aot_emit_walk(AotPlan *plan, u64 loc, u32 dep) {
  aot_emit_add_loc(plan, loc, dep);

  if (aot_emit_find_state(plan, loc) >= 0) {
    return;
  }

  Term t   = heap_read(loc);
  u8   tag = term_tag(t);

  switch (tag) {
    case LAM: {
      u64 nxt = term_val(t);
      aot_emit_add_state(plan, (AotState){
        .loc = loc,
        .tag = LAM,
        .ext = term_ext(t),
        .hit = nxt,
        .mis = 0,
      });
      aot_emit_walk(plan, nxt, dep + 1);
      return;
    }
    case MAT:
    case SWI: {
      u64 mat_loc = term_val(t);
      aot_emit_add_state(plan, (AotState){
        .loc = loc,
        .tag = tag,
        .ext = term_ext(t),
        .hit = mat_loc + 0,
        .mis = mat_loc + 1,
      });
      aot_emit_walk(plan, mat_loc + 0, dep + 0);
      aot_emit_walk(plan, mat_loc + 1, dep + 0);
      return;
    }
    default: {
      return;
    }
  }
}

// Returns decimal width for LOC_ indices (1..9 => 1, 10..99 => 2, ...).
fn u32 aot_emit_loc_width(u32 len) {
  u32 n = len == 0 ? 1 : len;
  u32 w = 1;
  while (n >= 10) {
    n /= 10;
    w++;
  }
  return w;
}

// Builds one LOC_<index> symbol name.
fn void aot_emit_loc_name(char *out, u32 out_cap, u32 idx, u32 width) {
  snprintf(out, out_cap, "LOC_%0*u", (int)width, idx);
}

// Resolves one static location to LOC_<index> or raw literal if missing.
fn void aot_emit_loc_ref(char *out, u32 out_cap, const AotPlan *plan, u32 width, u64 loc) {
  int idx = aot_emit_find_loc(plan, loc);
  if (idx >= 0) {
    aot_emit_loc_name(out, out_cap, (u32)idx, width);
    return;
  }
  snprintf(out, out_cap, "%lluULL", (unsigned long long)loc);
}

// Serializes one static term into a compact single-line comment.
fn void aot_emit_term_line(char *out, u32 out_cap, Term term, u32 dep) {
  if (out_cap == 0) {
    return;
  }

  FILE *tmp = tmpfile();
  if (tmp == NULL) {
    snprintf(out, out_cap, "<term>");
    return;
  }

  print_term_quoted_ex(tmp, term, dep);
  fflush(tmp);
  rewind(tmp);

  if (fgets(out, (int)out_cap, tmp) == NULL) {
    fclose(tmp);
    snprintf(out, out_cap, "<term>");
    return;
  }

  fclose(tmp);

  u32 w = 0;
  u8  ws = 0;
  for (u32 r = 0; out[r] != '\0'; r++) {
    char c = out[r];
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
    if (c == ' ') {
      if (ws) {
        continue;
      }
      ws = 1;
    } else {
      ws = 0;
    }
    out[w++] = c;
  }

  while (w > 0 && out[w - 1] == ' ') {
    w--;
  }

  out[w] = '\0';
  if (w == 0) {
    snprintf(out, out_cap, "<term>");
  }
}

// Writes one escaped byte as part of a C string literal.
fn void aot_emit_escaped_byte(FILE *f, u8 c) {
  switch (c) {
    case '\\': fputs("\\\\", f); break;
    case '"':  fputs("\\\"", f); break;
    case '\n': fputs("\\n", f); break;
    case '\r': fputs("\\r", f); break;
    case '\t': fputs("\\t", f); break;
    default: {
      if (c >= 32 && c <= 126) {
        fputc((int)c, f);
      } else {
        fprintf(f, "\\%03o", (unsigned)c);
      }
      break;
    }
  }
}

// Writes one C string literal token with escapes.
fn void aot_emit_c_string_token(FILE *f, const char *str) {
  fputc('"', f);
  for (u32 i = 0; str[i] != '\0'; i++) {
    aot_emit_escaped_byte(f, (u8)str[i]);
  }
  fputc('"', f);
}

// Writes one multi-line C string declaration from bytes.
fn void aot_emit_c_string_decl(FILE *f, const char *name, const char *text) {
  fprintf(f, "static const char *%s =\n", name);
  const u8 *ptr = (const u8 *)text;
  u32 i = 0;

  if (ptr[0] == '\0') {
    fprintf(f, "  \"\";\n\n");
    return;
  }

  while (ptr[i] != 0) {
    fprintf(f, "  \"");
    u32 chunk = 0;
    while (ptr[i] != 0 && chunk < 64) {
      aot_emit_escaped_byte(f, ptr[i++]);
      chunk++;
    }
    fprintf(f, "\"\n");
  }

  fprintf(f, "  ;\n\n");
}

// Emits location constants used by one compiled function.
fn void aot_emit_consts(FILE *f, const AotPlan *plan, u32 width) {
  fprintf(f, "  // Static locations used for fallback ALO allocation.\n");
  fprintf(f, "  enum {\n");
  for (u32 i = 0; i < plan->loc_len; i++) {
    char loc_name[64];
    char loc_note[256];
    aot_emit_loc_name(loc_name, sizeof(loc_name), i, width);
    aot_emit_term_line(loc_note, sizeof(loc_note), heap_read(plan->locs[i].loc), plan->locs[i].dep);
    fprintf(f, "    %s = %lluULL, // %s\n", loc_name, (unsigned long long)plan->locs[i].loc, loc_note);
  }
  fprintf(f, "  };\n\n");
}

// Emits a transition to another state or immediate fallback.
fn void aot_emit_jump(FILE *f, const AotPlan *plan, u32 width, u64 loc, const char *pad) {
  char loc_ref[64];
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, loc);

  if (aot_emit_find_state(plan, loc) < 0) {
    fprintf(f, "%sreturn aot_fallback_alo(%s, env_len, env);\n", pad, loc_ref);
    return;
  }

  fprintf(f, "%sat = %s;\n", pad, loc_ref);
  fprintf(f, "%scontinue;\n", pad);
}

// Emits one fast-path switch case.
fn void aot_emit_case(FILE *f, const AotPlan *plan, u32 width, AotState st) {
  char loc_ref[64];
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, st.loc);

  switch (st.tag) {
    case LAM: {
      fprintf(f, "      case %s: { // APP-LAM\n", loc_ref);
      fprintf(f, "        if (*s_pos <= base) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        Term frame = stack[*s_pos - 1];\n");
      fprintf(f, "        if (term_tag(frame) != APP) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        (*s_pos)--;\n");
      fprintf(f, "        u64 app_loc = term_val(frame);\n");
      fprintf(f, "        env[env_len++] = heap_read(app_loc + 1);\n");
      fprintf(f, "        ITRS_INC(\"APP-LAM\");\n");
      aot_emit_jump(f, plan, width, st.hit, "        ");
      fprintf(f, "      }\n");
      return;
    }

    case SWI: {
      fprintf(f, "      case %s: { // APP-MAT-NUM (== %u)\n", loc_ref, st.ext);
      fprintf(f, "        if (*s_pos <= base) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        Term frame = stack[*s_pos - 1];\n");
      fprintf(f, "        if (term_tag(frame) != APP) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        u64  app_loc = term_val(frame);\n");
      fprintf(f, "        Term arg     = heap_read(app_loc + 1);\n");
      fprintf(f, "        if (term_tag(arg) != NUM) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        if (term_val(arg) == %uULL) {\n", st.ext);
      fprintf(f, "          (*s_pos)--;\n");
      fprintf(f, "          ITRS_INC(\"APP-MAT-NUM-MAT\");\n");
      aot_emit_jump(f, plan, width, st.hit, "          ");
      fprintf(f, "        }\n");
      fprintf(f, "        ITRS_INC(\"APP-MAT-NUM-MIS\");\n");
      aot_emit_jump(f, plan, width, st.mis, "        ");
      fprintf(f, "      }\n");
      return;
    }

    case MAT: {
      char *ctr_name = table_get(st.ext);
      if (ctr_name != NULL) {
        fprintf(f, "      case %s: { // APP-MAT-CTR (#%s)\n", loc_ref, ctr_name);
      } else {
        fprintf(f, "      case %s: { // APP-MAT-CTR (id %u)\n", loc_ref, st.ext);
      }
      fprintf(f, "        if (*s_pos <= base) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        Term frame = stack[*s_pos - 1];\n");
      fprintf(f, "        if (term_tag(frame) != APP) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        u64  app_loc = term_val(frame);\n");
      fprintf(f, "        Term ctr     = heap_read(app_loc + 1);\n");
      fprintf(f, "        u8   ctr_tag = term_tag(ctr);\n");
      fprintf(f, "        if (ctr_tag < C00 || ctr_tag > C16) {\n");
      fprintf(f, "          return aot_fallback_alo(%s, env_len, env);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        if (term_ext(ctr) == %u) {\n", st.ext);
      fprintf(f, "          (*s_pos)--;\n");
      fprintf(f, "          ITRS_INC(\"APP-MAT-CTR-MAT\");\n");
      fprintf(f, "          aot_push_ctr_apps(stack, s_pos, ctr, ctr_tag);\n");
      aot_emit_jump(f, plan, width, st.hit, "          ");
      fprintf(f, "        }\n");
      fprintf(f, "        ITRS_INC(\"APP-MAT-CTR-MIS\");\n");
      aot_emit_jump(f, plan, width, st.mis, "        ");
      fprintf(f, "      }\n");
      return;
    }

    default: {
      return;
    }
  }
}

// Emits one compiled function for one top-level definition id.
fn void aot_emit_def(FILE *f, u32 id) {
  if (BOOK[id] == 0) {
    return;
  }

  char *name = table_get(id);
  if (name == NULL) {
    return;
  }

  AotPlan plan = {0};
  u64     root = BOOK[id];
  aot_emit_walk(&plan, root, 0);

  u32 width   = aot_emit_loc_width(plan.loc_len);
  u32 env_cap = 0;
  for (u32 i = 0; i < plan.st_len; i++) {
    if (plan.sts[i].tag == LAM) {
      env_cap++;
    }
  }
  if (env_cap == 0) {
    env_cap = 1;
  }

  fprintf(f, "// Compiled fast-path for @%s (id %u).\n", name, id);
  fprintf(f, "static Term F_%u(Term *stack, u32 *s_pos, u32 base) {\n", id);
  fprintf(f, "  Term env[%u];\n", env_cap);
  fprintf(f, "  u16  env_len = 0;\n\n");

  aot_emit_consts(f, &plan, width);

  char root_ref[64];
  aot_emit_loc_ref(root_ref, sizeof(root_ref), &plan, width, root);

  if (plan.st_len == 0) {
    fprintf(f, "  return aot_fallback_alo(%s, env_len, env);\n", root_ref);
    fprintf(f, "}\n\n");
    free(plan.sts);
    free(plan.locs);
    return;
  }

  fprintf(f, "  u64 at = %s;\n\n", root_ref);
  fprintf(f, "  for (;;) {\n");
  fprintf(f, "    switch (at) {\n");
  for (u32 i = 0; i < plan.st_len; i++) {
    aot_emit_case(f, &plan, width, plan.sts[i]);
  }
  fprintf(f, "      default: {\n");
  fprintf(f, "        return aot_fallback_alo(at, env_len, env);\n");
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  fprintf(f, "  }\n");
  fprintf(f, "}\n\n");

  free(plan.sts);
  free(plan.locs);
}

// Emits registration for all compiled definitions.
fn void aot_emit_register(FILE *f) {
  fprintf(f, "// Registers generated fast paths into the runtime table.\n");
  fprintf(f, "static void aot_register_generated(void) {\n");
  for (u32 id = 0; id < TABLE.len; id++) {
    if (BOOK[id] == 0) {
      continue;
    }
    char *name = table_get(id);
    if (name == NULL) {
      continue;
    }
    fprintf(f, "  // @%s\n", name);
    fprintf(f, "  AOT_FNS[%u] = F_%u;\n", id, id);
  }
  fprintf(f, "}\n\n");
}

// Emits the standalone entrypoint using shared runtime helpers.
fn void aot_emit_entry_main(FILE *f) {
  fprintf(f, "int main(void) {\n");
  fprintf(f, "  eval_runtime_init(1, 0, 0, 0);\n");
  fprintf(f, "  char *src = strdup(AOT_SOURCE_TEXT);\n");
  fprintf(f, "  if (src == NULL) {\n");
  fprintf(f, "    sys_error(\"AOT source allocation failed\");\n");
  fprintf(f, "  }\n");
  fprintf(f, "  eval_parse_source(AOT_SOURCE_PATH, src);\n");
  fprintf(f, "  free(src);\n");
  fprintf(f, "\n");
  fprintf(f, "  if (!eval_check_alo_space()) {\n");
  fprintf(f, "    return 1;\n");
  fprintf(f, "  }\n");
  fprintf(f, "\n");
  fprintf(f, "  aot_register_generated();\n");
  fprintf(f, "  u32 main_id = 0;\n");
  fprintf(f, "  if (!eval_get_main_id(&main_id)) {\n");
  fprintf(f, "    fprintf(stderr, \"Error: @main not defined\\n\");\n");
  fprintf(f, "    return 1;\n");
  fprintf(f, "  }\n");
  fprintf(f, "\n");
  fprintf(f, "  Term result = eval_normalize(term_new_ref(main_id));\n");
  fprintf(f, "  print_term(result);\n");
  fprintf(f, "  printf(\"\\n\");\n");
  fprintf(f, "  return 0;\n");
  fprintf(f, "}\n");
}

// Emits the full standalone AOT C program.
fn void aot_emit_to_file(FILE *f, const char *runtime_path, const char *src_path, const char *src_text) {
  fprintf(f, "// Auto-generated by HVM4 AOT.\n");
  fprintf(f, "// This file is standalone: compile it with `clang -O2 -o <out> <this_file.c>`.\n");
  fprintf(f, "//\n");
  fprintf(f, "// AOT design summary:\n");
  fprintf(f, "// - Includes the full runtime translation unit directly (%s).\n", runtime_path);
  fprintf(f, "// - Compiles each top-level definition to a native fast-path function.\n");
  fprintf(f, "// - Fast-path interactions: APP-LAM, APP-MAT-NUM, APP-MAT-CTR.\n");
  fprintf(f, "// - Any unsupported case falls back to ALO reconstruction from current state.\n\n");

  fprintf(f, "#include ");
  aot_emit_c_string_token(f, runtime_path);
  fprintf(f, "\n\n");

  aot_emit_c_string_decl(f, "AOT_SOURCE_PATH", src_path);
  aot_emit_c_string_decl(f, "AOT_SOURCE_TEXT", src_text);

  for (u32 id = 0; id < TABLE.len; id++) {
    if (BOOK[id] == 0) {
      continue;
    }
    aot_emit_def(f, id);
  }

  aot_emit_register(f);
  aot_emit_entry_main(f);
}

// Emits the full standalone AOT C program to a file path.
fn void aot_emit(const char *c_path, const char *runtime_path, const char *src_path, const char *src_text) {
  FILE *f = fopen(c_path, "w");
  if (f == NULL) {
    fprintf(stderr, "ERROR: failed to open AOT output '%s'\n", c_path);
    exit(1);
  }

  aot_emit_to_file(f, runtime_path, src_path, src_text);
  fclose(f);
}

// Emits the full standalone AOT C program to stdout.
fn void aot_emit_stdout(const char *runtime_path, const char *src_path, const char *src_text) {
  aot_emit_to_file(stdout, runtime_path, src_path, src_text);
}
