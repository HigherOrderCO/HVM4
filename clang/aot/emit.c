// AOT Module: Program Emitter
// ---------------------------
// Emits standalone C with one direct tree-shaped function per definition.

fn char *table_get(u32 id);

// Emit Options
// ------------

// Controls whether emitted AOT code should include interaction counting calls.
static int AOT_EMIT_ITRS = 1;

// Returns 1 when this AOT build needs interaction counting.
fn int aot_emit_counting(const AotBuildCfg *cfg) {
  if (cfg == NULL) {
    return 1;
  }
  return cfg->eval.stats || cfg->eval.silent || cfg->eval.step_by_step;
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

// Emits one lexical environment term argument.
// Active DUP binders keep their original pending value (`xN`) so fallback
// environments can keep DP semantics. Once a DUP cell is substituted, the
// captured value becomes the substituted payload.
fn void aot_emit_env_arg(FILE *f, u32 idx) {
  fprintf(f, "x%u", idx);
}

// Emits one lexical environment argument pair: `<len>, <ptr>`.
fn void aot_emit_env_args(FILE *f, u32 dep) {
  if (dep == 0) {
    fprintf(f, "0, NULL");
    return;
  }

  fprintf(f, "%u, (const Term[]){", dep);
  for (u32 i = 0; i < dep; i++) {
    if (i > 0) {
      fprintf(f, ", ");
    }
    aot_emit_env_arg(f, i);
  }
  fprintf(f, "}");
}

// Emits one ALO expression for static `loc` under current lexical depth.
fn void aot_emit_alo_expr(FILE *f, u64 loc, u32 dep) {
  fprintf(f, "aot_fallback_alo(%lluULL, ", (unsigned long long)loc);
  aot_emit_env_args(f, dep);
  fprintf(f, ")");
}

// Emits one deopt return for current location + lexical env.
fn void aot_emit_ret_fallback_loc(FILE *f, u64 loc, u32 dep, const char *pad) {
  fprintf(f, "%sreturn ", pad);
  aot_emit_alo_expr(f, loc, dep);
  fprintf(f, ";\n");
}

// Flattens one static APP spine into a REF head + argument locations.
fn int aot_emit_collect_app_ref(u64 loc, u32 *ref_id, u64 *arg_locs, u16 *arg_len) {
  u64 at = loc;
  u16 len = 0;

  for (;;) {
    Term cur = heap_read(at);
    if (term_tag(cur) != APP) {
      break;
    }

    if (len >= AOT_ARG_CAP) {
      return 0;
    }

    u64 app_loc = term_val(cur);
    arg_locs[len] = app_loc + 1;
    len++;
    at = app_loc + 0;
  }

  Term head = heap_read(at);
  if (term_tag(head) != REF) {
    return 0;
  }

  *ref_id  = term_ext(head);
  *arg_len = len;
  return 1;
}

// Returns 1 when callee argument 0 is guaranteed strict at entry.
fn int aot_emit_ref_arg0_strict(u32 ref_id) {
  if (BOOK[ref_id] == 0) {
    return 0;
  }

  Term root = heap_read(BOOK[ref_id]);
  u8  tag  = term_tag(root);
  return tag == SWI || tag == MAT;
}

// Returns 1 when one static subtree has no local binder references.
fn int aot_emit_is_closed(u64 loc, u32 dep) {
  Term term = heap_read(loc);
  u8 tag = term_tag(term);

  if (tag == VAR || tag == BJV || tag == DP0 || tag == BJ0 || tag == DP1 || tag == BJ1) {
    u64 lvl = term_val(term);
    return lvl == 0 || lvl > dep;
  }

  u32 ari = term_arity(term);
  if (ari == 0) {
    return 1;
  }

  u64 base = term_val(term);
  for (u32 i = 0; i < ari; i++) {
    if (!aot_emit_is_closed(base + i, dep)) {
      return 0;
    }
  }
  return 1;
}

// Returns 1 when one static subtree is a copy-free constructor/number tree.
fn int aot_emit_is_static_copy_free(u64 loc) {
  Term term = heap_read(loc);
  u8 tag = term_tag(term);

  if (tag == NUM || tag == C00) {
    return 1;
  }
  if (tag < C01 || tag > C16) {
    return 0;
  }

  u32 ari = term_arity(term);
  u64 base = term_val(term);
  for (u32 i = 0; i < ari; i++) {
    if (!aot_emit_is_static_copy_free(base + i)) {
      return 0;
    }
  }
  return 1;
}

// Emit Core
// ---------

// Emits one recursive node.
// - `head=1`: consumes APP frames from `stack/*s_pos` and returns from F_<def>.
// - `head=0`: evaluates one expression into local `Term <out>`.
fn void aot_emit_node(FILE *f, u64 loc, u32 dep, const char *out, u8 head, const char *pad, u32 *tmp) {
  Term term = heap_read(loc);
  u8   tag  = term_tag(term);

  char pad1[128];
  char pad2[128];
  char pad3[128];
  aot_emit_pad_next(pad1, sizeof(pad1), pad);
  aot_emit_pad_next(pad2, sizeof(pad2), pad1);
  aot_emit_pad_next(pad3, sizeof(pad3), pad2);

  aot_emit_wnf_comment(f, loc, pad);

  if (head) {
    switch (tag) {
      case REF: {
        u32 ref_id = term_ext(term);
        int has_fun = 0;
        char fun_name[256];
        if (ref_id < TABLE.len && BOOK[ref_id] != 0) {
          char *ref_name = table_get(ref_id);
          if (ref_name != NULL) {
            aot_emit_fun_name(fun_name, sizeof(fun_name), ref_name);
            has_fun = 1;
          }
        }

        char argc[32];
        char call_args[32];
        char frm[32];
        char app[32];
        char arg_n[32];
        char out_n[32];
        aot_emit_tmp(argc, sizeof(argc), "argc", tmp);
        aot_emit_tmp(call_args, sizeof(call_args), "call_args", tmp);
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        aot_emit_tmp(app, sizeof(app), "app", tmp);
        aot_emit_tmp(arg_n, sizeof(arg_n), "arg", tmp);
        aot_emit_tmp(out_n, sizeof(out_n), "out", tmp);

        fprintf(f, "%sif (*s_pos > base) {\n", pad);
        fprintf(f, "%sTerm %s;\n", pad1, out_n);
        if (has_fun) {
          fprintf(f, "%sif (aot_call_depth() < AOT_MAX_DEPTH) {\n", pad1);
          fprintf(f, "%sAOT_CALL_DEPTH++;\n", pad2);
          fprintf(f, "%s%s = %s(stack, s_pos, base);\n", pad2, out_n, fun_name);
          fprintf(f, "%sAOT_CALL_DEPTH--;\n", pad2);
          fprintf(f, "%sreturn %s;\n", pad2, out_n);
          fprintf(f, "%s}\n", pad1);
        } else {
          fprintf(f, "%sif (aot_call_ref_fast(%u, stack, s_pos, base, &%s)) {\n", pad1, ref_id, out_n);
          fprintf(f, "%sreturn %s;\n", pad2, out_n);
          fprintf(f, "%s}\n", pad1);
        }
        fprintf(f, "%su32 %s = *s_pos - base;\n", pad1, argc);
        fprintf(f, "%sif (%s > AOT_ARG_CAP) {\n", pad1, argc);
        aot_emit_ret_fallback_loc(f, loc, dep, pad2);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%sTerm %s[AOT_ARG_CAP];\n", pad1, call_args);
        fprintf(f, "%sfor (u32 i = 0; i < %s; i++) {\n", pad1, argc);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1 - i];\n", pad2, frm);
        fprintf(f, "%sif (term_tag(%s) != APP) {\n", pad2, frm);
        aot_emit_ret_fallback_loc(f, loc, dep, pad3);
        fprintf(f, "%s}\n", pad2);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad2, app, frm);
        fprintf(f, "%sTerm %s = heap_read(%s + 1);\n", pad2, arg_n, app);
        fprintf(f, "%s%s[i] = %s;\n", pad2, call_args, arg_n);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%s*s_pos = base;\n", pad1);
        fprintf(f, "%s%s = aot_call_ref(%u, (u16)%s, %s);\n", pad1, out_n, ref_id, argc, call_args);
        fprintf(f, "%sreturn %s;\n", pad1, out_n);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sTerm %s = heap_read(%lluULL);\n", pad, out_n, (unsigned long long)loc);
        fprintf(f, "%sreturn %s;\n", pad, out_n);
        return;
      }

      case LAM: {
        if (dep >= AOT_ENV_CAP) {
          aot_emit_ret_fallback_loc(f, loc, dep, pad);
          return;
        }

        char frm[32];
        char app[32];
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        aot_emit_tmp(app, sizeof(app), "app", tmp);

        fprintf(f, "%sif (*s_pos <= base) {\n", pad);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1];\n", pad, frm);
        fprintf(f, "%sif (term_tag(%s) != APP) {\n", pad, frm);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%s(*s_pos)--;\n", pad);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad, app, frm);
        fprintf(f, "%sTerm x%u = heap_read(%s + 1);\n", pad, dep, app);
        fprintf(f, "%su8 x%u_is_dup = 0;\n", pad, dep);
        fprintf(f, "%su64 x%u_loc = 0;\n", pad, dep);
        aot_emit_itrs_inc(f, pad);
        aot_emit_node(f, term_val(term), dep + 1, out, 1, pad, tmp);
        return;
      }

      case SWI: {
        u64 mat_loc = term_val(term);
        char frm[32];
        char app[32];
        char arg[32];
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        aot_emit_tmp(app, sizeof(app), "app", tmp);
        aot_emit_tmp(arg, sizeof(arg), "arg", tmp);

        fprintf(f, "%sif (*s_pos <= base) {\n", pad);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1];\n", pad, frm);
        fprintf(f, "%sif (term_tag(%s) != APP) {\n", pad, frm);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad, app, frm);
        fprintf(f, "%sTerm %s = heap_read(%s + 1);\n", pad, arg, app);
        fprintf(f, "%sif (term_tag(%s) != NUM) {\n", pad, arg);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sif (term_val(%s) == %uULL) {\n", pad, arg, term_ext(term));
        fprintf(f, "%s(*s_pos)--;\n", pad1);
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
        char cell[32];
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        aot_emit_tmp(app, sizeof(app), "app", tmp);
        aot_emit_tmp(arg, sizeof(arg), "arg", tmp);
        aot_emit_tmp(tag_n, sizeof(tag_n), "tag", tmp);
        aot_emit_tmp(ari, sizeof(ari), "ari", tmp);
        aot_emit_tmp(ctr, sizeof(ctr), "ctr", tmp);
        aot_emit_tmp(fld, sizeof(fld), "fld", tmp);
        aot_emit_tmp(cell, sizeof(cell), "cell", tmp);

        fprintf(f, "%sif (*s_pos <= base) {\n", pad);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1];\n", pad, frm);
        fprintf(f, "%sif (term_tag(%s) != APP) {\n", pad, frm);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad, app, frm);
        fprintf(f, "%sTerm %s = heap_read(%s + 1);\n", pad, arg, app);
        fprintf(f, "%su8 %s = term_tag(%s);\n", pad, tag_n, arg);
        fprintf(f, "%sif (%s < C00 || %s > C16) {\n", pad, tag_n, tag_n);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);
        fprintf(f, "%sif (term_ext(%s) == %u) {\n", pad, arg, term_ext(term));
        fprintf(f, "%s(*s_pos)--;\n", pad1);
        aot_emit_itrs_inc(f, pad1);
        fprintf(f, "%su32 %s = (u32)(%s - C00);\n", pad1, ari, tag_n);
        fprintf(f, "%su64 %s = term_val(%s);\n", pad1, ctr, arg);
        fprintf(f, "%sfor (u32 j = %s; j > 0; j--) {\n", pad1, ari);
        fprintf(f, "%s  Term %s = heap_read(%s + (u64)(j - 1));\n", pad1, fld, ctr);
        fprintf(f, "%s  u64 %s = heap_alloc(2);\n", pad1, cell);
        fprintf(f, "%s  heap_set(%s + 0, term_new_era());\n", pad1, cell);
        fprintf(f, "%s  heap_set(%s + 1, %s);\n", pad1, cell, fld);
        fprintf(f, "%s  stack[*s_pos] = term_new(0, APP, 0, %s);\n", pad1, cell);
        fprintf(f, "%s  (*s_pos)++;\n", pad1);
        fprintf(f, "%s}\n", pad1);
        aot_emit_node(f, mat_loc + 0, dep, out, 1, pad1, tmp);
        fprintf(f, "%s} else {\n", pad);
        aot_emit_itrs_inc(f, pad1);
        aot_emit_node(f, mat_loc + 1, dep, out, 1, pad1, tmp);
        fprintf(f, "%s}\n", pad);
        return;
      }

      case APP: {
        u64 app_loc = term_val(term);
        char arg_n[32];
        char cell[32];
        aot_emit_tmp(arg_n, sizeof(arg_n), "arg", tmp);
        aot_emit_tmp(cell, sizeof(cell), "cell", tmp);

        fprintf(f, "%sif (*s_pos >= AOT_ARG_CAP) {\n", pad);
        aot_emit_ret_fallback_loc(f, loc, dep, pad1);
        fprintf(f, "%s}\n", pad);

        aot_emit_node(f, app_loc + 1, dep, arg_n, 0, pad, tmp);
        fprintf(f, "%su64 %s = heap_alloc(2);\n", pad, cell);
        fprintf(f, "%sheap_set(%s + 0, term_new_era());\n", pad, cell);
        fprintf(f, "%sheap_set(%s + 1, %s);\n", pad, cell, arg_n);
        fprintf(f, "%sstack[*s_pos] = term_new(0, APP, 0, %s);\n", pad, cell);
        fprintf(f, "%s(*s_pos)++;\n", pad);
        aot_emit_node(f, app_loc + 0, dep, out, 1, pad, tmp);
        return;
      }

      case DUP: {
        if (dep >= AOT_ENV_CAP) {
          aot_emit_ret_fallback_loc(f, loc, dep, pad);
          return;
        }

        u64 dup_loc = term_val(term);
        char val[32];
        aot_emit_tmp(val, sizeof(val), "val", tmp);

        aot_emit_node(f, dup_loc + 0, dep, val, 0, pad, tmp);
        fprintf(f, "%sTerm x%u = %s;\n", pad, dep, val);
        fprintf(f, "%su8 x%u_is_dup = 0;\n", pad, dep);
        fprintf(f, "%su64 x%u_loc = 0;\n", pad, dep);
        fprintf(f, "%sif (!aot_is_copy_free(x%u)) {\n", pad, dep);
        fprintf(f, "%s  x%u_is_dup = 1;\n", pad, dep);
        fprintf(f, "%s  x%u_loc = heap_alloc(1);\n", pad, dep);
        fprintf(f, "%s  heap_set_rel(x%u_loc, x%u);\n", pad, dep, dep);
        fprintf(f, "%s}\n", pad);
        aot_emit_itrs_inc(f, pad);
        aot_emit_node(f, dup_loc + 1, dep + 1, out, 1, pad, tmp);
        return;
      }

      default: {
        char frm[32];
        char out_n[32];
        aot_emit_tmp(frm, sizeof(frm), "frm", tmp);
        aot_emit_tmp(out_n, sizeof(out_n), "out", tmp);

        fprintf(f, "%sif (*s_pos > base) {\n", pad);
        fprintf(f, "%sTerm %s = stack[*s_pos - 1];\n", pad1, frm);
        fprintf(f, "%sif (term_tag(%s) == APP) {\n", pad1, frm);
        aot_emit_ret_fallback_loc(f, loc, dep, pad2);
        fprintf(f, "%s}\n", pad1);
        fprintf(f, "%s}\n", pad);

        aot_emit_node(f, loc, dep, out_n, 0, pad, tmp);
        fprintf(f, "%sreturn %s;\n", pad, out_n);
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

    case VAR:
    case BJV: {
      u64 lvl = term_val(term);
      if (lvl == 0 || lvl > dep) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep);
        fprintf(f, ";\n");
        return;
      }

      u32 idx = (u32)(lvl - 1);
      fprintf(f, "%sTerm %s = x%u;\n", pad, out, idx);
      return;
    }

    case DP0:
    case BJ0: {
      u64 lvl = term_val(term);
      if (lvl == 0 || lvl > dep) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep);
        fprintf(f, ";\n");
        return;
      }

      u32 idx = (u32)(lvl - 1);
      u32 lab = term_ext(term);
      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%sif (x%u_is_dup) {\n", pad, idx);
      fprintf(f, "%s  %s = term_new(0, DP0, %u, x%u_loc);\n", pad, out, lab, idx);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%s  %s = x%u;\n", pad, out, idx);
      fprintf(f, "%s}\n", pad);
      return;
    }

    case DP1:
    case BJ1: {
      u64 lvl = term_val(term);
      if (lvl == 0 || lvl > dep) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep);
        fprintf(f, ";\n");
        return;
      }

      u32 idx = (u32)(lvl - 1);
      u32 lab = term_ext(term);
      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%sif (x%u_is_dup) {\n", pad, idx);
      fprintf(f, "%s  %s = term_new(0, DP1, %u, x%u_loc);\n", pad, out, lab, idx);
      fprintf(f, "%s} else {\n", pad);
      fprintf(f, "%s  %s = x%u;\n", pad, out, idx);
      fprintf(f, "%s}\n", pad);
      return;
    }

    case OP2: {
      u32 opr = term_ext(term);
      u64 arg = term_val(term);
      char lhs[32];
      char rhs[32];
      aot_emit_tmp(lhs, sizeof(lhs), "lhs", tmp);
      aot_emit_tmp(rhs, sizeof(rhs), "rhs", tmp);

      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%s{\n", pad);
      aot_emit_node(f, arg + 0, dep, lhs, 0, pad1, tmp);
      fprintf(f, "%sif (term_tag(%s) != NUM) {\n", pad1, lhs);
      fprintf(f, "%s%s = wnf(%s);\n", pad2, lhs, lhs);
      fprintf(f, "%s}\n", pad1);
      fprintf(f, "%sif (term_tag(%s) != NUM) {\n", pad1, lhs);
      fprintf(f, "%s%s = term_new_op2(%u, %s, ", pad2, out, opr, lhs);
      aot_emit_alo_expr(f, arg + 1, dep);
      fprintf(f, ");\n");
      fprintf(f, "%s} else {\n", pad1);
      aot_emit_node(f, arg + 1, dep, rhs, 0, pad2, tmp);
      fprintf(f, "%sif (term_tag(%s) != NUM) {\n", pad2, rhs);
      fprintf(f, "%s%s = wnf(%s);\n", pad3, rhs, rhs);
      fprintf(f, "%s}\n", pad2);
      fprintf(f, "%sif (term_tag(%s) != NUM) {\n", pad2, rhs);
      fprintf(f, "%s%s = term_new_op2(%u, %s, %s);\n", pad3, out, opr, lhs, rhs);
      fprintf(f, "%s} else {\n", pad2);
      fprintf(f, "%s%s = wnf_op2_num_num_raw(%u, (u32)term_val(%s), (u32)term_val(%s));\n", pad3, out, opr, lhs, rhs);
      fprintf(f, "%s}\n", pad2);
      fprintf(f, "%s}\n", pad1);
      fprintf(f, "%s}\n", pad);
      return;
    }

    case C01 ... C16: {
      if (dep == 0 && aot_emit_is_static_copy_free(loc)) {
        fprintf(f, "%sTerm %s = heap_read(%lluULL);\n", pad, out, (unsigned long long)loc);
      } else {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep);
        fprintf(f, ";\n");
      }
      return;
    }

    case DUP: {
      if (dep >= AOT_ENV_CAP) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep);
        fprintf(f, ";\n");
        return;
      }

      u64 dup_loc = term_val(term);
      char val[32];
      char bod[32];
      aot_emit_tmp(val, sizeof(val), "val", tmp);
      aot_emit_tmp(bod, sizeof(bod), "bod", tmp);

      fprintf(f, "%sTerm %s;\n", pad, out);
      fprintf(f, "%s{\n", pad);
      aot_emit_node(f, dup_loc + 0, dep, val, 0, pad1, tmp);
      fprintf(f, "%sTerm x%u = %s;\n", pad1, dep, val);
      fprintf(f, "%su8 x%u_is_dup = 0;\n", pad1, dep);
      fprintf(f, "%su64 x%u_loc = 0;\n", pad1, dep);
      fprintf(f, "%sif (!aot_is_copy_free(x%u)) {\n", pad1, dep);
      fprintf(f, "%s  x%u_is_dup = 1;\n", pad1, dep);
      fprintf(f, "%s  x%u_loc = heap_alloc(1);\n", pad1, dep);
      fprintf(f, "%s  heap_set_rel(x%u_loc, x%u);\n", pad1, dep, dep);
      fprintf(f, "%s}\n", pad1);
      aot_emit_itrs_inc(f, pad1);
      aot_emit_node(f, dup_loc + 1, dep + 1, bod, 0, pad1, tmp);
      fprintf(f, "%s%s = %s;\n", pad1, out, bod);
      fprintf(f, "%s}\n", pad);
      return;
    }

    case APP: {
      u64 app_loc = term_val(term);
      Term fun_term = heap_read(app_loc + 0);

      if (term_tag(fun_term) == LAM) {
        if (dep >= AOT_ENV_CAP) {
          fprintf(f, "%sTerm %s = ", pad, out);
          aot_emit_alo_expr(f, loc, dep);
          fprintf(f, ";\n");
          return;
        }

        u64 lam_loc = term_val(fun_term);
        fprintf(f, "%sTerm x%u = ", pad, dep);
        aot_emit_alo_expr(f, app_loc + 1, dep);
        fprintf(f, ";\n");
        fprintf(f, "%su8 x%u_is_dup = 0;\n", pad, dep);
        fprintf(f, "%su64 x%u_loc = 0;\n", pad, dep);
        aot_emit_itrs_inc(f, pad);
        aot_emit_node(f, lam_loc, dep + 1, out, 0, pad, tmp);
        return;
      }

      u32 ref_id = 0;
      u64 arg_locs[AOT_ARG_CAP];
      u16 arg_len = 0;
      int has_fun = 0;
      char fun_name[256];

      if (!aot_emit_collect_app_ref(loc, &ref_id, arg_locs, &arg_len)) {
        fprintf(f, "%sTerm %s = ", pad, out);
        aot_emit_alo_expr(f, loc, dep);
        fprintf(f, ";\n");
        return;
      }

      if (ref_id < TABLE.len && BOOK[ref_id] != 0) {
        char *ref_name = table_get(ref_id);
        if (ref_name != NULL) {
          aot_emit_fun_name(fun_name, sizeof(fun_name), ref_name);
          has_fun = 1;
        }
      }

      if (arg_len == 0) {
        if (has_fun) {
          fprintf(f, "%sTerm %s = aot_call_ref_direct(%s, %u, 0, NULL);\n", pad, out, fun_name, ref_id);
        } else {
          fprintf(f, "%sTerm %s = aot_call_ref(%u, 0, NULL);\n", pad, out, ref_id);
        }
        return;
      }

      char call_args[32];
      aot_emit_tmp(call_args, sizeof(call_args), "call_args", tmp);
      fprintf(f, "%sTerm %s[%u];\n", pad, call_args, arg_len);

      int strict0 = aot_emit_ref_arg0_strict(ref_id);
      u16 beg = 0;

      if (strict0) {
        char arg0[32];
        aot_emit_tmp(arg0, sizeof(arg0), "arg", tmp);
        aot_emit_node(f, arg_locs[arg_len - 1], dep, arg0, 0, pad, tmp);
        fprintf(f, "%s%s[0] = %s;\n", pad, call_args, arg0);
        beg = 1;
      }

      for (u16 i = beg; i < arg_len; i++) {
        u64 arg_loc = arg_locs[arg_len - 1 - i];
        Term arg_term = heap_read(arg_loc);
        u8 arg_tag = term_tag(arg_term);

        if (arg_tag == VAR || arg_tag == BJV || arg_tag == DP0 || arg_tag == BJ0 || arg_tag == DP1 || arg_tag == BJ1) {
          char arg_n[32];
          aot_emit_tmp(arg_n, sizeof(arg_n), "arg", tmp);
          aot_emit_node(f, arg_loc, dep, arg_n, 0, pad, tmp);
          fprintf(f, "%s%s[%u] = %s;\n", pad, call_args, i, arg_n);
        } else if (aot_emit_is_closed(arg_loc, dep) && aot_emit_is_static_copy_free(arg_loc)) {
          fprintf(f, "%s%s[%u] = heap_read(%lluULL);\n", pad, call_args, i, (unsigned long long)arg_loc);
        } else {
          fprintf(f, "%s%s[%u] = ", pad, call_args, i);
          aot_emit_alo_expr(f, arg_loc, dep);
          fprintf(f, ";\n");
        }
      }

      if (has_fun) {
        fprintf(f, "%sTerm %s = aot_call_ref_direct(%s, %u, %u, %s);\n", pad, out, fun_name, ref_id, arg_len, call_args);
      } else {
        fprintf(f, "%sTerm %s = aot_call_ref(%u, %u, %s);\n", pad, out, ref_id, arg_len, call_args);
      }
      return;
    }

    default: {
      fprintf(f, "%sTerm %s = ", pad, out);
      aot_emit_alo_expr(f, loc, dep);
      fprintf(f, ";\n");
      return;
    }
  }
}

// Definition Emitter
// ------------------

// Emits one forward declaration for a compiled definition.
fn void aot_emit_decl(FILE *f, u32 id) {
  if (BOOK[id] == 0) {
    return;
  }

  char *name = table_get(id);
  if (name == NULL) {
    return;
  }

  char fun_name[256];
  aot_emit_fun_name(fun_name, sizeof(fun_name), name);
  fprintf(f, "static Term %s(Term *stack, u32 *s_pos, u32 base);\n", fun_name);
}

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
  fprintf(f, "static Term %s(Term *stack, u32 *s_pos, u32 base) {\n", fun_name);
  fprintf(f, "  if (aot_call_depth() >= AOT_MAX_DEPTH) {\n");
  fprintf(f, "    return aot_fallback_alo(%lluULL, 0, NULL);\n", (unsigned long long)root);
  fprintf(f, "  }\n");
  fprintf(f, "\n");
  {
    u32 tmp = 0;
    aot_emit_node(f, root, 0, NULL, 1, "  ", &tmp);
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
    aot_emit_decl(f, id);
  }
  fprintf(f, "\n");

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
