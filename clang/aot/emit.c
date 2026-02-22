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

#define AOT_EVAL_LIT     1
#define AOT_EVAL_BIND    2
#define AOT_EVAL_OP2     3
#define AOT_EVAL_DUP     4
#define AOT_EVAL_APP_REF 5

// One compiled hot-eval node.
typedef struct {
  u64  loc;
  u8   kind;
  u32  ext;
  u16  arg_len;
  Term lit;
  u64  a;
  u64  b;
  u64  args[AOT_HOT_ARG_CAP];
} AotEval;

// Planning buffers collected from one root static location.
typedef struct {
  AotLoc   *locs;
  u32       loc_len;
  u32       loc_cap;
  AotState *sts;
  u32       st_len;
  u32       st_cap;
  AotEval  *evs;
  u32       ev_len;
  u32       ev_cap;
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

// Returns the index of one hot-eval node, or -1 if absent.
fn int aot_emit_find_eval(const AotPlan *plan, u64 loc) {
  for (u32 i = 0; i < plan->ev_len; i++) {
    if (plan->evs[i].loc == loc) {
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

// Appends one hot-eval node.
fn void aot_emit_add_eval(AotPlan *plan, AotEval ev) {
  if (plan->ev_len >= plan->ev_cap) {
    aot_emit_grow((void **)&plan->evs, &plan->ev_cap, sizeof(AotEval), "AOT eval allocation failed");
  }
  plan->evs[plan->ev_len++] = ev;
}

// Forward declaration for the recursive planner walk.
fn void aot_emit_walk(AotPlan *plan, u64 loc, u32 dep);

// Collects all fast-path states and fallback locations in root-first order.
fn void aot_emit_walk(AotPlan *plan, u64 loc, u32 dep) {
  aot_emit_add_loc(plan, loc, dep);

  if (aot_emit_find_state(plan, loc) >= 0 || aot_emit_find_eval(plan, loc) >= 0) {
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
    case VAR:
    case BJV:
    case DP0:
    case BJ0:
    case DP1:
    case BJ1: {
      aot_emit_add_eval(plan, (AotEval){
        .loc  = loc,
        .kind = AOT_EVAL_BIND,
        .ext  = (u32)term_val(t),
      });
      return;
    }
    case OP2: {
      u64 op2_loc = term_val(t);
      aot_emit_add_eval(plan, (AotEval){
        .loc  = loc,
        .kind = AOT_EVAL_OP2,
        .ext  = term_ext(t),
        .a    = op2_loc + 0,
        .b    = op2_loc + 1,
      });
      aot_emit_walk(plan, op2_loc + 0, dep);
      aot_emit_walk(plan, op2_loc + 1, dep);
      return;
    }
    case DUP: {
      u64 dup_loc = term_val(t);
      aot_emit_add_eval(plan, (AotEval){
        .loc  = loc,
        .kind = AOT_EVAL_DUP,
        .a    = dup_loc + 0,
        .b    = dup_loc + 1,
      });
      aot_emit_walk(plan, dup_loc + 0, dep);
      aot_emit_walk(plan, dup_loc + 1, dep + 1);
      return;
    }
    case APP: {
      u64 head_loc = loc;
      u64 arg_rev[AOT_HOT_ARG_CAP];
      u16 arg_len = 0;

      for (;;) {
        Term head = heap_read(head_loc);
        if (term_tag(head) != APP) {
          break;
        }
        if (arg_len >= AOT_HOT_ARG_CAP) {
          return;
        }
        u64 app_loc = term_val(head);
        arg_rev[arg_len++] = app_loc + 1;
        head_loc = app_loc + 0;
      }

      Term head = heap_read(head_loc);
      if (term_tag(head) != REF) {
        return;
      }

      AotEval ev = {
        .loc     = loc,
        .kind    = AOT_EVAL_APP_REF,
        .ext     = term_ext(head),
        .arg_len = arg_len,
      };

      for (u16 i = 0; i < arg_len; i++) {
        ev.args[i] = arg_rev[arg_len - 1 - i];
      }

      aot_emit_add_eval(plan, ev);

      for (u16 i = 0; i < arg_len; i++) {
        aot_emit_walk(plan, ev.args[i], dep);
      }
      return;
    }
    case NAM:
    case REF:
    case PRI:
    case ERA:
    case NUM:
    case ANY:
    case C00:
    case C01:
    case C02:
    case C03:
    case C04:
    case C05:
    case C06:
    case C07:
    case C08:
    case C09:
    case C10:
    case C11:
    case C12:
    case C13:
    case C14:
    case C15:
    case C16: {
      aot_emit_add_eval(plan, (AotEval){
        .loc  = loc,
        .kind = AOT_EVAL_LIT,
        .lit  = t,
      });
      return;
    }
    default: {
      return;
    }
  }
}

// Returns 1 when one definition can run on the pure u32 numeric lane.
fn int aot_emit_can_num(const AotPlan *plan) {
  for (u32 i = 0; i < plan->st_len; i++) {
    if (plan->sts[i].tag != LAM && plan->sts[i].tag != SWI) {
      return 0;
    }
  }

  for (u32 i = 0; i < plan->ev_len; i++) {
    AotEval ev = plan->evs[i];
    switch (ev.kind) {
      case AOT_EVAL_BIND:
      case AOT_EVAL_OP2:
      case AOT_EVAL_DUP:
      case AOT_EVAL_APP_REF: {
        break;
      }
      case AOT_EVAL_LIT: {
        if (term_tag(ev.lit) != NUM) {
          return 0;
        }
        break;
      }
      default: {
        return 0;
      }
    }
  }

  return 1;
}

// Returns 1 when every self-recursive APP node calls with exactly one arg.
fn int aot_emit_self_arg1(const AotPlan *plan, u32 id) {
  int seen = 0;
  for (u32 i = 0; i < plan->ev_len; i++) {
    AotEval ev = plan->evs[i];
    if (ev.kind != AOT_EVAL_APP_REF) {
      continue;
    }
    if (ev.ext != id) {
      continue;
    }
    seen = 1;
    if (ev.arg_len != 1) {
      return 0;
    }
  }
  return seen;
}

// Returns 1 when every numeric APP node is self-recursive.
fn int aot_emit_self_only(const AotPlan *plan, u32 id) {
  for (u32 i = 0; i < plan->ev_len; i++) {
    AotEval ev = plan->evs[i];
    if (ev.kind != AOT_EVAL_APP_REF) {
      continue;
    }
    if (ev.ext != id) {
      return 0;
    }
  }
  return 1;
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

// Resolves one static location to a raw literal.
fn void aot_emit_loc_raw(char *out, u32 out_cap, u64 loc) {
  snprintf(out, out_cap, "%lluULL", (unsigned long long)loc);
}

// Returns 1 when a location can be returned directly without ALO fallback.
fn int aot_emit_is_direct_leaf(u64 loc) {
  Term t   = heap_read(loc);
  u8   tag = term_tag(t);

  switch (tag) {
    case NAM:
    case REF:
    case PRI:
    case ERA:
    case NUM:
    case ANY:
    case C00: {
      return 1;
    }
    default: {
      return 0;
    }
  }
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
    if (aot_emit_is_direct_leaf(loc)) {
      fprintf(f, "%sreturn heap_read(%s);\n", pad, loc_ref);
      return;
    }
    fprintf(f, "%sreturn aot_exec_loc(%s, env, env_len, stack, s_pos, base);\n", pad, loc_ref);
    return;
  }

  fprintf(f, "%sat = %s;\n", pad, loc_ref);
  fprintf(f, "%scontinue;\n", pad);
}

// Emits one hot-apply transition.
fn void aot_emit_hot_jump(FILE *f, const AotPlan *plan, u32 width, u64 loc, const char *pad) {
  char loc_ref[64];
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, loc);
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

// Emits one hot-apply switch case.
fn void aot_emit_hot_case(FILE *f, const AotPlan *plan, u32 width, AotState st) {
  char loc_ref[64];
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, st.loc);

  switch (st.tag) {
    case LAM: {
      fprintf(f, "      case %s: { // APP-LAM\n", loc_ref);
      fprintf(f, "        if (i >= argc) {\n");
      fprintf(f, "          return aot_hot_fail_apply_env(%s, env_len, env, argc, args, i);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        if (env_len >= AOT_HOT_ENV_CAP) {\n");
      fprintf(f, "          return aot_hot_fail_apply_env(%s, env_len, env, argc, args, i);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        env[env_len++] = args[i];\n");
      fprintf(f, "        i++;\n");
      fprintf(f, "        ITRS_INC(\"APP-LAM\");\n");
      aot_emit_hot_jump(f, plan, width, st.hit, "        ");
      fprintf(f, "      }\n");
      return;
    }

    case SWI: {
      fprintf(f, "      case %s: { // APP-MAT-NUM (== %u)\n", loc_ref, st.ext);
      fprintf(f, "        if (i >= argc) {\n");
      fprintf(f, "          return aot_hot_fail_apply_env(%s, env_len, env, argc, args, i);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        Term arg = args[i];\n");
      fprintf(f, "        if (term_tag(arg) != NUM) {\n");
      fprintf(f, "          return aot_hot_fail_apply_env(%s, env_len, env, argc, args, i);\n", loc_ref);
      fprintf(f, "        }\n");
      fprintf(f, "        if (term_val(arg) == %uULL) {\n", st.ext);
      fprintf(f, "          i++;\n");
      fprintf(f, "          ITRS_INC(\"APP-MAT-NUM-MAT\");\n");
      aot_emit_hot_jump(f, plan, width, st.hit, "          ");
      fprintf(f, "        }\n");
      fprintf(f, "        ITRS_INC(\"APP-MAT-NUM-MIS\");\n");
      aot_emit_hot_jump(f, plan, width, st.mis, "        ");
      fprintf(f, "      }\n");
      return;
    }

    default: {
      return;
    }
  }
}

// Emits one numeric hot-apply switch case.
fn void aot_emit_num_case(FILE *f, const AotPlan *plan, u32 width, AotState st) {
  char loc_ref[64];
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, st.loc);

  switch (st.tag) {
    case LAM: {
      fprintf(f, "      case %s: {\n", loc_ref);
      fprintf(f, "        if (i >= argc) {\n");
      fprintf(f, "          return aot_num_fail();\n");
      fprintf(f, "        }\n");
      fprintf(f, "        if (env_len >= AOT_HOT_ENV_CAP) {\n");
      fprintf(f, "          return aot_num_fail();\n");
      fprintf(f, "        }\n");
      fprintf(f, "        env[env_len++] = args[i];\n");
      fprintf(f, "        i++;\n");
      aot_emit_hot_jump(f, plan, width, st.hit, "        ");
      fprintf(f, "      }\n");
      return;
    }
    case SWI: {
      fprintf(f, "      case %s: {\n", loc_ref);
      fprintf(f, "        if (i >= argc) {\n");
      fprintf(f, "          return aot_num_fail();\n");
      fprintf(f, "        }\n");
      fprintf(f, "        if (args[i] == %uU) {\n", st.ext);
      fprintf(f, "          i++;\n");
      aot_emit_hot_jump(f, plan, width, st.hit, "          ");
      fprintf(f, "        }\n");
      aot_emit_hot_jump(f, plan, width, st.mis, "        ");
      fprintf(f, "      }\n");
      return;
    }
    default: {
      return;
    }
  }
}

// Builds one eval-node helper name.
fn void aot_emit_eval_name(char *out, u32 out_cap, u32 id, u32 idx) {
  snprintf(out, out_cap, "FE_%u_%u", id, idx);
}

// Emits one direct call to an eval helper or dispatcher when missing.
fn void aot_emit_eval_call(FILE *f, u32 id, const AotPlan *plan, u32 width, u64 loc, const char *dst) {
  int ev_idx = aot_emit_find_eval(plan, loc);
  if (ev_idx >= 0) {
    char fn_name[64];
    aot_emit_eval_name(fn_name, sizeof(fn_name), id, (u32)ev_idx);
    fprintf(f, "  AotHotRes %s = %s(env, env_len, depth);\n", dst, fn_name);
    return;
  }

  char loc_ref[64];
  aot_emit_loc_raw(loc_ref, sizeof(loc_ref), loc);
  fprintf(f, "  AotHotRes %s = FE_%u(%s, env, env_len, depth);\n", dst, id, loc_ref);
}

// Emits one eval-node helper body.
fn void aot_emit_eval_node(FILE *f, u32 id, const AotPlan *plan, u32 width, u32 idx, AotEval ev) {
  char fn_name[64];
  char loc_ref[64];
  aot_emit_eval_name(fn_name, sizeof(fn_name), id, idx);
  aot_emit_loc_raw(loc_ref, sizeof(loc_ref), ev.loc);

  fprintf(f, "static AotHotRes %s(Term *env, u16 env_len, u32 depth) {\n", fn_name);
  fprintf(f, "  (void)depth;\n");

  switch (ev.kind) {
    case AOT_EVAL_LIT: {
      fprintf(f, "  return aot_hot_ok((Term)0x%016llxULL);\n", (unsigned long long)ev.lit);
      break;
    }
    case AOT_EVAL_BIND: {
      fprintf(f, "  if (%u == 0 || %u > env_len) {\n", ev.ext, ev.ext);
      fprintf(f, "    return aot_hot_fail_loc_env(%s, env_len, env);\n", loc_ref);
      fprintf(f, "  }\n");
      fprintf(f, "  return aot_hot_ok(env[env_len - %u]);\n", ev.ext);
      break;
    }
    case AOT_EVAL_OP2: {
      char rhs_ref[64];
      aot_emit_loc_raw(rhs_ref, sizeof(rhs_ref), ev.b);
      aot_emit_eval_call(f, id, plan, width, ev.a, "lhs");
      fprintf(f, "  if (!lhs.ok) {\n");
      fprintf(f, "    Term rhs = aot_fallback_alo(%s, env_len, env_len == 0 ? NULL : env);\n", rhs_ref);
      fprintf(f, "    Term res = term_new_op2(%u, lhs.term, rhs);\n", ev.ext);
      fprintf(f, "    return aot_hot_fail(res);\n");
      fprintf(f, "  }\n");
      fprintf(f, "  if (term_tag(lhs.term) != NUM) {\n");
      fprintf(f, "    return aot_hot_fail_loc_env(%s, env_len, env);\n", loc_ref);
      fprintf(f, "  }\n");
      aot_emit_eval_call(f, id, plan, width, ev.b, "rhs");
      fprintf(f, "  if (!rhs.ok) {\n");
      fprintf(f, "    Term res = term_new_op2(%u, lhs.term, rhs.term);\n", ev.ext);
      fprintf(f, "    return aot_hot_fail(res);\n");
      fprintf(f, "  }\n");
      fprintf(f, "  if (term_tag(rhs.term) != NUM) {\n");
      fprintf(f, "    Term res = term_new_op2(%u, lhs.term, rhs.term);\n", ev.ext);
      fprintf(f, "    return aot_hot_fail(res);\n");
      fprintf(f, "  }\n");
      fprintf(f, "  Term out = wnf_op2_num_num_raw(%u, (u32)term_val(lhs.term), (u32)term_val(rhs.term));\n", ev.ext);
      fprintf(f, "  return aot_hot_ok(out);\n");
      break;
    }
    case AOT_EVAL_DUP: {
      aot_emit_eval_call(f, id, plan, width, ev.a, "val");
      fprintf(f, "  if (!val.ok) {\n");
      fprintf(f, "    return aot_hot_fail_loc_env(%s, env_len, env);\n", loc_ref);
      fprintf(f, "  }\n");
      fprintf(f, "  if (!aot_hot_is_copy_free(val.term)) {\n");
      fprintf(f, "    return aot_hot_fail_loc_env(%s, env_len, env);\n", loc_ref);
      fprintf(f, "  }\n");
      fprintf(f, "  if (env_len >= AOT_HOT_ENV_CAP) {\n");
      fprintf(f, "    return aot_hot_fail_loc_env(%s, env_len, env);\n", loc_ref);
      fprintf(f, "  }\n");
      fprintf(f, "  env[env_len] = val.term;\n");
      fprintf(f, "  ITRS_INC(\"DUP-NOD\");\n");
      int bod_idx = aot_emit_find_eval(plan, ev.b);
      if (bod_idx >= 0) {
        char bod_name[64];
        aot_emit_eval_name(bod_name, sizeof(bod_name), id, (u32)bod_idx);
        fprintf(f, "  return %s(env, env_len + 1, depth);\n", bod_name);
      } else {
        char bod_ref[64];
        aot_emit_loc_raw(bod_ref, sizeof(bod_ref), ev.b);
        fprintf(f, "  return FE_%u(%s, env, env_len + 1, depth);\n", id, bod_ref);
      }
      break;
    }
    case AOT_EVAL_APP_REF: {
      u16 call_cap = ev.arg_len == 0 ? 1 : ev.arg_len;
      fprintf(f, "  Term call_args[%u];\n", call_cap);
      for (u16 i = 0; i < ev.arg_len; i++) {
        int arg_idx = aot_emit_find_eval(plan, ev.args[i]);
        if (arg_idx >= 0) {
          char arg_name[64];
          aot_emit_eval_name(arg_name, sizeof(arg_name), id, (u32)arg_idx);
          fprintf(f, "  AotHotRes arg_%u = %s(env, env_len, depth);\n", i, arg_name);
        } else {
          char arg_ref[64];
          aot_emit_loc_raw(arg_ref, sizeof(arg_ref), ev.args[i]);
          fprintf(f, "  AotHotRes arg_%u = FE_%u(%s, env, env_len, depth);\n", i, id, arg_ref);
        }
        fprintf(f, "  if (!arg_%u.ok) {\n", i);
        fprintf(f, "    Term call = term_new_ref(%u);\n", ev.ext);
        for (u16 j = 0; j < i; j++) {
          fprintf(f, "    call = term_new_app(call, call_args[%u]);\n", j);
        }
        fprintf(f, "    call = term_new_app(call, arg_%u.term);\n", i);
        for (u16 j = i + 1; j < ev.arg_len; j++) {
          char rem_ref[64];
          aot_emit_loc_raw(rem_ref, sizeof(rem_ref), ev.args[j]);
          fprintf(f, "    call = term_new_app(call, aot_fallback_alo(%s, env_len, env_len == 0 ? NULL : env));\n", rem_ref);
        }
        fprintf(f, "    return aot_hot_fail(call);\n");
        fprintf(f, "  }\n");
        fprintf(f, "  call_args[%u] = arg_%u.term;\n", i, i);
      }
      if (ev.ext == id) {
        fprintf(f, "  return FH_%u(%u, call_args, depth + 1);\n", id, ev.arg_len);
      } else {
        fprintf(f, "  return aot_hot_apply_ref(%u, %u, call_args, depth + 1);\n", ev.ext, ev.arg_len);
      }
      break;
    }
    default: {
      fprintf(f, "  return aot_hot_fail_loc_env(%s, env_len, env);\n", loc_ref);
      break;
    }
  }

  fprintf(f, "}\n\n");
}

// Emits one FE dispatcher switch case.
fn void aot_emit_eval_dispatch_case(FILE *f, u32 id, const AotPlan *plan, u32 width, u32 idx, AotEval ev) {
  char fn_name[64];
  char loc_ref[64];
  aot_emit_eval_name(fn_name, sizeof(fn_name), id, idx);
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, ev.loc);
  fprintf(f, "      case %s: {\n", loc_ref);
  fprintf(f, "        return %s(env, env_len, depth);\n", fn_name);
  fprintf(f, "      }\n");
}

// Builds one numeric eval-node helper name.
fn void aot_emit_num_name(char *out, u32 out_cap, u32 id, u32 idx) {
  snprintf(out, out_cap, "NE_%u_%u", id, idx);
}

// Emits one direct call to a numeric helper or dispatcher when missing.
fn void aot_emit_num_call(FILE *f, u32 id, const AotPlan *plan, u64 loc, const char *dst) {
  int ev_idx = aot_emit_find_eval(plan, loc);
  if (ev_idx >= 0) {
    char fn_name[64];
    aot_emit_num_name(fn_name, sizeof(fn_name), id, (u32)ev_idx);
    fprintf(f, "  AotNumRes %s = %s(env, env_len, depth);\n", dst, fn_name);
    return;
  }

  char loc_ref[64];
  aot_emit_loc_raw(loc_ref, sizeof(loc_ref), loc);
  fprintf(f, "  AotNumRes %s = NE_%u(%s, env, env_len, depth);\n", dst, id, loc_ref);
}

// Emits one numeric eval-node helper body.
fn void aot_emit_num_node(FILE *f, u32 id, const AotPlan *plan, u32 idx, AotEval ev, int self_fast1, int num_trust) {
  char fn_name[64];
  aot_emit_num_name(fn_name, sizeof(fn_name), id, idx);

  fprintf(f, "static inline __attribute__((always_inline)) AotNumRes %s(u32 *env, u16 env_len, u32 depth) {\n", fn_name);
  fprintf(f, "  (void)depth;\n");

  switch (ev.kind) {
    case AOT_EVAL_LIT: {
      fprintf(f, "  return aot_num_ok(%uU);\n", (u32)term_val(ev.lit));
      break;
    }
    case AOT_EVAL_BIND: {
      if (!num_trust) {
        fprintf(f, "  if (%u == 0 || %u > env_len) {\n", ev.ext, ev.ext);
        fprintf(f, "    return aot_num_fail();\n");
        fprintf(f, "  }\n");
      }
      fprintf(f, "  return aot_num_ok(env[env_len - %u]);\n", ev.ext);
      break;
    }
    case AOT_EVAL_OP2: {
      aot_emit_num_call(f, id, plan, ev.a, "lhs");
      if (!num_trust) {
        fprintf(f, "  if (!lhs.ok) {\n");
        fprintf(f, "    return aot_num_fail();\n");
        fprintf(f, "  }\n");
      }
      aot_emit_num_call(f, id, plan, ev.b, "rhs");
      if (!num_trust) {
        fprintf(f, "  if (!rhs.ok) {\n");
        fprintf(f, "    return aot_num_fail();\n");
        fprintf(f, "  }\n");
      }
      switch ((u16)ev.ext) {
        case OP_ADD: fprintf(f, "  return aot_num_ok(lhs.val + rhs.val);\n"); break;
        case OP_SUB: fprintf(f, "  return aot_num_ok(lhs.val - rhs.val);\n"); break;
        case OP_MUL: fprintf(f, "  return aot_num_ok(lhs.val * rhs.val);\n"); break;
        case OP_DIV: fprintf(f, "  return aot_num_ok(rhs.val != 0 ? lhs.val / rhs.val : 0U);\n"); break;
        case OP_MOD: fprintf(f, "  return aot_num_ok(rhs.val != 0 ? lhs.val %% rhs.val : 0U);\n"); break;
        case OP_AND: fprintf(f, "  return aot_num_ok(lhs.val & rhs.val);\n"); break;
        case OP_OR:  fprintf(f, "  return aot_num_ok(lhs.val | rhs.val);\n"); break;
        case OP_XOR: fprintf(f, "  return aot_num_ok(lhs.val ^ rhs.val);\n"); break;
        case OP_LSH: fprintf(f, "  return aot_num_ok(lhs.val << rhs.val);\n"); break;
        case OP_RSH: fprintf(f, "  return aot_num_ok(lhs.val >> rhs.val);\n"); break;
        case OP_NOT: fprintf(f, "  return aot_num_ok(~rhs.val);\n"); break;
        case OP_EQ:  fprintf(f, "  return aot_num_ok(lhs.val == rhs.val ? 1U : 0U);\n"); break;
        case OP_NE:  fprintf(f, "  return aot_num_ok(lhs.val != rhs.val ? 1U : 0U);\n"); break;
        case OP_LT:  fprintf(f, "  return aot_num_ok(lhs.val <  rhs.val ? 1U : 0U);\n"); break;
        case OP_LE:  fprintf(f, "  return aot_num_ok(lhs.val <= rhs.val ? 1U : 0U);\n"); break;
        case OP_GT:  fprintf(f, "  return aot_num_ok(lhs.val >  rhs.val ? 1U : 0U);\n"); break;
        case OP_GE:  fprintf(f, "  return aot_num_ok(lhs.val >= rhs.val ? 1U : 0U);\n"); break;
        default:     fprintf(f, "  return aot_num_fail();\n"); break;
      }
      break;
    }
    case AOT_EVAL_DUP: {
      aot_emit_num_call(f, id, plan, ev.a, "val");
      if (!num_trust) {
        fprintf(f, "  if (!val.ok) {\n");
        fprintf(f, "    return aot_num_fail();\n");
        fprintf(f, "  }\n");
      }
      fprintf(f, "  if (env_len >= AOT_HOT_ENV_CAP) {\n");
      fprintf(f, "    return aot_num_fail();\n");
      fprintf(f, "  }\n");
      fprintf(f, "  env[env_len] = val.val;\n");
      int bod_idx = aot_emit_find_eval(plan, ev.b);
      if (bod_idx >= 0) {
        char bod_name[64];
        aot_emit_num_name(bod_name, sizeof(bod_name), id, (u32)bod_idx);
        fprintf(f, "  return %s(env, env_len + 1, depth);\n", bod_name);
      } else {
        char bod_ref[64];
        aot_emit_loc_raw(bod_ref, sizeof(bod_ref), ev.b);
        fprintf(f, "  return NE_%u(%s, env, env_len + 1, depth);\n", id, bod_ref);
      }
      break;
    }
    case AOT_EVAL_APP_REF: {
      u16 call_cap = ev.arg_len == 0 ? 1 : ev.arg_len;
      fprintf(f, "  u32 call_args[%u];\n", call_cap);
      for (u16 i = 0; i < ev.arg_len; i++) {
        int arg_idx = aot_emit_find_eval(plan, ev.args[i]);
        if (arg_idx >= 0) {
          char arg_name[64];
          aot_emit_num_name(arg_name, sizeof(arg_name), id, (u32)arg_idx);
          fprintf(f, "  AotNumRes arg_%u = %s(env, env_len, depth);\n", i, arg_name);
        } else {
          char arg_ref[64];
          aot_emit_loc_raw(arg_ref, sizeof(arg_ref), ev.args[i]);
          fprintf(f, "  AotNumRes arg_%u = NE_%u(%s, env, env_len, depth);\n", i, id, arg_ref);
        }
        if (!num_trust) {
          fprintf(f, "  if (!arg_%u.ok) {\n", i);
          fprintf(f, "    return aot_num_fail();\n");
          fprintf(f, "  }\n");
        }
        fprintf(f, "  call_args[%u] = arg_%u.val;\n", i, i);
      }
      if (ev.ext == id) {
        if (self_fast1 && ev.arg_len == 1) {
          fprintf(f, "  return NX_%u_1(call_args[0], depth + 1);\n", id);
        } else {
          fprintf(f, "  return NH_%u(%u, call_args, depth + 1);\n", id, ev.arg_len);
        }
      } else {
        fprintf(f, "  return aot_num_apply_ref(%u, %u, call_args, depth + 1);\n", ev.ext, ev.arg_len);
      }
      break;
    }
    default: {
      fprintf(f, "  return aot_num_fail();\n");
      break;
    }
  }

  fprintf(f, "}\n\n");
}

// Emits one numeric dispatcher switch case.
fn void aot_emit_num_dispatch_case(FILE *f, u32 id, const AotPlan *plan, u32 width, u32 idx, AotEval ev) {
  char fn_name[64];
  char loc_ref[64];
  aot_emit_num_name(fn_name, sizeof(fn_name), id, idx);
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, ev.loc);
  fprintf(f, "      case %s: {\n", loc_ref);
  fprintf(f, "        return %s(env, env_len, depth);\n", fn_name);
  fprintf(f, "      }\n");
}

// Builds one zero-check numeric eval helper name.
fn void aot_emit_z_name(char *out, u32 out_cap, u32 id, u32 idx) {
  snprintf(out, out_cap, "ZE_%u_%u", id, idx);
}

// Emits one direct call to a zero-check numeric helper.
fn void aot_emit_z_call(FILE *f, u32 id, const AotPlan *plan, u64 loc, const char *dst) {
  int ev_idx = aot_emit_find_eval(plan, loc);
  if (ev_idx >= 0) {
    char fn_name[64];
    aot_emit_z_name(fn_name, sizeof(fn_name), id, (u32)ev_idx);
    fprintf(f, "  u32 %s = %s(env, env_len);\n", dst, fn_name);
    return;
  }
  fprintf(f, "  u32 %s = 0U;\n", dst);
}

// Emits one zero-check numeric eval helper body.
fn void aot_emit_z_node(FILE *f, u32 id, const AotPlan *plan, u32 idx, AotEval ev) {
  char fn_name[64];
  aot_emit_z_name(fn_name, sizeof(fn_name), id, idx);

  fprintf(f, "static inline __attribute__((always_inline)) u32 %s(u32 *env, u16 env_len) {\n", fn_name);

  switch (ev.kind) {
    case AOT_EVAL_LIT: {
      fprintf(f, "  return %uU;\n", (u32)term_val(ev.lit));
      break;
    }
    case AOT_EVAL_BIND: {
      fprintf(f, "  return env[env_len - %u];\n", ev.ext);
      break;
    }
    case AOT_EVAL_OP2: {
      aot_emit_z_call(f, id, plan, ev.a, "lhs");
      aot_emit_z_call(f, id, plan, ev.b, "rhs");
      switch ((u16)ev.ext) {
        case OP_ADD: fprintf(f, "  return lhs + rhs;\n"); break;
        case OP_SUB: fprintf(f, "  return lhs - rhs;\n"); break;
        case OP_MUL: fprintf(f, "  return lhs * rhs;\n"); break;
        case OP_DIV: fprintf(f, "  return rhs != 0 ? lhs / rhs : 0U;\n"); break;
        case OP_MOD: fprintf(f, "  return rhs != 0 ? lhs %% rhs : 0U;\n"); break;
        case OP_AND: fprintf(f, "  return lhs & rhs;\n"); break;
        case OP_OR:  fprintf(f, "  return lhs | rhs;\n"); break;
        case OP_XOR: fprintf(f, "  return lhs ^ rhs;\n"); break;
        case OP_LSH: fprintf(f, "  return lhs << rhs;\n"); break;
        case OP_RSH: fprintf(f, "  return lhs >> rhs;\n"); break;
        case OP_NOT: fprintf(f, "  return ~rhs;\n"); break;
        case OP_EQ:  fprintf(f, "  return lhs == rhs ? 1U : 0U;\n"); break;
        case OP_NE:  fprintf(f, "  return lhs != rhs ? 1U : 0U;\n"); break;
        case OP_LT:  fprintf(f, "  return lhs <  rhs ? 1U : 0U;\n"); break;
        case OP_LE:  fprintf(f, "  return lhs <= rhs ? 1U : 0U;\n"); break;
        case OP_GT:  fprintf(f, "  return lhs >  rhs ? 1U : 0U;\n"); break;
        case OP_GE:  fprintf(f, "  return lhs >= rhs ? 1U : 0U;\n"); break;
        default:     fprintf(f, "  return 0U;\n"); break;
      }
      break;
    }
    case AOT_EVAL_DUP: {
      aot_emit_z_call(f, id, plan, ev.a, "val");
      fprintf(f, "  env[env_len] = val;\n");
      int bod_idx = aot_emit_find_eval(plan, ev.b);
      if (bod_idx >= 0) {
        char bod_name[64];
        aot_emit_z_name(bod_name, sizeof(bod_name), id, (u32)bod_idx);
        fprintf(f, "  return %s(env, env_len + 1);\n", bod_name);
      } else {
        fprintf(f, "  return 0U;\n");
      }
      break;
    }
    case AOT_EVAL_APP_REF: {
      if (ev.arg_len == 1 && ev.ext == id) {
        int arg_idx = aot_emit_find_eval(plan, ev.args[0]);
        if (arg_idx >= 0) {
          char arg_name[64];
          aot_emit_z_name(arg_name, sizeof(arg_name), id, (u32)arg_idx);
          fprintf(f, "  u32 arg0 = %s(env, env_len);\n", arg_name);
          fprintf(f, "  return ZX_%u_1(arg0);\n", id);
        } else {
          fprintf(f, "  return 0U;\n");
        }
      } else {
        fprintf(f, "  return 0U;\n");
      }
      break;
    }
    default: {
      fprintf(f, "  return 0U;\n");
      break;
    }
  }

  fprintf(f, "}\n\n");
}

// Emits one zero-check dispatcher switch case.
fn void aot_emit_z_dispatch_case(FILE *f, u32 id, const AotPlan *plan, u32 width, u32 idx, AotEval ev) {
  char fn_name[64];
  char loc_ref[64];
  aot_emit_z_name(fn_name, sizeof(fn_name), id, idx);
  aot_emit_loc_ref(loc_ref, sizeof(loc_ref), plan, width, ev.loc);
  fprintf(f, "      case %s: {\n", loc_ref);
  fprintf(f, "        return %s(env, env_len);\n", fn_name);
  fprintf(f, "      }\n");
}

// Emits one specialized unary numeric apply decision tree.
fn void aot_emit_z_tree(FILE *f, u32 id, const AotPlan *plan, u32 width, u64 loc) {
  int st_idx = aot_emit_find_state(plan, loc);
  if (st_idx < 0) {
    int ev_idx = aot_emit_find_eval(plan, loc);
    if (ev_idx < 0) {
      fprintf(f, "  return 0U;\n");
      return;
    }
    char z_name[64];
    aot_emit_z_name(z_name, sizeof(z_name), id, (u32)ev_idx);
    fprintf(f, "  return %s(env, env_len);\n", z_name);
    return;
  }

  AotState st = plan->sts[st_idx];
  switch (st.tag) {
    case SWI: {
      fprintf(f, "  if (a0 == %uU) {\n", st.ext);
      aot_emit_z_tree(f, id, plan, width, st.hit);
      fprintf(f, "  } else {\n");
      aot_emit_z_tree(f, id, plan, width, st.mis);
      fprintf(f, "  }\n");
      return;
    }
    case LAM: {
      fprintf(f, "  env[env_len++] = a0;\n");
      aot_emit_z_tree(f, id, plan, width, st.hit);
      return;
    }
    default: {
      fprintf(f, "  return 0U;\n");
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
  int num_ok  = aot_emit_can_num(&plan);
  int num_fast1 = 0;
  int num_trust = 0;
  for (u32 i = 0; i < plan.st_len; i++) {
    if (plan.sts[i].tag == LAM) {
      env_cap++;
    }
  }
  if (env_cap == 0) {
    env_cap = 1;
  }
  if (num_ok && env_cap == 1 && aot_emit_self_arg1(&plan, id)) {
    num_fast1 = 1;
  }
  if (num_fast1 && aot_emit_self_only(&plan, id)) {
    num_trust = 1;
  }

  // Forward declaration for direct self-recursive hot calls.
  fprintf(f, "static AotHotRes FH_%u(u16 argc, const Term *args, u32 depth);\n\n", id);
  fprintf(f, "static AotNumRes NH_%u(u16 argc, const u32 *args, u32 depth);\n\n", id);
  if (num_fast1) {
    fprintf(f, "static inline __attribute__((always_inline)) AotNumRes NX_%u_1(u32 a0, u32 depth);\n\n", id);
  }

  fprintf(f, "// Compiled stack-entry fast-path for @%s (id %u).\n", name, id);
  fprintf(f, "static Term F_%u(Term *stack, u32 *s_pos, u32 base) {\n", id);
  fprintf(f, "  Term env[%u];\n", env_cap);
  fprintf(f, "  u16  env_len = 0;\n\n");

  aot_emit_consts(f, &plan, width);

  char root_ref[64];
  aot_emit_loc_ref(root_ref, sizeof(root_ref), &plan, width, root);

  if (plan.st_len == 0) {
    fprintf(f, "  return aot_exec_loc(%s, env, env_len, stack, s_pos, base);\n", root_ref);
    fprintf(f, "}\n\n");
  } else {
    fprintf(f, "  u64 at = %s;\n\n", root_ref);
    fprintf(f, "  for (;;) {\n");
    fprintf(f, "    switch (at) {\n");
    for (u32 i = 0; i < plan.st_len; i++) {
      aot_emit_case(f, &plan, width, plan.sts[i]);
    }
    fprintf(f, "      default: {\n");
    fprintf(f, "        return aot_exec_loc(at, env, env_len, stack, s_pos, base);\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n\n");
  }

  fprintf(f, "// Compiled hot-eval path for @%s (id %u).\n", name, id);
  fprintf(f, "static AotHotRes FE_%u(u64 at, Term *env, u16 env_len, u32 depth);\n", id);
  for (u32 i = 0; i < plan.ev_len; i++) {
    char ev_name[64];
    aot_emit_eval_name(ev_name, sizeof(ev_name), id, i);
    fprintf(f, "static AotHotRes %s(Term *env, u16 env_len, u32 depth);\n", ev_name);
  }
  fprintf(f, "\n");

  for (u32 i = 0; i < plan.ev_len; i++) {
    aot_emit_eval_node(f, id, &plan, width, i, plan.evs[i]);
  }

  fprintf(f, "static AotHotRes FE_%u(u64 at, Term *env, u16 env_len, u32 depth) {\n", id);
  aot_emit_consts(f, &plan, width);
  fprintf(f, "  if (depth >= AOT_HOT_MAX_DEPTH) {\n");
  fprintf(f, "    return aot_hot_fail_loc_env(at, env_len, env);\n");
  fprintf(f, "  }\n");
  fprintf(f, "  switch (at) {\n");
  for (u32 i = 0; i < plan.ev_len; i++) {
    aot_emit_eval_dispatch_case(f, id, &plan, width, i, plan.evs[i]);
  }
  fprintf(f, "    default: {\n");
  fprintf(f, "      return aot_hot_fail_loc_env(at, env_len, env);\n");
  fprintf(f, "    }\n");
  fprintf(f, "  }\n");
  fprintf(f, "}\n\n");

  if (num_ok) {
    fprintf(f, "// Compiled numeric hot-eval path for @%s (id %u).\n", name, id);
    fprintf(f, "static inline __attribute__((always_inline)) AotNumRes NE_%u(u64 at, u32 *env, u16 env_len, u32 depth);\n", id);
    for (u32 i = 0; i < plan.ev_len; i++) {
      char num_name[64];
      aot_emit_num_name(num_name, sizeof(num_name), id, i);
      fprintf(f, "static inline __attribute__((always_inline)) AotNumRes %s(u32 *env, u16 env_len, u32 depth);\n", num_name);
    }
    fprintf(f, "\n");

    if (num_trust) {
      fprintf(f, "static inline __attribute__((always_inline)) u32 ZD_%u(u64 at, u32 *env, u16 env_len);\n", id);
      fprintf(f, "static inline __attribute__((always_inline)) u32 ZX_%u_1(u32 a0);\n", id);
      for (u32 i = 0; i < plan.ev_len; i++) {
        char z_name[64];
        aot_emit_z_name(z_name, sizeof(z_name), id, i);
        fprintf(f, "static inline __attribute__((always_inline)) u32 %s(u32 *env, u16 env_len);\n", z_name);
      }
      fprintf(f, "\n");

      for (u32 i = 0; i < plan.ev_len; i++) {
        aot_emit_z_node(f, id, &plan, i, plan.evs[i]);
      }

      fprintf(f, "static inline __attribute__((always_inline)) u32 ZD_%u(u64 at, u32 *env, u16 env_len) {\n", id);
      aot_emit_consts(f, &plan, width);
      fprintf(f, "  switch (at) {\n");
      for (u32 i = 0; i < plan.ev_len; i++) {
        aot_emit_z_dispatch_case(f, id, &plan, width, i, plan.evs[i]);
      }
      fprintf(f, "    default: {\n");
      fprintf(f, "      return 0U;\n");
      fprintf(f, "    }\n");
      fprintf(f, "  }\n");
      fprintf(f, "}\n\n");

      fprintf(f, "static inline __attribute__((always_inline)) u32 ZX_%u_1(u32 a0) {\n", id);
      fprintf(f, "  u32 env[AOT_HOT_ENV_CAP];\n");
      fprintf(f, "  u16 env_len = 0;\n\n");
      aot_emit_z_tree(f, id, &plan, width, root);
      fprintf(f, "}\n\n");
    }

    if (num_fast1) {
      fprintf(f, "static inline __attribute__((always_inline)) AotNumRes NX_%u_1(u32 a0, u32 depth) {\n", id);
      if (!num_trust) {
        fprintf(f, "  if (depth >= AOT_HOT_MAX_DEPTH) {\n");
        fprintf(f, "    return aot_num_fail();\n");
        fprintf(f, "  }\n");
      }
      fprintf(f, "  u32 env[AOT_HOT_ENV_CAP];\n");
      fprintf(f, "  u16 env_len = 0;\n\n");
      aot_emit_consts(f, &plan, width);
      fprintf(f, "  u64 at = %s;\n\n", root_ref);
      fprintf(f, "  for (;;) {\n");
      fprintf(f, "    switch (at) {\n");
      for (u32 i = 0; i < plan.st_len; i++) {
        AotState st = plan.sts[i];
        char loc_ref[64];
        aot_emit_loc_ref(loc_ref, sizeof(loc_ref), &plan, width, st.loc);
        switch (st.tag) {
          case LAM: {
            char hit_ref[64];
            aot_emit_loc_ref(hit_ref, sizeof(hit_ref), &plan, width, st.hit);
            fprintf(f, "      case %s: {\n", loc_ref);
            if (!num_trust) {
              fprintf(f, "        if (env_len >= AOT_HOT_ENV_CAP) {\n");
              fprintf(f, "          return aot_num_fail();\n");
              fprintf(f, "        }\n");
            }
            fprintf(f, "        env[env_len++] = a0;\n");
            fprintf(f, "        at = %s;\n", hit_ref);
            fprintf(f, "        continue;\n");
            fprintf(f, "      }\n");
            break;
          }
          case SWI: {
            char hit_ref[64];
            char mis_ref[64];
            aot_emit_loc_ref(hit_ref, sizeof(hit_ref), &plan, width, st.hit);
            aot_emit_loc_ref(mis_ref, sizeof(mis_ref), &plan, width, st.mis);
            fprintf(f, "      case %s: {\n", loc_ref);
            fprintf(f, "        if (a0 == %uU) {\n", st.ext);
            fprintf(f, "          at = %s;\n", hit_ref);
            fprintf(f, "          continue;\n");
            fprintf(f, "        }\n");
            fprintf(f, "        at = %s;\n", mis_ref);
            fprintf(f, "        continue;\n");
            fprintf(f, "      }\n");
            break;
          }
          default: {
            break;
          }
        }
      }
      fprintf(f, "      default: {\n");
      fprintf(f, "        return NE_%u(at, env, env_len, depth);\n", id);
      fprintf(f, "      }\n");
      fprintf(f, "    }\n");
      fprintf(f, "  }\n");
      fprintf(f, "}\n\n");
    }

    for (u32 i = 0; i < plan.ev_len; i++) {
      aot_emit_num_node(f, id, &plan, i, plan.evs[i], num_fast1, num_trust);
    }

    fprintf(f, "static inline __attribute__((always_inline)) AotNumRes NE_%u(u64 at, u32 *env, u16 env_len, u32 depth) {\n", id);
    aot_emit_consts(f, &plan, width);
    fprintf(f, "  if (depth >= AOT_HOT_MAX_DEPTH) {\n");
    fprintf(f, "    return aot_num_fail();\n");
    fprintf(f, "  }\n");
    fprintf(f, "  switch (at) {\n");
    for (u32 i = 0; i < plan.ev_len; i++) {
      aot_emit_num_dispatch_case(f, id, &plan, width, i, plan.evs[i]);
    }
    fprintf(f, "    default: {\n");
    fprintf(f, "      return aot_num_fail();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n\n");

    if (num_fast1) {
      fprintf(f, "static AotNumRes NH_%u(u16 argc, const u32 *args, u32 depth) {\n", id);
      fprintf(f, "  if (argc != 1) {\n");
      fprintf(f, "    return aot_num_fail();\n");
      fprintf(f, "  }\n");
      fprintf(f, "  return NX_%u_1(args[0], depth);\n", id);
      fprintf(f, "}\n\n");
    } else {
      fprintf(f, "static AotNumRes NH_%u(u16 argc, const u32 *args, u32 depth) {\n", id);
      fprintf(f, "  if (depth >= AOT_HOT_MAX_DEPTH) {\n");
      fprintf(f, "    return aot_num_fail();\n");
      fprintf(f, "  }\n");
      fprintf(f, "  u32 env[AOT_HOT_ENV_CAP];\n");
      fprintf(f, "  u16 env_len = 0;\n");
      fprintf(f, "  u16 i       = 0;\n\n");
      aot_emit_consts(f, &plan, width);
      fprintf(f, "  u64 at = %s;\n\n", root_ref);
      if (plan.st_len == 0) {
        fprintf(f, "  if (i < argc) {\n");
        fprintf(f, "    return aot_num_fail();\n");
        fprintf(f, "  }\n");
        fprintf(f, "  return NE_%u(at, env, env_len, depth);\n", id);
        fprintf(f, "}\n\n");
      } else {
        fprintf(f, "  for (;;) {\n");
        fprintf(f, "    switch (at) {\n");
        for (u32 i = 0; i < plan.st_len; i++) {
          aot_emit_num_case(f, &plan, width, plan.sts[i]);
        }
        fprintf(f, "      default: {\n");
        fprintf(f, "        if (i < argc) {\n");
        fprintf(f, "          return aot_num_fail();\n");
        fprintf(f, "        }\n");
        fprintf(f, "        return NE_%u(at, env, env_len, depth);\n", id);
        fprintf(f, "      }\n");
        fprintf(f, "    }\n");
        fprintf(f, "  }\n");
        fprintf(f, "}\n\n");
      }
    }
  } else {
    fprintf(f, "// Numeric lane disabled for @%s.\n", name);
    fprintf(f, "static AotNumRes NH_%u(u16 argc, const u32 *args, u32 depth) {\n", id);
    fprintf(f, "  (void)argc;\n");
    fprintf(f, "  (void)args;\n");
    fprintf(f, "  (void)depth;\n");
    fprintf(f, "  return aot_num_fail();\n");
    fprintf(f, "}\n\n");
  }

  fprintf(f, "// Compiled hot-apply path for @%s (id %u).\n", name, id);
  fprintf(f, "static AotHotRes FH_%u(u16 argc, const Term *args, u32 depth) {\n", id);
  fprintf(f, "  if (depth >= AOT_HOT_MAX_DEPTH) {\n");
  fprintf(f, "    Term call = aot_hot_reapply(term_new_ref(%u), argc, args, 0);\n", id);
  fprintf(f, "    return aot_hot_fail(call);\n");
  fprintf(f, "  }\n");
  if (num_trust) {
    fprintf(f, "  if (argc == 1 && term_tag(args[0]) == NUM) {\n");
    fprintf(f, "    return aot_hot_ok(term_new_num(ZX_%u_1((u32)term_val(args[0]))));\n", id);
    fprintf(f, "  }\n");
  }
  fprintf(f, "  if (argc <= AOT_HOT_ARG_CAP) {\n");
  fprintf(f, "    u32 num_args[AOT_HOT_ARG_CAP];\n");
  fprintf(f, "    u16 k = 0;\n");
  fprintf(f, "    for (; k < argc; k++) {\n");
  fprintf(f, "      if (term_tag(args[k]) != NUM) {\n");
  fprintf(f, "        break;\n");
  fprintf(f, "      }\n");
  fprintf(f, "      num_args[k] = (u32)term_val(args[k]);\n");
  fprintf(f, "    }\n");
  fprintf(f, "    if (k == argc) {\n");
  fprintf(f, "      AotNumRes num = NH_%u(argc, num_args, depth);\n", id);
  fprintf(f, "      if (num.ok) {\n");
  fprintf(f, "        return aot_hot_ok(term_new_num(num.val));\n");
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  fprintf(f, "  }\n");
      fprintf(f, "  Term env[AOT_HOT_ENV_CAP];\n");
      fprintf(f, "  u16  env_len = 0;\n");
      fprintf(f, "  u16  i       = 0;\n\n");
      aot_emit_consts(f, &plan, width);
      fprintf(f, "  u64  at      = %s;\n\n", root_ref);
  if (plan.st_len == 0) {
    fprintf(f, "  if (i < argc) {\n");
    fprintf(f, "    return aot_hot_fail_apply_env(at, env_len, env, argc, args, i);\n");
    fprintf(f, "  }\n");
    fprintf(f, "  return FE_%u(at, env, env_len, depth);\n", id);
    fprintf(f, "}\n\n");
  } else {
    fprintf(f, "  for (;;) {\n");
    fprintf(f, "    switch (at) {\n");
    for (u32 i = 0; i < plan.st_len; i++) {
      aot_emit_hot_case(f, &plan, width, plan.sts[i]);
    }
    fprintf(f, "      default: {\n");
    fprintf(f, "        if (i < argc) {\n");
    fprintf(f, "          return aot_hot_fail_apply_env(at, env_len, env, argc, args, i);\n");
    fprintf(f, "        }\n");
    fprintf(f, "        return FE_%u(at, env, env_len, depth);\n", id);
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n\n");
  }

  free(plan.sts);
  free(plan.evs);
  free(plan.locs);
}

// Emits registration for all compiled definitions.
fn void aot_emit_register(FILE *f) {
  fprintf(f, "// Registers generated fast paths into the runtime tables.\n");
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
    fprintf(f, "  AOT_HOT_FNS[%u] = FH_%u;\n", id, id);
    fprintf(f, "  AOT_NUM_FNS[%u] = NH_%u;\n", id, id);
  }
  fprintf(f, "}\n\n");
}

// Emits the generated static FFI-load table used by standalone AOT programs.
fn void aot_emit_ffi_table(FILE *f, const AotBuildCfg *cfg) {
  u32 ffi_len = cfg ? cfg->ffi_len : 0;
  u32 ffi_cap = ffi_len == 0 ? 1 : ffi_len;

  fprintf(f, "static const RuntimeFfiLoad AOT_FFI_LOADS[%u] = {\n", ffi_cap);
  if (ffi_len == 0) {
    fprintf(f, "  { .is_dir = 0, .path = NULL },\n");
  } else {
    for (u32 i = 0; i < ffi_len; i++) {
      fprintf(f, "  { .is_dir = %d, .path = ", cfg->ffi[i].is_dir);
      aot_emit_c_string_token(f, cfg->ffi[i].path ? cfg->ffi[i].path : "");
      fprintf(f, " },\n");
    }
  }
  fprintf(f, "};\n");
  fprintf(f, "static const u32 AOT_FFI_LEN = %u;\n\n", ffi_len);
}

// Emits the standalone entrypoint using shared runtime helper functions.
fn void aot_emit_entry_main(FILE *f, const AotBuildCfg *cfg) {
  u32 threads = (cfg && cfg->threads > 0) ? cfg->threads : 1;
  int debug   = cfg ? cfg->debug : 0;

  RuntimeEvalCfg eval_cfg = {
    .do_collapse   = 0,
    .collapse_limit = -1,
    .stats         = 0,
    .silent        = 0,
    .step_by_step  = 0,
  };

  if (cfg != NULL) {
    eval_cfg = cfg->eval;
  }

  fprintf(f, "int main(void) {\n");
  fprintf(f, "  runtime_init(%u, %d, %d, %d);\n", threads, debug, eval_cfg.silent, eval_cfg.step_by_step);
  fprintf(f, "  runtime_load_ffi(AOT_FFI_LOADS, AOT_FFI_LEN, 0);\n");
  fprintf(f, "\n");
  fprintf(f, "  u32 main_id = 0;\n");
  fprintf(f, "  if (!runtime_prepare_text(&main_id, AOT_SOURCE_PATH, AOT_SOURCE_TEXT)) {\n");
  fprintf(f, "    runtime_free();\n");
  fprintf(f, "    return 1;\n");
  fprintf(f, "  }\n");
  fprintf(f, "\n");
  fprintf(f, "  aot_register_generated();\n");
  fprintf(f, "  RuntimeEvalCfg eval_cfg = {\n");
  fprintf(f, "    .do_collapse = %d,\n", eval_cfg.do_collapse);
  fprintf(f, "    .collapse_limit = %d,\n", eval_cfg.collapse_limit);
  fprintf(f, "    .stats = %d,\n", eval_cfg.stats);
  fprintf(f, "    .silent = %d,\n", eval_cfg.silent);
  fprintf(f, "    .step_by_step = %d,\n", eval_cfg.step_by_step);
  fprintf(f, "  };\n");
  fprintf(f, "  runtime_eval_main(main_id, &eval_cfg);\n");
  fprintf(f, "  runtime_free();\n");
  fprintf(f, "  return 0;\n");
  fprintf(f, "}\n");
}

// Emits the full standalone AOT C program.
fn void aot_emit_to_file(FILE *f, const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
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
  aot_emit_ffi_table(f, cfg);

  for (u32 id = 0; id < TABLE.len; id++) {
    if (BOOK[id] == 0) {
      continue;
    }
    aot_emit_def(f, id);
  }

  aot_emit_register(f);
  aot_emit_entry_main(f, cfg);
}

// Emits the full standalone AOT C program to a file path.
fn void aot_emit(const char *c_path, const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  FILE *f = fopen(c_path, "w");
  if (f == NULL) {
    fprintf(stderr, "ERROR: failed to open AOT output '%s'\n", c_path);
    exit(1);
  }

  aot_emit_to_file(f, runtime_path, src_path, src_text, cfg);
  fclose(f);
}

// Emits the full standalone AOT C program to stdout.
fn void aot_emit_stdout(const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  aot_emit_to_file(stdout, runtime_path, src_path, src_text, cfg);
}
