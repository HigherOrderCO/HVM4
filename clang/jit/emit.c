// JIT Module: C Emitter
// ---------------------
// Generates readable C fast paths from static book terms. It compiles only
// APP-LAM and APP-MAT (CTR/NUM) interactions and emits ALO fallback everywhere
// else, preserving runtime behavior.

fn void sys_error(const char *msg);
fn char *table_get(u32 id);
fn void print_term_ex(FILE *f, Term term);

// One compiled fast-path head (LAM, MAT, or SWI) with its next static locations.
typedef struct {
  u64 loc;
  u8  tag;
  u16 ext;
  u64 nxt0;
  u64 nxt1;
} JitNode;

// Returns the index of a state by static location, or -1 if missing.
fn int jit_emit_find_node(JitNode *nodes, u32 len, u64 loc) {
  for (u32 i = 0; i < len; i++) {
    if (nodes[i].loc == loc) {
      return (int)i;
    }
  }
  return -1;
}

// Appends one state to the list, growing storage when needed.
fn void jit_emit_push_node(JitNode **nodes, u32 *len, u32 *cap, JitNode node) {
  if (*len >= *cap) {
    u32 new_cap = *cap == 0 ? 16 : (*cap * 2);
    JitNode *new_nodes = realloc(*nodes, new_cap * sizeof(JitNode));
    if (new_nodes == NULL) {
      sys_error("JIT node allocation failed");
    }
    *nodes = new_nodes;
    *cap   = new_cap;
  }
  (*nodes)[(*len)++] = node;
}

// Collects reachable LAM/MAT/SWI heads from the root static location.
fn void jit_emit_collect(JitNode **nodes, u32 *len, u32 *cap, u64 loc) {
  if (jit_emit_find_node(*nodes, *len, loc) >= 0) {
    return;
  }

  Term t   = heap_read(loc);
  u8   tag = term_tag(t);

  switch (tag) {
    case LAM: {
      u64 nxt = term_val(t);
      jit_emit_push_node(nodes, len, cap, (JitNode){
        .loc  = loc,
        .tag  = tag,
        .ext  = term_ext(t),
        .nxt0 = nxt,
        .nxt1 = 0,
      });
      jit_emit_collect(nodes, len, cap, nxt);
      return;
    }
    case MAT:
    case SWI: {
      u64 mat_loc = term_val(t);
      jit_emit_push_node(nodes, len, cap, (JitNode){
        .loc  = loc,
        .tag  = tag,
        .ext  = term_ext(t),
        .nxt0 = mat_loc + 0,
        .nxt1 = mat_loc + 1,
      });
      jit_emit_collect(nodes, len, cap, mat_loc + 0);
      jit_emit_collect(nodes, len, cap, mat_loc + 1);
      return;
    }
    default: {
      return;
    }
  }
}

// Computes an upper bound for captured APP-LAM arguments.
fn u32 jit_emit_env_cap(JitNode *nodes, u32 len) {
  u32 cap = 0;
  for (u32 i = 0; i < len; i++) {
    if (nodes[i].tag == LAM) {
      cap = cap + 1;
    }
  }
  return cap == 0 ? 1 : cap;
}

// Returns the index of a location constant, or -1 if missing.
fn int jit_emit_find_loc(u64 *locs, u32 len, u64 loc) {
  for (u32 i = 0; i < len; i++) {
    if (locs[i] == loc) {
      return (int)i;
    }
  }
  return -1;
}

// Appends one unique static location to the constants list.
fn void jit_emit_push_loc(u64 **locs, u32 *len, u32 *cap, u64 loc) {
  if (jit_emit_find_loc(*locs, *len, loc) >= 0) {
    return;
  }
  if (*len >= *cap) {
    u32  new_cap  = *cap == 0 ? 16 : (*cap * 2);
    u64 *new_locs = realloc(*locs, new_cap * sizeof(u64));
    if (new_locs == NULL) {
      sys_error("JIT location allocation failed");
    }
    *locs = new_locs;
    *cap  = new_cap;
  }
  (*locs)[(*len)++] = loc;
}

// Collects all static locations referenced by emitted states.
fn void jit_emit_collect_locs(u64 **locs, u32 *len, u32 *cap, u64 root, JitNode *nodes, u32 nlen) {
  jit_emit_push_loc(locs, len, cap, root);
  for (u32 i = 0; i < nlen; i++) {
    jit_emit_push_loc(locs, len, cap, nodes[i].loc);
    jit_emit_push_loc(locs, len, cap, nodes[i].nxt0);
    if (nodes[i].tag == MAT || nodes[i].tag == SWI) {
      jit_emit_push_loc(locs, len, cap, nodes[i].nxt1);
    }
  }
}

// Returns decimal digit width for LOC_ indices (1..9 => 1, 10..99 => 2, ...).
fn u32 jit_emit_loc_width(u32 len) {
  u32 n = len == 0 ? 1 : len;
  u32 w = 1;
  while (n >= 10) {
    n /= 10;
    w++;
  }
  return w;
}

// Builds a symbol name for one LOC_<index> constant.
fn void jit_emit_loc_name(char *out, u32 out_cap, u32 idx, u32 width) {
  snprintf(out, out_cap, "LOC_%0*u", (int)width, idx);
}

// Builds either LOC_<index> or a raw ULL literal for one static location.
fn void jit_emit_loc_ref(char *out, u32 out_cap, u64 *locs, u32 llen, u32 loc_width, u64 loc) {
  int idx = jit_emit_find_loc(locs, llen, loc);
  if (idx >= 0) {
    jit_emit_loc_name(out, out_cap, (u32)idx, loc_width);
    return;
  }
  snprintf(out, out_cap, "%lluULL", (unsigned long long)loc);
}

// Prints a term to a compact single-line string for code comments.
fn void jit_emit_term_line(char *out, u32 out_cap, Term term) {
  if (out_cap == 0) {
    return;
  }

  FILE *tmp = tmpfile();
  if (tmp == NULL) {
    snprintf(out, out_cap, "<term>");
    return;
  }

  print_term_ex(tmp, term);
  fflush(tmp);
  rewind(tmp);

  u32 len = 0;
  u8  spc = 0;

  for (;;) {
    int c = fgetc(tmp);
    if (c == EOF) {
      break;
    }
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
    if (c == ' ') {
      if (spc) {
        continue;
      }
      spc = 1;
    } else {
      spc = 0;
    }
    if (len + 1 >= out_cap) {
      break;
    }
    out[len++] = (char)c;
  }

  fclose(tmp);

  while (len > 0 && out[len - 1] == ' ') {
    len--;
  }

  out[len] = '\0';

  // Drop trailing DUP bind dump emitted by print_term_finish (starts at ";!").
  char *tail = strstr(out, ";!");
  if (tail != NULL) {
    *tail = '\0';
    len = (u32)strlen(out);
    while (len > 0 && out[len - 1] == ' ') {
      len--;
    }
    out[len] = '\0';
  }

  if (len == 0) {
    snprintf(out, out_cap, "<term>");
    return;
  }
}

// Builds a short comment string for one static location constant.
fn void jit_emit_loc_note(char *out, u32 out_cap, u64 loc) {
  jit_emit_term_line(out, out_cap, heap_read(loc));
}

// Emits a transition at the given indentation, or falls back to ALO when needed.
fn void jit_emit_jump(FILE *f, JitNode *nodes, u32 nlen, u64 *locs, u32 llen, u32 loc_width, u64 target_loc, const char *indent) {
  int idx = jit_emit_find_node(nodes, nlen, target_loc);

  char loc_ref[64];
  jit_emit_loc_ref(loc_ref, sizeof(loc_ref), locs, llen, loc_width, target_loc);

  if (idx < 0) {
    fprintf(f, "%sreturn jit_alo(%s, env_len, env);\n", indent, loc_ref);
    return;
  }

  fprintf(f, "%sat = %s;\n", indent, loc_ref);
  fprintf(f, "%scontinue;\n", indent);
}

// Emits all location constants used by the generated function.
fn void jit_emit_consts(FILE *f, u64 *locs, u32 llen, u32 loc_width) {
  fprintf(f, "  // Static locations used for fallback ALO allocation.\n");
  fprintf(f, "  enum {\n");
  for (u32 i = 0; i < llen; i++) {
    char loc_name[64];
    char loc_note[256];
    jit_emit_loc_name(loc_name, sizeof(loc_name), i, loc_width);
    jit_emit_loc_note(loc_note, sizeof(loc_note), locs[i]);
    fprintf(f, "    %s = %lluULL, // %s\n", loc_name, (unsigned long long)locs[i], loc_note);
  }
  fprintf(f, "  };\n\n");
}

// Emits one LAM fast-path state (APP-LAM).
fn void jit_emit_case_lam(FILE *f, JitNode node, JitNode *nodes, u32 nlen, u64 *locs, u32 llen, u32 loc_width) {
  char loc_ref[64];
  jit_emit_loc_ref(loc_ref, sizeof(loc_ref), locs, llen, loc_width, node.loc);

  fprintf(f, "      case %s: { // APP-LAM\n", loc_ref);
  fprintf(f, "        Term arg;\n");
  fprintf(f, "        if (!jit_peek_app_arg(stack, s_pos, base, heap, &arg)) {\n");
  fprintf(f, "          return jit_alo(%s, env_len, env);\n", loc_ref);
  fprintf(f, "        }\n");
  fprintf(f, "        (*s_pos)--;\n");
  fprintf(f, "        env[env_len++] = arg;\n");
  fprintf(f, "        jit_itr(JIT_I_LAM);\n");
  jit_emit_jump(f, nodes, nlen, locs, llen, loc_width, node.nxt0, "        ");
  fprintf(f, "      }\n");
}

// Emits one SWI fast-path state (APP-MAT-NUM).
fn void jit_emit_case_swi(FILE *f, JitNode node, JitNode *nodes, u32 nlen, u64 *locs, u32 llen, u32 loc_width) {
  char loc_ref[64];
  jit_emit_loc_ref(loc_ref, sizeof(loc_ref), locs, llen, loc_width, node.loc);

  fprintf(f, "      case %s: { // APP-MAT-NUM (== %u)\n", loc_ref, node.ext);
  fprintf(f, "        u64 num;\n");
  fprintf(f, "        if (!jit_peek_num_arg(stack, s_pos, base, heap, &num)) {\n");
  fprintf(f, "          return jit_alo(%s, env_len, env);\n", loc_ref);
  fprintf(f, "        }\n");
  fprintf(f, "        if (num == %uULL) {\n", node.ext);
  fprintf(f, "          (*s_pos)--;\n");
  fprintf(f, "          jit_itr(JIT_I_NUM_H);\n");
  jit_emit_jump(f, nodes, nlen, locs, llen, loc_width, node.nxt0, "          ");
  fprintf(f, "        }\n");
  fprintf(f, "        jit_itr(JIT_I_NUM_M);\n");
  jit_emit_jump(f, nodes, nlen, locs, llen, loc_width, node.nxt1, "        ");
  fprintf(f, "      }\n");
}

// Emits one MAT fast-path state (APP-MAT-CTR).
fn void jit_emit_case_mat(FILE *f, JitNode node, JitNode *nodes, u32 nlen, u64 *locs, u32 llen, u32 loc_width) {
  char loc_ref[64];
  jit_emit_loc_ref(loc_ref, sizeof(loc_ref), locs, llen, loc_width, node.loc);

  char *ctr_name = table_get(node.ext);
  if (ctr_name != NULL) {
    fprintf(f, "      case %s: { // APP-MAT-CTR (#%s)\n", loc_ref, ctr_name);
  } else {
    fprintf(f, "      case %s: { // APP-MAT-CTR (id %u)\n", loc_ref, node.ext);
  }

  fprintf(f, "        Term ctr;\n");
  fprintf(f, "        u8   ctr_tag;\n");
  fprintf(f, "        if (!jit_peek_ctr_arg(stack, s_pos, base, heap, &ctr, &ctr_tag)) {\n");
  fprintf(f, "          return jit_alo(%s, env_len, env);\n", loc_ref);
  fprintf(f, "        }\n");
  fprintf(f, "        if (jit_ext(ctr) == %u) {\n", node.ext);
  fprintf(f, "          (*s_pos)--;\n");
  fprintf(f, "          jit_itr(JIT_I_CTR_H);\n");
  fprintf(f, "          jit_push_ctr_apps(stack, s_pos, heap, ctr, ctr_tag);\n");
  jit_emit_jump(f, nodes, nlen, locs, llen, loc_width, node.nxt0, "          ");
  fprintf(f, "        }\n");
  fprintf(f, "        jit_itr(JIT_I_CTR_M);\n");
  jit_emit_jump(f, nodes, nlen, locs, llen, loc_width, node.nxt1, "        ");
  fprintf(f, "      }\n");
}

// Emits a readable C fast-path function for one top-level definition.
fn void jit_emit(const char *c_path, u32 id) {
  FILE *f = fopen(c_path, "w");
  if (f == NULL) {
    fprintf(stderr, "ERROR: failed to open JIT output '%s'\n", c_path);
    exit(1);
  }

  JitNode *nodes = NULL;
  u32      nlen  = 0;
  u32      ncap  = 0;
  u64      root  = BOOK[id];
  jit_emit_collect(&nodes, &nlen, &ncap, root);

  u64 *locs = NULL;
  u32  llen = 0;
  u32  lcap = 0;
  jit_emit_collect_locs(&locs, &llen, &lcap, root, nodes, nlen);

  u32 loc_width = jit_emit_loc_width(llen);

  u32 env_cap = jit_emit_env_cap(nodes, nlen);

  fprintf(f, "// Auto-generated by HVM4 JIT.\n");
  fprintf(f, "// Fast-paths covered: APP-LAM, APP-MAT-CTR, APP-MAT-NUM.\n");
  fprintf(f, "#include <stdint.h>\n");
  fprintf(f, "#include \"runtime.h\"\n\n");

  fprintf(f, "Term hvm4_jit_fn(Term *stack, u32 *s_pos, u32 base) {\n");
  fprintf(f, "  Term *heap = jit_heap();\n");
  fprintf(f, "  Term env[%u];\n", env_cap);
  fprintf(f, "  u16  env_len = 0;\n\n");
  jit_emit_consts(f, locs, llen, loc_width);

  char root_loc_name[64];
  jit_emit_loc_ref(root_loc_name, sizeof(root_loc_name), locs, llen, loc_width, root);

  if (nlen == 0) {
    fprintf(f, "  return jit_alo(%s, env_len, env);\n", root_loc_name);
    fprintf(f, "}\n");
    free(locs);
    free(nodes);
    fclose(f);
    return;
  }

  int root_idx = jit_emit_find_node(nodes, nlen, root);
  if (root_idx < 0) {
    fprintf(f, "  return jit_alo(%s, env_len, env);\n", root_loc_name);
    fprintf(f, "}\n");
    free(locs);
    free(nodes);
    fclose(f);
    return;
  }

  fprintf(f, "  u64 at = %s;\n\n", root_loc_name);

  fprintf(f, "  for (;;) {\n");
  fprintf(f, "    switch (at) {\n");

  for (u32 i = 0; i < nlen; i++) {
    switch (nodes[i].tag) {
      case LAM: {
        jit_emit_case_lam(f, nodes[i], nodes, nlen, locs, llen, loc_width);
        break;
      }
      case MAT: {
        jit_emit_case_mat(f, nodes[i], nodes, nlen, locs, llen, loc_width);
        break;
      }
      case SWI: {
        jit_emit_case_swi(f, nodes[i], nodes, nlen, locs, llen, loc_width);
        break;
      }
      default: {
        break;
      }
    }
  }

  fprintf(f, "      default: {\n");
  fprintf(f, "        return jit_alo(at, env_len, env);\n");
  fprintf(f, "      }\n");
  fprintf(f, "    }\n");
  fprintf(f, "  }\n");
  fprintf(f, "}\n");

  free(locs);
  free(nodes);
  fclose(f);
}
