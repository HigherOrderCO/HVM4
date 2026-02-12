fn Term cdef_body(u32 fid, u32 arity) {
  u32  loc  = BOOK[fid];
  Term term = heap_read(loc);
  for (u32 i = 0; i < arity; i++) {
    if (term_tag(term) != LAM) {
      return term;
    }
    loc  = term_val(term);
    term = heap_read(loc + 0);
  }
  return term;
}

fn u8 cdef_is_simple(Term term, u32 arity) {
  switch (term_tag(term)) {
    case BJV: {
      u32 lvl = term_val(term);
      return term_ext(term) == 0 && lvl >= 1 && lvl <= arity;
    }
    case NUM:
    case REF: {
      return 1;
    }
    case APP: {
      u32  loc = term_val(term);
      Term fun = heap_read(loc + 0);
      Term arg = heap_read(loc + 1);
      return cdef_is_simple(fun, arity) && cdef_is_simple(arg, arity);
    }
    default: {
      return 0;
    }
  }
}

fn void cdef_emit_expr(FILE *out, Term term) {
  switch (term_tag(term)) {
    case BJV: {
      u32 idx = term_val(term) - 1;
      fprintf(out, "args[%u]", idx);
      return;
    }
    case NUM: {
      fprintf(out, "API->term_new_num(%u)", term_val(term));
      return;
    }
    case REF: {
      fprintf(out, "API->term_new_ref(%u)", term_ext(term));
      return;
    }
    case APP: {
      u32  loc = term_val(term);
      Term fun = heap_read(loc + 0);
      Term arg = heap_read(loc + 1);
      fprintf(out, "API->term_new_app(");
      cdef_emit_expr(out, fun);
      fprintf(out, ", ");
      cdef_emit_expr(out, arg);
      fprintf(out, ")");
      return;
    }
    default: {
      fprintf(out, "0");
      return;
    }
  }
}

fn void cdef_emit_api(FILE *out) {
  static const char prelude[] =
    "typedef __UINT32_TYPE__ u32;\n"
    "typedef __UINT64_TYPE__ Term;\n"
    "\n";
  size_t prelude_len = sizeof(prelude) - 1;
  if (fwrite(prelude, 1, prelude_len, out) != prelude_len) {
    sys_error("compiled-def codegen failed while writing ABI prelude");
  }

  size_t blob_len = (size_t)CDEF_API_BLOB_LEN;
  if (fwrite(CDEF_API_BLOB, 1, blob_len, out) != blob_len) {
    sys_error("compiled-def codegen failed while writing ABI blob");
  }
  if (fputc('\n', out) == EOF) {
    sys_error("compiled-def codegen failed while finalizing ABI text");
  }
}

fn void cdef_gen(const char *c_path) {
  FILE *out = fopen(c_path, "wb");
  if (out == NULL) {
    sys_error("compiled-def codegen failed to open output file");
  }

  u8 *simple = (u8*)calloc(CDEF_CAP, sizeof(u8));
  if (simple == NULL) {
    sys_error("compiled-def codegen metadata allocation failed");
  }

  cdef_emit_api(out);
  fprintf(out, "static const Hvm4DefApi *API = 0;\n");
  fprintf(out, "\n");

  for (u32 fid = 0; fid < CDEF_CAP; fid++) {
    if (BOOK[fid] == 0) {
      continue;
    }
    u32  arity = CDEF_ARIS[fid];
    Term body  = cdef_body(fid, arity);
    simple[fid] = cdef_is_simple(body, arity);

    fprintf(out, "static Term def_%u(Term *args, u32 argc) {\n", fid);
    if (simple[fid]) {
      fprintf(out, "  return ");
      cdef_emit_expr(out, body);
      fprintf(out, ";\n");
    } else {
      fprintf(out, "  return API->def_apply(%u, args, argc);\n", fid);
    }
    fprintf(out, "}\n");
    fprintf(out, "\n");
  }

  fprintf(out, "void hvm4_register(const Hvm4DefApi *api, Hvm4DefFun *out_fun, u32 *out_ari, u32 out_cap) {\n");
  fprintf(out, "  API = api;\n");
  fprintf(out, "\n");
  for (u32 fid = 0; fid < CDEF_CAP; fid++) {
    if (BOOK[fid] == 0 || !simple[fid]) {
      continue;
    }
    fprintf(out, "  if (%u < out_cap) {\n", fid);
    fprintf(out, "    out_fun[%u] = def_%u;\n", fid, fid);
    fprintf(out, "    out_ari[%u] = %u;\n", fid, CDEF_ARIS[fid]);
    fprintf(out, "  }\n");
  }
  fprintf(out, "}\n");

  free(simple);
  fclose(out);
}
