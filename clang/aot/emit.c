// AOT Module: Program Emitter
// ---------------------------
// Emits standalone C with one direct tree-shaped function per definition.

fn char *table_get(u32 id);

// Emit Options
// ------------

// Controls whether emitted AOT code should include interaction counting calls.
static int AOT_EMIT_ITRS = 1;
static u32 AOT_EMIT_DEOPT_SITE = 0;
static u8  AOT_EMIT_DEOPT_KIND[AOT_DEOPT_SITE_CAP] = {0};
static u64 AOT_EMIT_DEOPT_LOC[AOT_DEOPT_SITE_CAP] = {0};
static u32 AOT_EMIT_DEOPT_AUX[AOT_DEOPT_SITE_CAP] = {0};
static u32 AOT_EMIT_DEF_ID = 0;
static u8  AOT_EMIT_SELF_LOOP = 0;
// Bounds structural expression emission to avoid emitter/code-size blowups.
#define AOT_EMIT_EXPR_STRUCT_DEPTH_CAP 128
#define AOT_EMIT_EXPR_STRUCT_NODE_CAP  4096

// Returns 1 when this AOT build needs interaction counting.
fn int aot_emit_counting(const AotBuildCfg *cfg) {
  if (cfg == NULL) {
    return 1;
  }
  // `-S` is throughput-oriented; keep counting for `-s`/`-D` observability.
  return cfg->eval.stats || cfg->eval.step_by_step;
}

// Emits one `aot_itrs_inc()` line if counting is enabled.
fn void aot_emit_itrs_inc(FILE *f, const char *pad) {
  if (!AOT_EMIT_ITRS) {
    return;
  }
  fprintf(f, "%saot_itrs_inc();\n", pad);
}

// Name Helpers
// ------------

// Builds one unique temporary identifier.
fn void aot_emit_tmp(char *out, u32 out_cap, const char *pre, u32 *next) {
  snprintf(out, out_cap, "%s_%u", pre, *next);
  *next = *next + 1;
}

// Builds one extra-indented padding string.
fn void aot_emit_pad_next(char *out, u32 out_cap, const char *pad) {
  snprintf(out, out_cap, "%s  ", pad);
}

// Builds one C identifier for a compiled definition function.
fn void aot_emit_fun_name(char *out, u32 out_cap, const char *name) {
  if (out_cap < 4) {
    if (out_cap > 0) {
      out[0] = '\0';
    }
    return;
  }

  // Fail fast on truncation risk: truncated C symbol names can collide.
  // Reserve 1 byte for NUL terminator.
  u32 need = 3; // "F_" + at least one body char
  for (u32 i = 0; name[i] != '\0'; i++) {
    need += 1;
  }
  if (need > out_cap) {
    fprintf(stderr, "ERROR: AOT function name for '@%s' exceeds emitter limit (%u chars)\n", name, out_cap - 1);
    exit(1);
  }

  u32 j = 0;
  out[j++] = 'F';
  out[j++] = '_';

  for (u32 i = 0; name[i] != '\0'; i++) {
    u8 c = (u8)name[i];
    u8 az = c >= 'a' && c <= 'z';
    u8 AZ = c >= 'A' && c <= 'Z';
    u8 d9 = c >= '0' && c <= '9';
    char d = (az || AZ || d9) ? (char)c : '_';

    if (j == 2 && d9) {
      if (j + 1 >= out_cap) {
        break;
      }
      out[j++] = '_';
    }

    if (j + 1 >= out_cap) {
      break;
    }
    out[j++] = d;
  }

  if (j == 2) {
    out[j++] = '_';
  }
  out[j] = '\0';
}

// Escape Helpers
// --------------

// Writes one escaped byte as part of a C string literal.
fn void aot_emit_escaped_byte(FILE *f, u8 c) {
  switch (c) {
    case '\\': {
      fputs("\\\\", f);
      return;
    }
    case '"': {
      fputs("\\\"", f);
      return;
    }
    case '\n': {
      fputs("\\n", f);
      return;
    }
    case '\r': {
      fputs("\\r", f);
      return;
    }
    case '\t': {
      fputs("\\t", f);
      return;
    }
    default: {
      break;
    }
  }

  if (c >= 32 && c <= 126) {
    fputc((int)c, f);
    return;
  }

  fprintf(f, "\\%03o", (unsigned)c);
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
  if (ptr[0] == '\0') {
    fprintf(f, "  \"\";\n\n");
    return;
  }

  u32 i = 0;
  while (ptr[i] != 0) {
    fprintf(f, "  \"");
    u32 chunk = 0;
    while (ptr[i] != 0 && chunk < 64) {
      aot_emit_escaped_byte(f, ptr[i]);
      i++;
      chunk++;
    }
    fprintf(f, "\"\n");
  }

  fprintf(f, "  ;\n\n");
}

// Term Helpers
// ------------

// Emits one source-map style comment for the strict WNF position.
fn void aot_emit_wnf_comment(FILE *f, u64 loc, const char *pad) {
  fprintf(f, "%s// wnf ", pad);
  print_term_quoted_ex(f, heap_read(loc), 0);
  fprintf(f, "\n");
}

// Emits one lexical environment bind-list head expression.
fn void aot_emit_env_head(FILE *f, u32 dep) {
  if (dep == 0) {
    fprintf(f, "0ULL");
  } else {
    fprintf(f, "aot_env_head(env_cells, env_locs, %u)", dep);
  }
}

// Stores one static metadata entry for one emitted deopt site.
fn void aot_emit_deopt_site_note(u32 site, u32 kind, u64 loc, u32 aux) {
  if (site >= AOT_DEOPT_SITE_CAP) {
    return;
  }
  AOT_EMIT_DEOPT_KIND[site] = (u8)kind;
  AOT_EMIT_DEOPT_LOC[site]  = loc;
  AOT_EMIT_DEOPT_AUX[site]  = aux;
}

// Emits one ALO expression + one deopt counter site for static `loc`.
fn void aot_emit_alo_expr_meta(FILE *f, u64 loc, u32 dep, u32 reason, u32 kind, u32 aux) {
  u32 site = AOT_EMIT_DEOPT_SITE++;
  aot_emit_deopt_site_note(site, kind, loc, aux);
  fprintf(f, "aot_fallback_alo(%lluULL, %u, ", (unsigned long long)loc, dep);
  aot_emit_env_head(f, dep);
  fprintf(f, ", %u, %u)", reason, site);
}

// Emits one default ALO expression without extra site metadata.
fn void aot_emit_alo_expr(FILE *f, u64 loc, u32 dep, u32 reason) {
  aot_emit_alo_expr_meta(f, loc, dep, reason, AOT_DEOPT_SITE_KIND_NONE, 0);
}

// Emits one deopt return for current location + lexical env.
fn void aot_emit_ret_fallback_loc(FILE *f, u64 loc, u32 dep, u32 reason, const char *pad) {
  fprintf(f, "%sreturn ", pad);
  aot_emit_alo_expr(f, loc, dep, reason);
  fprintf(f, ";\n");
}

// Emits one deopt return with explicit strict-site metadata.
fn void aot_emit_ret_fallback_loc_meta(FILE *f, u64 loc, u32 dep, u32 reason, u32 kind, u32 aux, const char *pad) {
  fprintf(f, "%sreturn ", pad);
  aot_emit_alo_expr_meta(f, loc, dep, reason, kind, aux);
  fprintf(f, ";\n");
}

// Emits one recursive node.
// - `head=1`: consumes APP frames from `stack/*s_pos` and returns from F_<def>.
// - `head=0`: materializes one lazy expression into local `Term <out>`.
fn void aot_emit_node(FILE *f, u64 loc, u32 dep, const char *out, u8 head, const char *pad, u32 *tmp);
fn void aot_emit_node_demanded(FILE *f, u64 loc, u32 dep, const char *out, const char *pad, u32 *tmp);

// Returns 1 when one expression subtree is safe to emit structurally.
fn u8 aot_emit_expr_can_structuralize(u64 loc, u32 depth, u32 *budget) {
  if (depth >= AOT_EMIT_EXPR_STRUCT_DEPTH_CAP) {
    return 0;
  }
  if (*budget == 0) {
    return 0;
  }
  *budget = *budget - 1;

  Term term = heap_read(loc);
  switch (term_tag(term)) {
    case NUM:
    case NAM:
    case ERA:
    case ANY:
    case C00:
    case REF:
    case VAR:
    case BJV:
    case DP0:
    case BJ0:
    case DP1:
    case BJ1: {
      return 1;
    }
    case C01 ... C16: {
      u32 ari = (u32)(term_tag(term) - C00);
      u64 ctr_loc = term_val(term);
      for (u32 i = 0; i < ari; i++) {
        if (!aot_emit_expr_can_structuralize(ctr_loc + i, depth + 1, budget)) {
          return 0;
        }
      }
      return 1;
    }
    case OP2: {
      u64 op2_loc = term_val(term);
      return aot_emit_expr_can_structuralize(op2_loc + 0, depth + 1, budget)
          && aot_emit_expr_can_structuralize(op2_loc + 1, depth + 1, budget);
    }
    case APP: {
      u64 app_loc = term_val(term);
      return aot_emit_expr_can_structuralize(app_loc + 0, depth + 1, budget)
          && aot_emit_expr_can_structuralize(app_loc + 1, depth + 1, budget);
    }
    case DUP: {
      u64 dup_loc = term_val(term);
      return aot_emit_expr_can_structuralize(dup_loc + 0, depth + 1, budget)
          && aot_emit_expr_can_structuralize(dup_loc + 1, depth + 1, budget);
    }
    default: {
      return 0;
    }
  }
}

// Returns 1 when one expression should use structural emission.
fn u8 aot_emit_expr_structural(u64 loc) {
  u32 budget = AOT_EMIT_EXPR_STRUCT_NODE_CAP;
  return aot_emit_expr_can_structuralize(loc, 0, &budget);
}

// Returns 1 when one head-position subtree can self-tail-call this definition.
fn u8 aot_emit_has_self_tail_ref(u64 loc, u32 def_id) {
  Term term = heap_read(loc);
  switch (term_tag(term)) {
    case APP: {
      return aot_emit_has_self_tail_ref(term_val(term) + 0, def_id);
    }
    case LAM: {
      return aot_emit_has_self_tail_ref(term_val(term), def_id);
    }
    case DUP: {
      return aot_emit_has_self_tail_ref(term_val(term) + 1, def_id);
    }
    case SWI:
    case MAT: {
      u64 mat_loc = term_val(term);
      return aot_emit_has_self_tail_ref(mat_loc + 0, def_id)
          || aot_emit_has_self_tail_ref(mat_loc + 1, def_id);
    }
    case REF: {
      return term_ext(term) == def_id;
    }
    default: {
      return 0;
    }
  }
}

// Collects APP spine args (outer-to-inner) when head is one static REF.
fn u8 aot_emit_collect_ref_spine(u64 loc, u32 *ref_id, u64 *arg_locs, u32 *arg_len) {
  u32 len = 0;
  u64 cur = loc;

  while (1) {
    Term term = heap_read(cur);
    switch (term_tag(term)) {
      case APP: {
        if (len >= AOT_ARG_CAP) {
          return 0;
        }
        u64 app_loc = term_val(term);
        arg_locs[len] = app_loc + 1;
        len = len + 1;
        cur = app_loc + 0;
        continue;
      }
      case REF: {
        *ref_id  = term_ext(term);
        *arg_len = len;
        return len != 0;
      }
      default: {
        return 0;
      }
    }
  }
}

// Emits one demanded APP(ref, ...) fast path using a direct compiled call.
// Falls back to regular structural emission when step-limits are active.
fn u8 aot_emit_try_demanded_ref_call(FILE *f, u64 loc, u32 dep, const char *out, const char *pad, u32 *tmp) {
  u64 arg_locs[AOT_ARG_CAP] = {0};
  u32 ref_id = 0;
  u32 arg_len = 0;

  if (!aot_emit_collect_ref_spine(loc, &ref_id, arg_locs, &arg_len)) {
    return 0;
  }

  char pad1[128];
  aot_emit_pad_next(pad1, sizeof(pad1), pad);

  char args_n[32];
  char pos_n[32];
  char base_n[32];
  char head_n[32];
  char arg_n[32];
  char fb_n[32];
  aot_emit_tmp(args_n, sizeof(args_n), "call_args", tmp);
  aot_emit_tmp(pos_n,  sizeof(pos_n),  "call_pos",  tmp);
  aot_emit_tmp(base_n, sizeof(base_n), "call_base", tmp);
  aot_emit_tmp(head_n, sizeof(head_n), "call_out",  tmp);
  aot_emit_tmp(fb_n,   sizeof(fb_n),   "fallback",  tmp);

  fprintf(f, "%sTerm %s;\n", pad, out);
  fprintf(f, "%sif (STEPS_ITRS_LIM == 0) {\n", pad);
  fprintf(f, "%sTerm %s[AOT_ARG_CAP] = {0};\n", pad1, args_n);
  fprintf(f, "%su32 %s = %u;\n", pad1, pos_n, arg_len);
  for (u32 i = arg_len; i > 0; i--) {
    u32 idx = i - 1;
    aot_emit_tmp(arg_n, sizeof(arg_n), "arg", tmp);
    if (aot_emit_expr_structural(arg_locs[idx])) {
      aot_emit_node(f, arg_locs[idx], dep, arg_n, 0, pad1, tmp);
    } else {
      fprintf(f, "%sTerm %s = ", pad1, arg_n);
      aot_emit_alo_expr(f, arg_locs[idx], dep, AOT_DEOPT_EXPR_APP_ARG_CAPTURE);
      fprintf(f, ";\n");
    }
    fprintf(f, "%s%s[%u] = %s;\n", pad1, args_n, idx, arg_n);
  }
  fprintf(f, "%su32 %s = *s_pos;\n", pad1, base_n);
  fprintf(f, "%sTerm %s = aot_call_ref(%u, stack, s_pos, %s, %s, &%s);\n", pad1, head_n, ref_id, base_n, args_n, pos_n);
  fprintf(f, "%s%s = %s;\n", pad1, out, head_n);
  fprintf(f, "%sfor (u32 j = %s; j > 0; j--) {\n", pad1, pos_n);
  fprintf(f, "%s  %s = term_new_app(%s, %s[j - 1]);\n", pad1, out, out, args_n);
  fprintf(f, "%s}\n", pad1);
  fprintf(f, "%s} else {\n", pad);
  aot_emit_node(f, loc, dep, fb_n, 0, pad1, tmp);
  fprintf(f, "%s%s = %s;\n", pad1, out, fb_n);
  fprintf(f, "%s}\n", pad);
  return 1;
}

// Emits one demanded expression with strict-safe hot-path lowering.
fn void aot_emit_node_demanded(FILE *f, u64 loc, u32 dep, const char *out, const char *pad, u32 *tmp) {
  Term term = heap_read(loc);
  switch (term_tag(term)) {
    case APP: {
      if (aot_emit_try_demanded_ref_call(f, loc, dep, out, pad, tmp)) {
        return;
      }
      aot_emit_node(f, loc, dep, out, 0, pad, tmp);
      return;
    }
    case OP2: {
      u32 opr = term_ext(term);
      u64 op2_loc = term_val(term);
      char lhs_n[32];
      char rhs_n[32];
      char fb_n[32];
      char pad1[128];
      char pad2[128];
      char pad3[128];
      aot_emit_tmp(lhs_n, sizeof(lhs_n), "lhs", tmp);
      aot_emit_tmp(rhs_n, sizeof(rhs_n), "rhs", tmp);
      aot_emit_tmp(fb_n, sizeof(fb_n), "fallback", tmp);
      aot_emit_pad_next(pad1, sizeof(pad1), pad);
      aot_emit_pad_next(pad2, sizeof(pad2), pad1);
      aot_emit_pad_next(pad3, sizeof(pad3), pad2);

      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%sif (STEPS_ITRS_LIM == 0) {\n", pad);
      aot_emit_node_demanded(f, op2_loc + 0, dep, lhs_n, pad1, tmp);
      fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad1, lhs_n, lhs_n);
      fprintf(f, "%sif (term_tag(%s) == ERA) {\n", pad1, lhs_n);
      fprintf(f, "%s%s = wnf_op2_era();\n", pad2, out);
      fprintf(f, "%s} else if (term_tag(%s) == NUM) {\n", pad1, lhs_n);
      aot_emit_node_demanded(f, op2_loc + 1, dep, rhs_n, pad2, tmp);
      fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad2, rhs_n, rhs_n);
      fprintf(f, "%sif (term_tag(%s) == ERA) {\n", pad2, rhs_n);
      fprintf(f, "%s%s = wnf_op2_num_era();\n", pad3, out);
      fprintf(f, "%s} else if (term_tag(%s) == NUM) {\n", pad2, rhs_n);
      fprintf(f, "%s%s = wnf_op2_num_num_raw(%u, (u32)term_val(%s), (u32)term_val(%s));\n", pad3, out, opr, lhs_n, rhs_n);
      fprintf(f, "%s} else {\n", pad2);
      fprintf(f, "%s%s = term_new_op2(%u, %s, %s);\n", pad3, out, opr, lhs_n, rhs_n);
      fprintf(f, "%s}\n", pad2);
      fprintf(f, "%s} else {\n", pad1);
      fprintf(f, "%s%s = term_new_op2(%u, %s, ", pad2, out, opr, lhs_n);
      aot_emit_alo_expr(f, op2_loc + 1, dep, AOT_DEOPT_EXPR_OP2_RHS_CAPTURE);
      fprintf(f, ");\n");
      fprintf(f, "%s}\n", pad1);
      fprintf(f, "%s} else {\n", pad);
      aot_emit_node(f, loc, dep, fb_n, 0, pad1, tmp);
      fprintf(f, "%s%s = %s;\n", pad1, out, fb_n);
      fprintf(f, "%s}\n", pad);
      return;
    }
    default: {
      aot_emit_node(f, loc, dep, out, 0, pad, tmp);
      return;
    }
  }
}

// Returns current head term without consuming pending APP frames.
fn void aot_emit_ret_head(FILE *f, u64 loc, u32 dep, const char *pad, u32 *tmp) {
  char head_n[32];

  aot_emit_tmp(head_n, sizeof(head_n), "head", tmp);

  aot_emit_node(f, loc, dep, head_n, 0, pad, tmp);
  fprintf(f, "%sreturn %s;\n", pad, head_n);
}

// Emit Core
// ---------

fn void aot_emit_node(FILE *f, u64 loc, u32 dep, const char *out, u8 head, const char *pad, u32 *tmp) {
  Term term = heap_read(loc);
  u8   tag  = term_tag(term);

  char pad1[128];
  char pad2[128];
  aot_emit_pad_next(pad1, sizeof(pad1), pad);
  aot_emit_pad_next(pad2, sizeof(pad2), pad1);
  
  if (head) {
    aot_emit_wnf_comment(f, loc, pad);
    switch (tag) {
      case APP: {
        u64 app_loc = term_val(term);
        u64 fun_loc = app_loc + 0;
        u64 arg_loc = app_loc + 1;
        char arg_n[32];
        aot_emit_tmp(arg_n, sizeof(arg_n), "arg", tmp);

        fprintf(f, "%sif (*a_pos >= AOT_ARG_CAP) {\n", pad);
        aot_emit_ret_fallback_loc(f, loc, dep, AOT_DEOPT_HEAD_APP_ARG_CAP, pad1);
        fprintf(f, "%s}\n", pad);
        aot_emit_node(f, arg_loc, dep, arg_n, 0, pad, tmp);
        fprintf(f, "%sa_args[*a_pos] = %s;\n", pad, arg_n);
        fprintf(f, "%s(*a_pos)++;\n", pad);

        aot_emit_node(f, fun_loc, dep, out, 1, pad, tmp);
        return;
      }

      case LAM: {
        u32 lam_ext = term_ext(term);
        u8  lam_era = (lam_ext & LAM_ERA_MASK) != 0;
        char frm[32];
        char app[32];
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        if (!lam_era) {
          aot_emit_tmp(app, sizeof(app), "app", tmp);
        }

        if (dep >= AOT_ENV_CAP) {
          aot_emit_ret_fallback_loc(f, loc, dep, AOT_DEOPT_HEAD_LAM_ENV_CAP, pad);
          return;
        }
        if (!lam_era) {
          fprintf(f, "%sTerm x%u;\n", pad, dep);
        }
        fprintf(f, "%sif (*a_pos > 0) {\n", pad);
        fprintf(f, "%s(*a_pos)--;\n", pad1);
        if (lam_era) {
          fprintf(f, "%senv_cells[%u] = term_sub_set(term_new_era(), 1);\n", pad1, dep);
          fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad1, dep);
        } else {
          fprintf(f, "%sx%u = a_args[*a_pos];\n", pad1, dep);
          fprintf(f, "%senv_cells[%u] = term_sub_set(x%u, 1);\n", pad1, dep, dep);
          fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad1, dep);
        }
        fprintf(f, "%s} else {\n", pad);
        fprintf(f, "%sif (*s_pos <= base) {\n", pad1);
        aot_emit_ret_head(f, loc, dep, pad2, tmp);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1];\n", pad1, frm);
        fprintf(f, "%sif (term_tag(%s) != APP) {\n", pad1, frm);
        aot_emit_ret_head(f, loc, dep, pad2, tmp);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%s(*s_pos)--;\n", pad1);
        if (lam_era) {
          fprintf(f, "%senv_cells[%u] = term_sub_set(term_new_era(), 1);\n", pad1, dep);
          fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad1, dep);
        } else {
          fprintf(f, "%su64 %s = term_val(%s);\n", pad1, app, frm);
          fprintf(f, "%sx%u = heap_read(%s + 1);\n", pad1, dep, app);
          fprintf(f, "%senv_cells[%u] = term_sub_set(x%u, 1);\n", pad1, dep, dep);
          fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad1, dep);
        }
        fprintf(f, "%s}\n", pad);
        aot_emit_itrs_inc(f, pad);
        aot_emit_node(f, term_val(term), dep + 1, out, 1, pad, tmp);
        return;
      }

      case DUP: {
        if (dep >= AOT_ENV_CAP) {
          aot_emit_ret_fallback_loc(f, loc, dep, AOT_DEOPT_HEAD_DUP_ENV_CAP, pad);
          return;
        }

        u64 dup_loc = term_val(term);
        char val_n[32];
        aot_emit_tmp(val_n, sizeof(val_n), "val", tmp);

        aot_emit_node(f, dup_loc + 0, dep, val_n, 0, pad, tmp);
        fprintf(f, "%sTerm x%u = %s;\n", pad, dep, val_n);
        fprintf(f, "%sif (aot_is_copy_free(x%u)) {\n", pad, dep);
        fprintf(f, "%senv_cells[%u] = term_sub_set(x%u, 1);\n", pad1, dep, dep);
        fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad1, dep);
        fprintf(f, "%s} else {\n", pad);
        fprintf(f, "%su64 e%u = aot_alloc(2, AOT_ALLOC_DUP_CELL);\n", pad1, dep);
        fprintf(f, "%sheap_set(e%u + 1, term_new(0, NUM, 0, 0ULL));\n", pad1, dep);
        fprintf(f, "%sheap_set(e%u + 0, x%u);\n", pad1, dep, dep);
        fprintf(f, "%senv_locs[%u] = e%u;\n", pad1, dep, dep);
        fprintf(f, "%s}\n", pad);
        aot_emit_node(f, dup_loc + 1, dep + 1, out, 1, pad, tmp);
        return;
      }

      case SWI: {
        u64 mat_loc = term_val(term);
        char frm[32];
        char app[32];
        char arg[32];
        char src[32];
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        aot_emit_tmp(app, sizeof(app), "app", tmp);
        aot_emit_tmp(arg, sizeof(arg), "arg", tmp);
        aot_emit_tmp(src, sizeof(src), "src", tmp);

        fprintf(f, "%su8 %s = (*a_pos > 0);\n", pad, src);
        fprintf(f, "%sTerm %s;\n", pad, arg);
        fprintf(f, "%sif (%s) {\n", pad, src);
        fprintf(f, "%s%s = a_args[*a_pos - 1];\n", pad1, arg);
        fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad1, arg, arg);
        fprintf(f, "%sa_args[*a_pos - 1] = %s;\n", pad1, arg);
        fprintf(f, "%s} else {\n", pad);
        fprintf(f, "%sif (*s_pos <= base) {\n", pad1);
        aot_emit_ret_head(f, loc, dep, pad2, tmp);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1];\n", pad1, frm);
        fprintf(f, "%sif (term_tag(%s) != APP) {\n", pad1, frm);
        aot_emit_ret_head(f, loc, dep, pad2, tmp);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad1, app, frm);
        fprintf(f, "%s%s = heap_read(%s + 1);\n", pad1, arg, app);
        fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad1, arg, arg);
        fprintf(f, "%sheap_set_rel(%s + 1, %s);\n", pad1, app, arg);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sif (term_tag(%s) != NUM) {\n", pad, arg);
        aot_emit_ret_fallback_loc_meta(f, loc, dep, AOT_DEOPT_EXPR_UNSUPPORTED_TAG, AOT_DEOPT_SITE_KIND_HEAD_SWI_NON_NUM, term_ext(term), pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sif (term_val(%s) == %uULL) {\n", pad, arg, term_ext(term));
        fprintf(f, "%sif (%s) {\n", pad1, src);
        fprintf(f, "%s(*a_pos)--;\n", pad2);
        fprintf(f, "%s} else {\n", pad1);
        fprintf(f, "%s(*s_pos)--;\n", pad2);
        fprintf(f, "%s}\n", pad1);
        aot_emit_itrs_inc(f, pad1);
        aot_emit_node(f, mat_loc + 0, dep, out, 1, pad1, tmp);
        fprintf(f, "%s} else {\n", pad);
        aot_emit_itrs_inc(f, pad1);
        aot_emit_node(f, mat_loc + 1, dep, out, 1, pad1, tmp);
        fprintf(f, "%s}\n", pad);
        return;
      }

      case MAT: {
        u64 mat_loc = term_val(term);
        char frm[32];
        char app[32];
        char arg[32];
        char tag_n[32];
        char ari[32];
        char ctr[32];
        char fld[32];
        char src[32];
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        aot_emit_tmp(app, sizeof(app), "app", tmp);
        aot_emit_tmp(arg, sizeof(arg), "arg", tmp);
        aot_emit_tmp(tag_n, sizeof(tag_n), "tag", tmp);
        aot_emit_tmp(ari, sizeof(ari), "ari", tmp);
        aot_emit_tmp(ctr, sizeof(ctr), "ctr", tmp);
        aot_emit_tmp(fld, sizeof(fld), "fld", tmp);
        aot_emit_tmp(src, sizeof(src), "src", tmp);

        fprintf(f, "%su8 %s = (*a_pos > 0);\n", pad, src);
        fprintf(f, "%sTerm %s;\n", pad, arg);
        fprintf(f, "%sif (%s) {\n", pad, src);
        fprintf(f, "%s%s = a_args[*a_pos - 1];\n", pad1, arg);
        fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad1, arg, arg);
        fprintf(f, "%sa_args[*a_pos - 1] = %s;\n", pad1, arg);
        fprintf(f, "%s} else {\n", pad);
        fprintf(f, "%sif (*s_pos <= base) {\n", pad1);
        aot_emit_ret_head(f, loc, dep, pad2, tmp);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1];\n", pad1, frm);
        fprintf(f, "%sif (term_tag(%s) != APP) {\n", pad1, frm);
        aot_emit_ret_head(f, loc, dep, pad2, tmp);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad1, app, frm);
        fprintf(f, "%s%s = heap_read(%s + 1);\n", pad1, arg, app);
        fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad1, arg, arg);
        fprintf(f, "%sheap_set_rel(%s + 1, %s);\n", pad1, app, arg);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%su8 %s = term_tag(%s);\n", pad, tag_n, arg);
        fprintf(f, "%sif (%s < C00 || %s > C16) {\n", pad, tag_n, tag_n);
        u32 site = AOT_EMIT_DEOPT_SITE++;
        aot_emit_deopt_site_note(site, AOT_DEOPT_SITE_KIND_HEAD_MAT_NON_CTR, loc, term_ext(term));
        fprintf(f, "%saot_deopt_site_observe_term(%u, %s);\n", pad1, site, arg);
        fprintf(f, "%sreturn aot_fallback_alo(%lluULL, %u, ", pad1, (unsigned long long)loc, dep);
        aot_emit_env_head(f, dep);
        fprintf(f, ", %u, %u);\n", AOT_DEOPT_EXPR_UNSUPPORTED_TAG, site);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sif (term_ext(%s) == %u) {\n", pad, arg, term_ext(term));
        fprintf(f, "%su32 %s = (u32)(%s - C00);\n", pad1, ari, tag_n);
        fprintf(f, "%sif (%s) {\n", pad1, src);
        fprintf(f, "%sif ((u64)(*a_pos) + (u64)%s > (u64)AOT_ARG_CAP + 1ULL) {\n", pad2, ari);
        aot_emit_ret_fallback_loc(f, loc, dep, AOT_DEOPT_HEAD_APP_ARG_CAP, pad2);
        fprintf(f, "%s}\n", pad2);
        fprintf(f, "%s(*a_pos)--;\n", pad2);
        fprintf(f, "%s} else {\n", pad1);
        fprintf(f, "%sif ((u64)(*a_pos) + (u64)%s > (u64)AOT_ARG_CAP) {\n", pad2, ari);
        aot_emit_ret_fallback_loc(f, loc, dep, AOT_DEOPT_HEAD_APP_ARG_CAP, pad2);
        fprintf(f, "%s}\n", pad2);
        fprintf(f, "%s(*s_pos)--;\n", pad2);
        fprintf(f, "%s}\n", pad1);
        aot_emit_itrs_inc(f, pad1);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad1, ctr, arg);
        fprintf(f, "%sfor (u32 j = %s; j > 0; j--) {\n", pad1, ari);
        fprintf(f, "%s  Term %s = heap_read(%s + (u64)(j - 1));\n", pad1, fld, ctr);
        fprintf(f, "%s  a_args[*a_pos] = %s;\n", pad1, fld);
        fprintf(f, "%s  (*a_pos)++;\n", pad1);
        fprintf(f, "%s}\n", pad1);
        aot_emit_node(f, mat_loc + 0, dep, out, 1, pad1, tmp);
        fprintf(f, "%s} else {\n", pad);
        aot_emit_itrs_inc(f, pad1);
        aot_emit_node(f, mat_loc + 1, dep, out, 1, pad1, tmp);
        fprintf(f, "%s}\n", pad);
        return;
      }

      case OP2: {
        u32 opr = term_ext(term);
        u64 arg = term_val(term);
        char lhs[32];
        char rhs[32];
        char out_n[32];
        aot_emit_tmp(lhs, sizeof(lhs), "lhs", tmp);
        aot_emit_tmp(rhs, sizeof(rhs), "rhs", tmp);
        aot_emit_tmp(out_n, sizeof(out_n), "out", tmp);

        aot_emit_node_demanded(f, arg + 0, dep, lhs, pad, tmp);
        fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad, lhs, lhs);

        fprintf(f, "%sTerm %s;\n", pad, out_n);
        fprintf(f, "%sif (term_tag(%s) == ERA) {\n", pad, lhs);
        fprintf(f, "%s%s = wnf_op2_era();\n", pad1, out_n);
        fprintf(f, "%s} else if (term_tag(%s) == NUM) {\n", pad, lhs);
        aot_emit_node_demanded(f, arg + 1, dep, rhs, pad1, tmp);
        fprintf(f, "%s%s = aot_force_strict(%s, *s_pos);\n", pad1, rhs, rhs);
        fprintf(f, "%sif (term_tag(%s) == ERA) {\n", pad1, rhs);
        fprintf(f, "%s%s = wnf_op2_num_era();\n", pad2, out_n);
        fprintf(f, "%s} else if (term_tag(%s) == NUM) {\n", pad1, rhs);
        fprintf(f, "%s%s = wnf_op2_num_num_raw(%u, (u32)term_val(%s), (u32)term_val(%s));\n", pad2, out_n, opr, lhs, rhs);
        fprintf(f, "%s} else {\n", pad1);
        fprintf(f, "%s%s = term_new_op2(%u, %s, %s);\n", pad2, out_n, opr, lhs, rhs);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%s} else {\n", pad);
        fprintf(f, "%s%s = term_new_op2(%u, %s, ", pad1, out_n, opr, lhs);
        aot_emit_alo_expr(f, arg + 1, dep, AOT_DEOPT_EXPR_OP2_RHS_CAPTURE);
        fprintf(f, ");\n");
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sreturn %s;\n", pad, out_n);
        return;
      }

      case REF: {
        fprintf(f, "%sif (STEPS_ITRS_LIM == 0) {\n", pad);
        if (term_ext(term) == AOT_EMIT_DEF_ID && AOT_EMIT_SELF_LOOP) {
          fprintf(f, "%scontinue;\n", pad1);
        } else {
          fprintf(f, "%sreturn aot_call_ref(%u, stack, s_pos, base, a_args, a_pos);\n", pad1, term_ext(term));
        }
        fprintf(f, "%s}\n", pad);
        aot_emit_ret_head(f, loc, dep, pad, tmp);
        return;
      }

      default: {
        aot_emit_ret_head(f, loc, dep, pad, tmp);
        return;
      }
    }
  }

  switch (tag) {
    case NUM:
    case NAM:
    case ERA:
    case ANY:
    case C00:
    case REF: {
      fprintf(f, "%sTerm %s = heap_read(%lluULL);\n", pad, out, (unsigned long long)loc);
      return;
    }
    case C01 ... C16: {
      u32 ari = (u32)(tag - C00);
      u64 ctr_loc = term_val(term);
      char ctr_n[32];
      char fld_n[32];
      aot_emit_tmp(ctr_n, sizeof(ctr_n), "ctr", tmp);

      fprintf(f, "%su64 %s = aot_alloc(%u, AOT_ALLOC_CTR_FIELDS);\n", pad, ctr_n, ari);
      for (u32 i = 0; i < ari; i++) {
        aot_emit_tmp(fld_n, sizeof(fld_n), "fld", tmp);
        if (aot_emit_expr_structural(ctr_loc + i)) {
          aot_emit_node(f, ctr_loc + i, dep, fld_n, 0, pad, tmp);
        } else {
          fprintf(f, "%sTerm %s = ", pad, fld_n);
          aot_emit_alo_expr(f, ctr_loc + i, dep, AOT_DEOPT_EXPR_CTR_FIELD_CAPTURE);
          fprintf(f, ";\n");
        }
        fprintf(f, "%sheap_set(%s + %u, %s);\n", pad, ctr_n, i, fld_n);
      }
      fprintf(f, "%sTerm %s = term_new(%u, %u, %u, %s);\n", pad, out, term_sub_get(term), tag, term_ext(term), ctr_n);
      return;
    }
    case VAR:
    case BJV: {
      u64 lvl = term_val(term);
      if (lvl == 0 || lvl > dep) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep, AOT_DEOPT_EXPR_VAR_LEVEL_OOB);
        fprintf(f, ";\n");
        return;
      }
      fprintf(f, "%sTerm %s = x%u;\n", pad, out, (u32)(lvl - 1));
      return;
    }
    case DP0:
    case BJ0: {
      u64 lvl = term_val(term);
      if (lvl == 0 || lvl > dep) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep, AOT_DEOPT_EXPR_DP_LEVEL_OOB);
        fprintf(f, ";\n");
        return;
      }

      u32 idx = (u32)(lvl - 1);
      u32 lab = term_ext(term);
      char cell_n[32];
      char loc_n[32];
      aot_emit_tmp(cell_n, sizeof(cell_n), "cell", tmp);
      aot_emit_tmp(loc_n, sizeof(loc_n), "loc", tmp);

      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%sTerm %s;\n", pad, cell_n);
      fprintf(f, "%su64 %s = env_locs[%u];\n", pad, loc_n, idx);
      fprintf(f, "%sif (%s != 0ULL) {\n", pad, loc_n);
      fprintf(f, "%s%s = heap_read(%s + 0);\n", pad1, cell_n, loc_n);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%s%s = env_cells[%u];\n", pad1, cell_n, idx);
      fprintf(f, "%s}\n", pad);
      fprintf(f, "%sif (term_sub_get(%s)) {\n", pad, cell_n);
      fprintf(f, "%s%s = term_sub_set(%s, 0);\n", pad1, out, cell_n);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%s%s = aot_env_get(env_cells, env_locs, %u);\n", pad1, loc_n, idx);
      fprintf(f, "%s%s = term_new(0, DP0, %u, %s);\n", pad1, out, lab, loc_n);
      fprintf(f, "%s}\n", pad);
      return;
    }
    case DP1:
    case BJ1: {
      u64 lvl = term_val(term);
      if (lvl == 0 || lvl > dep) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep, AOT_DEOPT_EXPR_DP_LEVEL_OOB);
        fprintf(f, ";\n");
        return;
      }

      u32 idx = (u32)(lvl - 1);
      u32 lab = term_ext(term);
      char cell_n[32];
      char loc_n[32];
      aot_emit_tmp(cell_n, sizeof(cell_n), "cell", tmp);
      aot_emit_tmp(loc_n, sizeof(loc_n), "loc", tmp);

      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%sTerm %s;\n", pad, cell_n);
      fprintf(f, "%su64 %s = env_locs[%u];\n", pad, loc_n, idx);
      fprintf(f, "%sif (%s != 0ULL) {\n", pad, loc_n);
      fprintf(f, "%s%s = heap_read(%s + 0);\n", pad1, cell_n, loc_n);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%s%s = env_cells[%u];\n", pad1, cell_n, idx);
      fprintf(f, "%s}\n", pad);
      fprintf(f, "%sif (term_sub_get(%s)) {\n", pad, cell_n);
      fprintf(f, "%s%s = term_sub_set(%s, 0);\n", pad1, out, cell_n);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%s%s = aot_env_get(env_cells, env_locs, %u);\n", pad1, loc_n, idx);
      fprintf(f, "%s%s = term_new(0, DP1, %u, %s);\n", pad1, out, lab, loc_n);
      fprintf(f, "%s}\n", pad);
      return;
    }
    case OP2: {
      u32 opr = term_ext(term);
      u64 op2_loc = term_val(term);
      char lhs_n[32];
      char rhs_n[32];
      aot_emit_tmp(lhs_n, sizeof(lhs_n), "lhs", tmp);
      aot_emit_tmp(rhs_n, sizeof(rhs_n), "rhs", tmp);

      aot_emit_node(f, op2_loc + 0, dep, lhs_n, 0, pad, tmp);
      aot_emit_node(f, op2_loc + 1, dep, rhs_n, 0, pad, tmp);
      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%sif (STEPS_ITRS_LIM == 0) {\n", pad);
      fprintf(f, "%sif (term_tag(%s) == ERA) {\n", pad1, lhs_n);
      fprintf(f, "%s%s = wnf_op2_era();\n", pad2, out);
      fprintf(f, "%s} else if (term_tag(%s) == NUM) {\n", pad1, lhs_n);
      fprintf(f, "%sif (term_tag(%s) == ERA) {\n", pad2, rhs_n);
      fprintf(f, "%s%s = wnf_op2_num_era();\n", pad2, out);
      fprintf(f, "%s} else if (term_tag(%s) == NUM) {\n", pad2, rhs_n);
      fprintf(f, "%s%s = wnf_op2_num_num_raw(%u, (u32)term_val(%s), (u32)term_val(%s));\n", pad2, out, opr, lhs_n, rhs_n);
      fprintf(f, "%s} else {\n", pad2);
      fprintf(f, "%s%s = term_new_op2(%u, %s, %s);\n", pad2, out, opr, lhs_n, rhs_n);
      fprintf(f, "%s}\n", pad2);
      fprintf(f, "%s} else {\n", pad1);
      fprintf(f, "%s%s = term_new_op2(%u, %s, %s);\n", pad2, out, opr, lhs_n, rhs_n);
      fprintf(f, "%s}\n", pad1);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%s%s = term_new_op2(%u, %s, %s);\n", pad1, out, opr, lhs_n, rhs_n);
      fprintf(f, "%s}\n", pad);
      return;
    }
    case APP: {
      u64 app_loc = term_val(term);
      Term fun = heap_read(app_loc + 0);
      u8 fun_tag = term_tag(fun);
      if (fun_tag != LAM) {
        char fun_n[32];
        char arg_n[32];
        aot_emit_tmp(fun_n, sizeof(fun_n), "fun", tmp);
        aot_emit_tmp(arg_n, sizeof(arg_n), "arg", tmp);
        aot_emit_node(f, app_loc + 0, dep, fun_n, 0, pad, tmp);
        if (aot_emit_expr_structural(app_loc + 1)) {
          aot_emit_node(f, app_loc + 1, dep, arg_n, 0, pad, tmp);
        } else {
          fprintf(f, "%sTerm %s = ", pad, arg_n);
          aot_emit_alo_expr(f, app_loc + 1, dep, AOT_DEOPT_EXPR_APP_ARG_CAPTURE);
          fprintf(f, ";\n");
        }
        fprintf(f, "%sTerm %s = term_new_app(%s, %s);\n", pad, out, fun_n, arg_n);
        return;
      }
      if (dep >= AOT_ENV_CAP) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep, AOT_DEOPT_EXPR_APP_ENV_CAP);
        fprintf(f, ";\n");
        return;
      }
      if ((term_ext(fun) & LAM_ERA_MASK) != 0) {
        fprintf(f, "%senv_cells[%u] = term_sub_set(term_new_era(), 1);\n", pad, dep);
        fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad, dep);
      } else {
        if (aot_emit_expr_structural(app_loc + 1)) {
          char arg_n[32];
          aot_emit_tmp(arg_n, sizeof(arg_n), "arg", tmp);
          aot_emit_node(f, app_loc + 1, dep, arg_n, 0, pad, tmp);
          fprintf(f, "%sTerm x%u = %s;\n", pad, dep, arg_n);
        } else {
          fprintf(f, "%sTerm x%u = ", pad, dep);
          aot_emit_alo_expr(f, app_loc + 1, dep, AOT_DEOPT_EXPR_APP_ARG_CAPTURE);
          fprintf(f, ";\n");
        }
        fprintf(f, "%senv_cells[%u] = term_sub_set(x%u, 1);\n", pad, dep, dep);
        fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad, dep);
      }
      aot_emit_itrs_inc(f, pad);
      aot_emit_node(f, term_val(fun), dep + 1, out, 0, pad, tmp);
      return;
    }
    case DUP: {
      u64 dup_loc = term_val(term);
      if (dep >= AOT_ENV_CAP) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep, AOT_DEOPT_EXPR_DUP_ENV_CAP);
        fprintf(f, ";\n");
        return;
      }

      char val_n[32];
      char bod_n[32];
      aot_emit_tmp(val_n, sizeof(val_n), "val", tmp);
      aot_emit_tmp(bod_n, sizeof(bod_n), "bod", tmp);

      aot_emit_node(f, dup_loc + 0, dep, val_n, 0, pad, tmp);
      fprintf(f, "%sTerm x%u = %s;\n", pad, dep, val_n);
      fprintf(f, "%sif (aot_is_copy_free(x%u)) {\n", pad, dep);
      fprintf(f, "%senv_cells[%u] = term_sub_set(x%u, 1);\n", pad1, dep, dep);
      fprintf(f, "%senv_locs[%u] = 0ULL;\n", pad1, dep);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%su64 e%u = aot_alloc(2, AOT_ALLOC_DUP_CELL);\n", pad1, dep);
      fprintf(f, "%sheap_set(e%u + 1, term_new(0, NUM, 0, 0ULL));\n", pad1, dep);
      fprintf(f, "%sheap_set(e%u + 0, x%u);\n", pad1, dep, dep);
      fprintf(f, "%senv_locs[%u] = e%u;\n", pad1, dep, dep);
      fprintf(f, "%s}\n", pad);
      aot_emit_node(f, dup_loc + 1, dep + 1, bod_n, 0, pad, tmp);
      fprintf(f, "%sTerm %s = %s;\n", pad, out, bod_n);
      return;
    }
    default: {
      fprintf(f, "%sTerm %s = ", pad, out);
      aot_emit_alo_expr_meta(
        f,
        loc,
        dep,
        AOT_DEOPT_EXPR_UNSUPPORTED_TAG,
        AOT_DEOPT_SITE_KIND_EXPR_UNSUPPORTED_TAG,
        aot_deopt_aux_pack_tag_ext(tag, term_ext(term)));
      fprintf(f, ";\n");
      return;
    }
  }
}

// Definition Emitter
// ------------------

// Emits one compiled definition.
fn void aot_emit_def(FILE *f, u32 id) {
  if (BOOK[id] == 0) {
    return;
  }

  char *name = table_get(id);
  if (name == NULL) {
    return;
  }

  u64 root = BOOK[id];
  char fun_name[256];
  aot_emit_fun_name(fun_name, sizeof(fun_name), name);

  fprintf(f, "// Compiled function for @%s (id %u).\n", name, id);
  fprintf(f, "static Term %s(Term *stack, u32 *s_pos, u32 base, Term *a_args, u32 *a_pos) {\n", fun_name);
  fprintf(f, "  if (aot_call_depth() >= AOT_MAX_DEPTH) {\n");
  fprintf(f, "    return ");
  aot_emit_alo_expr(f, root, 0, AOT_DEOPT_CALL_DEPTH_CAP);
  fprintf(f, ";\n");
  fprintf(f, "  }\n");
  fprintf(f, "\n");
  u8 self_loop = aot_emit_has_self_tail_ref(root, id);
  AOT_EMIT_DEF_ID = id;
  AOT_EMIT_SELF_LOOP = self_loop;
  if (self_loop) {
    fprintf(f, "  for (;;) {\n");
    fprintf(f, "    Term env_cells[AOT_ENV_CAP] = {0};\n");
    fprintf(f, "    u64 env_locs[AOT_ENV_CAP] = {0};\n");
    fprintf(f, "\n");
    {
      u32 tmp = 0;
      aot_emit_node(f, root, 0, NULL, 1, "    ", &tmp);
    }
    fprintf(f, "  }\n");
  } else {
    fprintf(f, "  Term env_cells[AOT_ENV_CAP] = {0};\n");
    fprintf(f, "  u64 env_locs[AOT_ENV_CAP] = {0};\n");
    fprintf(f, "\n");
    {
      u32 tmp = 0;
      aot_emit_node(f, root, 0, NULL, 1, "  ", &tmp);
    }
  }
  fprintf(f, "}\n\n");
}

// Registration Emitter
// --------------------

// Emits registration for all compiled definitions.
fn void aot_emit_register(FILE *f) {
  fprintf(f, "// Registers generated functions into the runtime table.\n");
  fprintf(f, "static void aot_register_generated(void) {\n");
  for (u32 id = 0; id < TABLE.len; id++) {
    if (BOOK[id] == 0) {
      continue;
    }

    char *name = table_get(id);
    if (name == NULL) {
      continue;
    }

    char fun_name[256];
    aot_emit_fun_name(fun_name, sizeof(fun_name), name);

    fprintf(f, "  // @%s\n", name);
    fprintf(f, "  AOT_FNS[%u] = %s;\n", id, fun_name);
  }
  fprintf(f, "}\n\n");
}

// Emits registration for source-mapped deopt sites.
fn void aot_emit_register_deopt_sites(FILE *f) {
  u32 len = AOT_EMIT_DEOPT_SITE;
  if (len > AOT_DEOPT_SITE_CAP) {
    len = AOT_DEOPT_SITE_CAP;
  }

  fprintf(f, "// Registers deopt-site metadata for deopt counters.\n");
  fprintf(f, "static void aot_register_deopt_sites_generated(void) {\n");
  for (u32 site = 0; site < len; site++) {
    u32 kind = AOT_EMIT_DEOPT_KIND[site];
    if (kind == AOT_DEOPT_SITE_KIND_NONE) {
      continue;
    }
    fprintf(f, "  aot_deopt_site_define(%u, %u, %lluULL, %u);\n",
      site,
      kind,
      (unsigned long long)AOT_EMIT_DEOPT_LOC[site],
      AOT_EMIT_DEOPT_AUX[site]);
  }
  fprintf(f, "}\n\n");
}

// Emits the static FFI-load table used by standalone AOT programs.
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

// Emits standalone entrypoint using shared runtime helper functions.
fn void aot_emit_entry_main(FILE *f, const AotBuildCfg *cfg) {
  u32 threads = (cfg && cfg->threads > 0) ? cfg->threads : 1;
  int debug   = cfg ? cfg->debug : 0;

  RuntimeEvalCfg eval_cfg = {
    .do_collapse    = 0,
    .collapse_limit = -1,
    .stats          = 0,
    .silent         = 0,
    .step_by_step   = 0,
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
  fprintf(f, "  aot_register_deopt_sites_generated();\n");
  fprintf(f, "  RuntimeEvalCfg eval = {\n");
  fprintf(f, "    .do_collapse = %d,\n", eval_cfg.do_collapse);
  fprintf(f, "    .collapse_limit = %d,\n", eval_cfg.collapse_limit);
  fprintf(f, "    .stats = %d,\n", eval_cfg.stats);
  fprintf(f, "    .silent = %d,\n", eval_cfg.silent);
  fprintf(f, "    .step_by_step = %d,\n", eval_cfg.step_by_step);
  fprintf(f, "  };\n");
  fprintf(f, "  runtime_eval_main(main_id, &eval);\n");
  fprintf(f, "  runtime_free();\n");
  fprintf(f, "  return 0;\n");
  fprintf(f, "}\n");
}

// Program Emitter
// ---------------

// Emits the full standalone AOT C program.
fn void aot_emit_to_file(FILE *f, const char *runtime_path, const char *src_path, const char *src_text, const AotBuildCfg *cfg) {
  AOT_EMIT_ITRS = aot_emit_counting(cfg);
  AOT_EMIT_DEOPT_SITE = 0;
  memset(AOT_EMIT_DEOPT_KIND, 0, sizeof(AOT_EMIT_DEOPT_KIND));
  memset(AOT_EMIT_DEOPT_LOC, 0, sizeof(AOT_EMIT_DEOPT_LOC));
  memset(AOT_EMIT_DEOPT_AUX, 0, sizeof(AOT_EMIT_DEOPT_AUX));

  fprintf(f, "// Auto-generated by HVM AOT.\n");
  fprintf(f, "// This file is standalone: compile with `clang -O2 -o <out> <file.c>`.\n");
  fprintf(f, "//\n");
  fprintf(f, "// AOT summary:\n");
  fprintf(f, "// - Includes full runtime TU directly (%s).\n", runtime_path);
  fprintf(f, "// - Emits one tree-shaped function per definition.\n");
  fprintf(f, "// - Uses lexical binder registers x0, x1, ...\n");
  fprintf(f, "// - Deopts by returning linear-safe residual terms.\n\n");

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
  aot_emit_register_deopt_sites(f);
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
