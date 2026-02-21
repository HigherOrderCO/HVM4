// Pretty-printer overview
// - Dynamic links: LAM/VAR and DP0/DP1 point to heap locations; DUP is a
//   syntactic binder; it yields a DUP node (DP0/DP1 share its expr loc).
// - Static terms (inside ALO) are immutable and use BJV/BJ0/BJ1 de Bruijn levels.
// - NAM is a literal stuck name (^x), unrelated to binders.
// - Dynamic printing assigns globally unique names to each LAM body location.
// - Dup names are keyed by the DUP node expr location and printed after the term.
// - Static printing renders quoted/book terms and applies ALO substitutions.
// - Substitutions live in heap slots with the SUB bit set; these must be
//   unmarked before printing, and print_term_at asserts this invariant.
// - Lambda names: lowercase (a, b, ..., aa, ab, ...), dup names: uppercase.
// - Quoted lambdas are tagged by LAM.ext = depth + 1 (linked LAM.ext = 0).
// - Name tables are fixed-size (PRINT_NAME_MAX) to keep the printer simple.

typedef struct {
  u64 loc;
  u32 name;
} LamBind;

// DupBind records a DUP node keyed by its expr location.
typedef struct {
  u64 loc;
  u32 name;
  u32 lab;
} DupBind;

// PrintState keeps naming tables and ALO printing mode.
// - quoted/subst/subst_len: current printing mode, bind list head, and length.
// Fixed-size tables: keep naming simple and bounded.
#define PRINT_NAME_MAX 65536
static LamBind PRINT_LAMS[PRINT_NAME_MAX];
static DupBind PRINT_DUPS[PRINT_NAME_MAX];

typedef struct {
  u32 lam_len;
  u32 dup_len;
  u32 dup_print;
  u32 next_lam;
  u32 next_dup;
  u8  quoted;
  u64 subst;
  u32 subst_len;
} PrintState;

// Core recursive printer; always called through print_term_at.
fn void print_term_go(FILE *f, Term term, u32 depth, PrintState *st);
// Guards against printing a term with the SUB bit set.
fn void print_term_at(FILE *f, Term term, u32 depth, PrintState *st) {
  assert(!term_sub_get(term));
  print_term_go(f, term, depth, st);
}

// Temporarily switches print mode (quoted + subst) for nested ALO rendering.
fn void print_term_mode(FILE *f, Term term, u32 depth, u8 quoted, u64 subst, u32 subst_len, PrintState *st) {
  u8  old_quoted = st->quoted;
  u64 old_subst  = st->subst;
  u32 old_len    = st->subst_len;
  st->quoted = quoted;
  st->subst  = subst;
  st->subst_len = quoted ? subst_len : 0;
  print_term_at(f, term, depth, st);
  st->quoted = old_quoted;
  st->subst  = old_subst;
  st->subst_len = old_len;
}

// Base-26 alpha printer: 1->a, 26->z, 27->aa. 0 prints '_' for unscoped vars.
fn void print_alpha_name(FILE *f, u32 n, char base) {
  if (n == 0) {
    fputc('_', f);
    return;
  }
  char buf[32];
  u32  len = 0;
  while (n > 0) {
    n--;
    buf[len++] = (char)(base + (n % 26));
    n /= 26;
  }
  for (u32 i = 0; i < len; i++) {
    fputc(buf[len - 1 - i], f);
  }
}

// Emits a lambda name (lowercase alpha).
fn void print_lam_name(FILE *f, u32 name) {
  print_alpha_name(f, name, 'a');
}

// Emits a dup name (uppercase alpha).
fn void print_dup_name(FILE *f, u32 name) {
  print_alpha_name(f, name, 'A');
}

// Initializes the printer state and name counters.
fn void print_state_init(PrintState *st) {
  memset(st, 0, sizeof(*st));
  st->next_lam    = 1;
  st->next_dup    = 1;
}

// No-op for fixed tables; kept for symmetry with print_state_init.
fn void print_state_free(PrintState *st) {
  (void)st;
}

// Returns the global name for a lambda body location, allocating if needed.
fn u32 print_state_lam(PrintState *st, u64 loc) {
  for (u32 i = 0; i < st->lam_len; i++) {
    if (PRINT_LAMS[i].loc == loc) {
      return PRINT_LAMS[i].name;
    }
  }
  if (st->lam_len >= PRINT_NAME_MAX) {
    fprintf(stderr, "print_state: too many lambdas\n");
    exit(1);
  }
  u32 name = st->next_lam++;
  PRINT_LAMS[st->lam_len] = (LamBind){.loc = loc, .name = name};
  st->lam_len++;
  return name;
}

// Returns the global name for a DUP node keyed by its expr location.
fn u32 print_state_dup(PrintState *st, u64 loc, u32 lab) {
  for (u32 i = 0; i < st->dup_len; i++) {
    if (PRINT_DUPS[i].loc == loc) {
      return PRINT_DUPS[i].name;
    }
  }
  if (st->dup_len >= PRINT_NAME_MAX) {
    fprintf(stderr, "print_state: too many dups\n");
    exit(1);
  }
  u32 name = st->next_dup++;
  PRINT_DUPS[st->dup_len] = (DupBind){.loc = loc, .name = name, .lab = lab};
  st->dup_len++;
  return name;
}

// Looks up an ALO bind list entry by index (0 = innermost), returning a dynamic loc.
fn u64 alo_subst_get(u64 ls_loc, u32 idx) {
  u64 ls = ls_loc;
  for (u32 i = 0; i < idx && ls != 0; i++) {
    ls = term_val(HEAP[ls + 1]);
  }
  return ls;
}

// Prints an interned name (used by refs/primitives), with fallback for unknown ids.
fn void print_def_name(FILE *f, u32 nam) {
  char *name = table_get(nam);
  if (name != NULL) {
    fputs(name, f);
  } else {
    print_name(f, nam);
  }
}

// Prints an interned name (used by ctors/labels/stuck names), with fallback for unknown ids.
fn void print_sym_name(FILE *f, u32 nam) {
  char *name = table_get(nam);
  if (name != NULL) {
    fputs(name, f);
  } else {
    print_name(f, nam);
  }
}


// Prints match constructor labels with special sugar for nat/list forms.
fn void print_mat_name(FILE *f, u32 nam) {
  if (nam == SYM_ZER) {
    fputs("0n", f);
  } else if (nam == SYM_SUC) {
    fputs("1n+", f);
  } else if (nam == SYM_NIL) {
    fputs("[]", f);
  } else if (nam == SYM_CON) {
    fputs("<>", f);
  } else {
    fputc('#', f);
    print_sym_name(f, nam);
  }
}

// Prints APP/DRY spines as f(x,y,...) with a parenthesis around lambdas.
fn void print_app(FILE *f, Term term, u32 depth, PrintState *st) {
  Term spine[256];
  u32  len  = 0;
  Term curr = term;
  while ((term_tag(curr) == APP || term_tag(curr) == DRY) && len < 256) {
    u64 loc = term_val(curr);
    spine[len++] = HEAP[loc + 1];
    curr = HEAP[loc];
  }
  if (term_tag(curr) == LAM) {
    fputc('(', f);
    print_term_at(f, curr, depth, st);
    fputc(')', f);
  } else {
    print_term_at(f, curr, depth, st);
  }
  fputc('(', f);
  for (u32 i = 0; i < len; i++) {
    if (i > 0) {
      fputc(',', f);
    }
    print_term_at(f, spine[len - 1 - i], depth, st);
  }
  fputc(')', f);
}

// Prints constructors, with sugar for nat, char, string, and list forms.
fn void print_ctr(FILE *f, Term t, u32 d, PrintState *st) {
  u32 nam = term_ext(t), ari = term_tag(t) - C00;
  u64 loc = term_val(t);
  // Nat: count SUCs, print as Nn or Nn+x
  if (nam == SYM_ZER || nam == SYM_SUC) {
    u32 n = 0;
    while (term_tag(t) == C01 && term_ext(t) == SYM_SUC) {
      n++;
      t = HEAP[term_val(t)];
    }
    fprintf(f, "%un", n);
    if (!(term_tag(t) == C00 && term_ext(t) == SYM_ZER)) {
      fputc('+', f);
      print_term_at(f, t, d, st);
    }
    return;
  }
  // Char: 'x' or '\n'
  if (nam == SYM_CHR && ari == 1 && term_tag(HEAP[loc]) == NUM) {
    u32 c = term_val(HEAP[loc]);
    if (print_utf8_can_escape(c, PRINT_ESC_CHAR)) {
      fputc('\'', f);
      print_utf8_escape(f, c, PRINT_ESC_CHAR);
      fputc('\'', f);
      return;
    }
  }
  // List/String
  if (nam == SYM_NIL || nam == SYM_CON) {
    // Check if string (non-empty, all escapable chars)
    int is_str = (nam == SYM_CON);
    for (Term x = t; term_tag(x) == C02 && term_ext(x) == SYM_CON; x = HEAP[term_val(x) + 1]) {
      Term h = HEAP[term_val(x)];
      if (!(term_tag(h) == C01 && term_ext(h) == SYM_CHR)) {
        is_str = 0;
        break;
      }
      if (term_tag(HEAP[term_val(h)]) != NUM) {
        is_str = 0;
        break;
      }
      u32 c = term_val(HEAP[term_val(h)]);
      if (!print_utf8_can_escape(c, PRINT_ESC_STR)) {
        is_str = 0;
        break;
      }
    }
    Term end = t;
    while (term_tag(end) == C02 && term_ext(end) == SYM_CON) {
      end = HEAP[term_val(end) + 1];
    }
    if (is_str && term_tag(end) == C00 && term_ext(end) == SYM_NIL) {
      fputc('"', f);
      for (Term x = t; term_tag(x) == C02; x = HEAP[term_val(x) + 1]) {
        u32 c = term_val(HEAP[term_val(HEAP[term_val(x)])]);
        print_utf8_escape(f, c, PRINT_ESC_STR);
      }
      fputc('"', f);
      return;
    }
    // Proper list: [a,b,c]
    if (term_tag(end) == C00 && term_ext(end) == SYM_NIL) {
      fputc('[', f);
      for (Term x = t; term_tag(x) == C02; x = HEAP[term_val(x) + 1]) {
        if (x != t) {
          fputc(',', f);
        }
        print_term_at(f, HEAP[term_val(x)], d, st);
      }
      fputc(']', f);
      return;
    }
    // Improper list: h<>t
    if (nam == SYM_CON) {
      print_term_at(f, HEAP[loc], d, st);
      fputs("<>", f);
      print_term_at(f, HEAP[loc + 1], d, st);
      return;
    }
  }
  // Default CTR
  fputc('#', f);
  print_sym_name(f, nam);
  fputc('{', f);
  for (u32 i = 0; i < ari; i++) {
    if (i) {
      fputc(',', f);
    }
    print_term_at(f, HEAP[loc + i], d, st);
  }
  fputc('}', f);
}

// Recursive printer that handles both dynamic (linked) and quoted (book) terms.
fn void print_term_go(FILE *f, Term term, u32 depth, PrintState *st) {
  u8  quoted = st->quoted;
  u64 subst  = st->subst;
  switch (term_tag(term)) {
    case NAM: {
      // Literal stuck name (^x).
      print_name(f, term_ext(term));
      break;
    }
    case DRY: {
      // Stuck application ^(f x) rendered as f(x).
      print_app(f, term, depth, st);
      break;
    }
    case BJV: {
      // Quoted VAR: val is de Bruijn level; try ALO substitution.
      u64 lvl  = term_val(term);
      u64 bind = 0;
      if (quoted && lvl > 0 && lvl <= st->subst_len) {
        bind = alo_subst_get(subst, st->subst_len - lvl);
      }
      if (bind != 0) {
        Term val = HEAP[bind];
        if (term_sub_get(val)) {
          val = term_sub_set(val, 0);
          print_term_mode(f, val, depth, 0, 0, 0, st);
        } else {
          print_term_mode(f, term_new_var(bind), depth, 0, 0, 0, st);
        }
      } else {
        u32 nam = (lvl > st->subst_len) ? (lvl - st->subst_len) : 0;
        if (nam > depth) {
          nam = 0;
        }
        print_alpha_name(f, nam, 'a');
      }
      break;
    }
    case NUM: {
      fprintf(f, "%u", (u32)term_val(term));
      break;
    }
    case REF: {
      fputc('@', f);
      print_def_name(f, term_ext(term));
      break;
    }
    case PRI: {
      fputc('%', f);
      print_def_name(f, term_ext(term));
      u32 ari = term_arity(term);
      if (ari > 0) {
        u64 loc = term_val(term);
        fputc('(', f);
        for (u32 i = 0; i < ari; i++) {
          if (i > 0) {
            fputc(',', f);
          }
          print_term_at(f, HEAP[loc + i], depth, st);
        }
        fputc(')', f);
      }
      break;
    }
    case ERA: {
      fputs("&{}", f);
      break;
    }
    case ANY: {
      fputc('*', f);
      break;
    }
    case BJ0:
    case BJ1: {
      // Quoted BJ_: val is de Bruijn level; try ALO substitution.
      u64 lvl  = term_val(term);
      u64 bind = 0;
      if (quoted && lvl > 0 && lvl <= st->subst_len) {
        bind = alo_subst_get(subst, st->subst_len - lvl);
      }
      if (bind != 0) {
        Term val = HEAP[bind];
        if (term_sub_get(val)) {
          val = term_sub_set(val, 0);
          print_term_mode(f, val, depth, 0, 0, 0, st);
        } else {
          u8  tag = term_tag(term) == BJ0 ? DP0 : DP1;
          u32 lab = term_ext(term);
          print_term_mode(f, term_new(0, tag, lab, bind), depth, 0, 0, 0, st);
        }
      } else {
        u32 nam = (lvl > st->subst_len) ? (lvl - st->subst_len) : 0;
        if (nam > depth) {
          nam = 0;
        }
        if (nam == 0) {
          fputc('_', f);
        } else {
          print_alpha_name(f, nam, 'A');
        }
        fputs(term_tag(term) == BJ0 ? "₀" : "₁", f);
      }
      break;
    }
    case VAR: {
      // Runtime VAR: val is binding lam body location.
      u64 loc = term_val(term);
      if (loc != 0 && term_sub_get(HEAP[loc])) {
        print_term_mode(f, term_sub_set(HEAP[loc], 0), depth, 0, 0, 0, st);
      } else {
        u32 nam = print_state_lam(st, loc);
        print_lam_name(f, nam);
      }
      break;
    }
    case DP0:
    case DP1: {
      // Runtime DP_: val is a DUP node expr location.
      u64 loc = term_val(term);
      if (loc != 0 && term_sub_get(HEAP[loc])) {
        print_term_mode(f, term_sub_set(HEAP[loc], 0), depth, 0, 0, 0, st);
      } else {
        u32 nam = print_state_dup(st, loc, term_ext(term));
        print_dup_name(f, nam);
        fputs(term_tag(term) == DP0 ? "₀" : "₁", f);
      }
      break;
    }
    case LAM: {
      // Quoted mode uses depth-based names; dynamic mode uses global naming.
      u64 loc = term_val(term);
      fputs("λ", f);
      if (quoted) {
        print_alpha_name(f, depth + 1, 'a');
        fputc('.', f);
        print_term_at(f, HEAP[loc], depth + 1, st);
      } else {
        u32 nam = print_state_lam(st, loc);
        print_lam_name(f, nam);
        fputc('.', f);
        print_term_at(f, HEAP[loc], depth + 1, st);
      }
      break;
    }
    case APP: {
      print_app(f, term, depth, st);
      break;
    }
    case SUP: {
      u64 loc = term_val(term);
      fputc('&', f);
      print_name(f, term_ext(term));
      fputc('{', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputc(',', f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc('}', f);
      break;
    }
    case DUP: {
      // DUP term is a syntactic binder; dynamic mode queues its DUP node and prints the body.
      u64 loc = term_val(term);
      if (quoted) {
        fputc('!', f);
        print_alpha_name(f, depth + 1, 'A');
        fputc('&', f);
        print_name(f, term_ext(term));
        fputc('=', f);
        print_term_at(f, HEAP[loc + 0], depth, st);
        fputc(';', f);
        print_term_at(f, HEAP[loc + 1], depth + 1, st);
      } else {
        print_state_dup(st, loc, term_ext(term));
        print_term_at(f, HEAP[loc + 1], depth, st);
      }
      break;
    }
    case MAT:
    case SWI: {
      fputs("λ{", f);
      Term cur = term;
      while (term_tag(cur) == MAT || term_tag(cur) == SWI) {
        u64 loc = term_val(cur);
        if (term_tag(cur) == SWI) {
          fprintf(f, "%u", term_ext(cur));
        } else {
          print_mat_name(f, term_ext(cur));
        }
        fputc(':', f);
        print_term_at(f, HEAP[loc + 0], depth, st);
        Term next = HEAP[loc + 1];
        if (term_tag(next) == MAT || term_tag(next) == SWI) {
          fputc(';', f);
        }
        cur = next;
      }
      // Handle tail: NUM(0) = empty, USE = wrapped default, other = default.
      if (term_tag(cur) == NUM && term_val(cur) == 0) {
        // empty default - just close
      } else if (term_tag(cur) == USE) {
        fputc(';', f);
        print_term_at(f, HEAP[term_val(cur)], depth, st);
      } else {
        fputc(';', f);
        print_term_at(f, cur, depth, st);
      }
      fputc('}', f);
      break;
    }
    case USE: {
      u64 loc = term_val(term);
      fputs("λ{", f);
      print_term_at(f, HEAP[loc], depth, st);
      fputc('}', f);
      break;
    }
    case C00 ... C16: {
      print_ctr(f, term, depth, st);
      break;
    }
    case OP2: {
      u32 opr = term_ext(term);
      u64 loc = term_val(term);
      static const char *op_syms[] = {
        "+", "-", "*", "/", "%", "&&", "||", "^", "<<", ">>",
        "~", "==", "!=", "<", "<=", ">", ">="
      };
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputc(' ', f);
      if (opr < 17) {
        fputs(op_syms[opr], f);
      } else {
        fprintf(f, "?%u", opr);
      }
      fputc(' ', f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case DSU: {
      u64 loc = term_val(term);
      fputs("&(", f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs("){", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(',', f);
      print_term_at(f, HEAP[loc + 2], depth, st);
      fputc('}', f);
      break;
    }
    case DDU: {
      u64 loc = term_val(term);
      fputs("!(", f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(")=", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(';', f);
      print_term_at(f, HEAP[loc + 2], depth, st);
      break;
    }
    case ALO: {
      // ALO prints as @{book_term}, applying ALO substitutions to book vars.
      u16 len     = term_ext(term);
      u64 tm_loc;
      u64 ls_loc;
      if (len == 0) {
        tm_loc = term_val(term);
        ls_loc = 0;
      } else {
        u64 alo_loc = term_val(term);
        u64 pair = HEAP[alo_loc];
        ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
        tm_loc = pair & ALO_TM_MASK;
      }
      fputs("@{", f);
      print_term_mode(f, HEAP[tm_loc], 0, 1, ls_loc, len, st);
      fputc('}', f);
      break;
    }
    case EQL: {
      u64 loc = term_val(term);
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(" === ", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case AND: {
      u64 loc = term_val(term);
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(" .&. ", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case OR: {
      u64 loc = term_val(term);
      fputc('(', f);
      print_term_at(f, HEAP[loc + 0], depth, st);
      fputs(" .|. ", f);
      print_term_at(f, HEAP[loc + 1], depth, st);
      fputc(')', f);
      break;
    }
    case UNS: {
      // UNS binds an unscoped lam/var pair; show them with global names.
      u64 loc   = term_val(term);
      Term lamf = HEAP[loc];
      u64 locf  = term_val(lamf);
      Term lamv = HEAP[locf];
      u64 locv  = term_val(lamv);
      u32 namf  = print_state_lam(st, locf);
      u32 namv  = print_state_lam(st, locv);
      Term body = HEAP[locv];
      fputs("! ", f);
      print_lam_name(f, namf);
      fputs(" = λ ", f);
      print_lam_name(f, namv);
      fputs(" ; ", f);
      print_term_at(f, body, depth + 2, st);
      break;
    }
    case INC: {
      u64 loc = term_val(term);
      fputs("↑", f);
      print_term_at(f, HEAP[loc], depth, st);
      break;
    }
  }
}

// Prints all discovered dup definitions after the main term.
fn void print_term_finish(FILE *f, PrintState *st) {
  int need_sep = (st->dup_print == 0);
  while (st->dup_print < st->dup_len) {
    if (need_sep) {
      fputc(';', f);
      need_sep = 0;
    }
    u32 idx = st->dup_print++;
    u64 loc = PRINT_DUPS[idx].loc;
    u32 lab = PRINT_DUPS[idx].lab;
    u32 nam = PRINT_DUPS[idx].name;
    fputc('!', f);
    print_dup_name(f, nam);
    fputc('&', f);
    print_name(f, lab);
    fputc('=', f);
    Term val = HEAP[loc];
    if (term_sub_get(val)) {
      val = term_sub_set(val, 0);
    }
    print_term_at(f, val, 0, st);
    fputc(';', f);
  }
}

// Entry point that sets up state, prints the term, then prints floating dups.
fn void print_term_ex(FILE *f, Term term) {
  PrintState st;
  print_state_init(&st);
  print_term_at(f, term, 0, &st);
  print_term_finish(f, &st);
  print_state_free(&st);
}

// Prints a dynamic term (linked, global naming, deferred dup printing).
fn void print_term(Term term) {
  print_term_ex(stdout, term);
}

// Prints a static/quoted term to a custom stream at a given initial depth.
fn void print_term_quoted_ex(FILE *f, Term term, u32 depth) {
  PrintState st;
  print_state_init(&st);
  st.quoted = 1;
  st.subst  = 0;
  st.subst_len = 0;
  print_term_at(f, term, depth, &st);
  print_term_finish(f, &st);
  print_state_free(&st);
}

// Prints a static/quoted term (BJV/BJ0/BJ1) with depth-based lambda names.
fn void print_term_quoted(Term term) {
  print_term_quoted_ex(stdout, term, 0);
}
