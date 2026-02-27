// HVM AOT Runtime Core
// ====================
//
// This module exposes the tiny runtime surface used by generated AOT code.
//
// Model:
// - Each compiled definition is one stack-entry function: `F_<name>`.
// - Generated code is direct tree-shaped C with explicit fallback points.
// - Deopt returns residual runtime terms (ALO/app chains), never rewinds state.

// Type
// ----

typedef Term (*HvmAotFn)(Term *stack, u32 *s_pos, u32 base);

// Build-time runtime config embedded into generated AOT executables.
typedef struct {
  u32            threads;
  int            debug;
  RuntimeEvalCfg eval;
  u32            ffi_len;
  RuntimeFfiLoad ffi[RUNTIME_FFI_MAX];
} AotBuildCfg;

fn Term wnf_dup_nam(u32 lab, u64 loc, u8 side, Term nam);
fn Term wnf_dup_lam(u32 lab, u64 loc, u8 side, Term lam);
fn Term wnf_dup_sup(u32 lab, u64 loc, u8 side, Term sup);
fn Term wnf_dup_nod(u32 lab, u64 loc, u8 side, Term term);
fn Term wnf(Term term);

// Runtime
// -------

// Per-definition compiled entrypoint table (BOOK id -> native function).
static HvmAotFn AOT_FNS[BOOK_CAP] = {0};

// AOT runtime limits.
#define AOT_ARG_CAP   64
#define AOT_ENV_CAP   16
#define AOT_MAX_DEPTH 4096
#define AOT_TRY_CALL_MAX_DEPTH 1024
#define AOT_FORCE_DUP_FUEL 32
#define AOT_FORCE_STRICT_FUEL 64
#define AOT_DEOPT_SITE_CAP (1u << 16)
#define AOT_DEOPT_TOP_K    16

// Thread-local compiled-call depth for recursion cutoff.
static _Thread_local u32 AOT_CALL_DEPTH = 0;
static _Thread_local u8  AOT_TRY_CALL_ENABLED = 1;

// Counts one interaction on compiled paths.
fn void aot_itrs_inc(void) {
  if (ITRS_ENABLED) {
    ITRS++;
  }
}

// Adds a known number of interactions on compiled paths.
fn void aot_itrs_add(u64 amount) {
  if (ITRS_ENABLED) {
    ITRS += amount;
  }
}

// Fallback
// --------

typedef enum {
  AOT_DEOPT_HEAD_APP_ARG_CAP = 0,
  AOT_DEOPT_HEAD_LAM_ENV_CAP,
  AOT_DEOPT_HEAD_DUP_ENV_CAP,
  AOT_DEOPT_EXPR_VAR_LEVEL_OOB,
  AOT_DEOPT_EXPR_DP_LEVEL_OOB,
  AOT_DEOPT_EXPR_APP_UNSUPPORTED_FUN,
  AOT_DEOPT_EXPR_APP_ENV_CAP,
  AOT_DEOPT_EXPR_APP_ARG_CAPTURE,
  AOT_DEOPT_EXPR_CTR_FIELD_CAPTURE,
  AOT_DEOPT_EXPR_DUP_ENV_CAP,
  AOT_DEOPT_EXPR_OP2_LHS_CAPTURE,
  AOT_DEOPT_EXPR_OP2_RHS_CAPTURE,
  AOT_DEOPT_EXPR_UNSUPPORTED_TAG,
  AOT_DEOPT_CALL_DEPTH_CAP,
  AOT_DEOPT_REASON_COUNT,
} AotDeoptReason;

typedef enum {
  AOT_DEOPT_SITE_KIND_NONE = 0,
  AOT_DEOPT_SITE_KIND_HEAD_SWI_NON_NUM,
  AOT_DEOPT_SITE_KIND_HEAD_MAT_NON_CTR,
  AOT_DEOPT_SITE_KIND_EXPR_UNSUPPORTED_TAG,
  AOT_DEOPT_SITE_KIND_COUNT,
} AotDeoptSiteKind;

static int AOT_DEOPT_ENABLED = -1;
static u64 AOT_DEOPT_REASON_COUNTS[AOT_DEOPT_REASON_COUNT] = {0};
static u64 AOT_RESID_REASON_COUNTS[AOT_DEOPT_REASON_COUNT] = {0};
static u64 AOT_DEOPT_SITE_COUNTS[AOT_DEOPT_SITE_CAP] = {0};
static u64 AOT_RESID_SITE_COUNTS[AOT_DEOPT_SITE_CAP] = {0};
static u8  AOT_DEOPT_SITE_REASONS[AOT_DEOPT_SITE_CAP] = {0};
static u8  AOT_DEOPT_SITE_KINDS[AOT_DEOPT_SITE_CAP] = {0};
static u64 AOT_DEOPT_SITE_LOCS[AOT_DEOPT_SITE_CAP] = {0};
static u32 AOT_DEOPT_SITE_AUX[AOT_DEOPT_SITE_CAP] = {0};
static u64 AOT_DEOPT_SITE_OVERFLOW = 0;
static u64 AOT_RESID_SITE_OVERFLOW = 0;

// Returns one human-readable name for a deopt reason.
fn const char *aot_deopt_reason_name(u32 reason) {
  static const char *NAMES[AOT_DEOPT_REASON_COUNT] = {
    "HEAD_APP_ARG_CAP",
    "HEAD_LAM_ENV_CAP",
    "HEAD_DUP_ENV_CAP",
    "EXPR_VAR_LEVEL_OOB",
    "EXPR_DP_LEVEL_OOB",
    "EXPR_APP_UNSUPPORTED_FUN",
    "EXPR_APP_ENV_CAP",
    "EXPR_APP_ARG_CAPTURE",
    "EXPR_CTR_FIELD_CAPTURE",
    "EXPR_DUP_ENV_CAP",
    "EXPR_OP2_LHS_CAPTURE",
    "EXPR_OP2_RHS_CAPTURE",
    "EXPR_UNSUPPORTED_TAG",
    "CALL_DEPTH_CAP",
  };
  if (reason >= AOT_DEOPT_REASON_COUNT) {
    return "UNKNOWN";
  }
  return NAMES[reason];
}

// Returns one human-readable name for deopt site metadata kind.
fn const char *aot_deopt_site_kind_name(u32 kind) {
  static const char *NAMES[AOT_DEOPT_SITE_KIND_COUNT] = {
    "NONE",
    "HEAD_SWI_NON_NUM",
    "HEAD_MAT_NON_CTR",
    "EXPR_UNSUPPORTED_TAG",
  };
  if (kind >= AOT_DEOPT_SITE_KIND_COUNT) {
    return "UNKNOWN";
  }
  return NAMES[kind];
}

// Packs one static (tag,ext) pair into deopt site aux metadata.
fn u32 aot_deopt_aux_pack_tag_ext(u8 tag, u32 ext) {
  return ((u32)(tag & TAG_MASK) << EXT_BITS) | (ext & EXT_MASK);
}

// Unpacks one static tag from deopt site aux metadata.
fn u32 aot_deopt_aux_unpack_tag(u32 aux) {
  return (aux >> EXT_BITS) & TAG_MASK;
}

// Unpacks one static ext from deopt site aux metadata.
fn u32 aot_deopt_aux_unpack_ext(u32 aux) {
  return aux & EXT_MASK;
}

// Returns one human-readable name for a term tag.
fn const char *aot_term_tag_name(u32 tag) {
  static const char *NAMES[TAG_MASK + 1] = {
    [APP] = "APP",
    [VAR] = "VAR",
    [LAM] = "LAM",
    [DP0] = "DP0",
    [DP1] = "DP1",
    [SUP] = "SUP",
    [DUP] = "DUP",
    [ALO] = "ALO",
    [REF] = "REF",
    [NAM] = "NAM",
    [DRY] = "DRY",
    [ERA] = "ERA",
    [MAT] = "MAT",
    [C00] = "C00",
    [C01] = "C01",
    [C02] = "C02",
    [C03] = "C03",
    [C04] = "C04",
    [C05] = "C05",
    [C06] = "C06",
    [C07] = "C07",
    [C08] = "C08",
    [C09] = "C09",
    [C10] = "C10",
    [C11] = "C11",
    [C12] = "C12",
    [C13] = "C13",
    [C14] = "C14",
    [C15] = "C15",
    [C16] = "C16",
    [NUM] = "NUM",
    [SWI] = "SWI",
    [USE] = "USE",
    [OP2] = "OP2",
    [DSU] = "DSU",
    [DDU] = "DDU",
    [EQL] = "EQL",
    [AND] = "AND",
    [OR]  = "OR",
    [UNS] = "UNS",
    [ANY] = "ANY",
    [INC] = "INC",
    [BJV] = "BJV",
    [BJ0] = "BJ0",
    [BJ1] = "BJ1",
    [PRI] = "PRI",
  };
  if (tag > TAG_MASK) {
    return "UNKNOWN";
  }
  if (NAMES[tag] == NULL) {
    return "UNKNOWN";
  }
  return NAMES[tag];
}

// Returns 1 when one reason is intentional residualization, not a hard bailout.
fn int aot_deopt_reason_is_residual(u32 reason) {
  return reason == AOT_DEOPT_EXPR_APP_ARG_CAPTURE
      || reason == AOT_DEOPT_EXPR_CTR_FIELD_CAPTURE
      || reason == AOT_DEOPT_EXPR_OP2_LHS_CAPTURE
      || reason == AOT_DEOPT_EXPR_OP2_RHS_CAPTURE;
}

// Returns 1 when deopt tracing is enabled via HVM_AOT_DEOPT.
fn int aot_deopt_enabled(void) {
  int enabled = __atomic_load_n(&AOT_DEOPT_ENABLED, __ATOMIC_RELAXED);
  if (enabled >= 0) {
    return enabled;
  }

  const char *env = getenv("HVM_AOT_DEOPT");
  enabled = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
  __atomic_store_n(&AOT_DEOPT_ENABLED, enabled, __ATOMIC_RELAXED);
  return enabled;
}

// Records one runtime fallback event by reason and emission site.
fn void aot_deopt_hit(u32 reason, u32 site) {
  if (!aot_deopt_enabled()) {
    return;
  }

  int residual = aot_deopt_reason_is_residual(reason);

  if (reason < AOT_DEOPT_REASON_COUNT) {
    if (residual) {
      __atomic_fetch_add(&AOT_RESID_REASON_COUNTS[reason], 1, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&AOT_DEOPT_REASON_COUNTS[reason], 1, __ATOMIC_RELAXED);
    }
  }

  if (site < AOT_DEOPT_SITE_CAP) {
    if (residual) {
      __atomic_fetch_add(&AOT_RESID_SITE_COUNTS[site], 1, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&AOT_DEOPT_SITE_COUNTS[site], 1, __ATOMIC_RELAXED);
    }
    if (reason < AOT_DEOPT_REASON_COUNT) {
      __atomic_store_n(&AOT_DEOPT_SITE_REASONS[site], (u8)reason, __ATOMIC_RELAXED);
    }
  } else {
    if (residual) {
      __atomic_fetch_add(&AOT_RESID_SITE_OVERFLOW, 1, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&AOT_DEOPT_SITE_OVERFLOW, 1, __ATOMIC_RELAXED);
    }
  }
}

// Registers static metadata for one emitted deopt site.
fn void aot_deopt_site_define(u32 site, u32 kind, u64 loc, u32 aux) {
  if (site >= AOT_DEOPT_SITE_CAP) {
    return;
  }
  if (kind < AOT_DEOPT_SITE_KIND_COUNT) {
    __atomic_store_n(&AOT_DEOPT_SITE_KINDS[site], (u8)kind, __ATOMIC_RELAXED);
  }
  __atomic_store_n(&AOT_DEOPT_SITE_LOCS[site], loc, __ATOMIC_RELAXED);
  __atomic_store_n(&AOT_DEOPT_SITE_AUX[site], aux, __ATOMIC_RELAXED);
}

// Inserts one (site,hits) pair into descending top-k buffers.
fn void aot_deopt_top_insert(u32 site, u64 hits, u32 *top_sites, u64 *top_hits) {
  for (u32 i = 0; i < AOT_DEOPT_TOP_K; i++) {
    if (hits <= top_hits[i]) {
      continue;
    }
    for (u32 j = AOT_DEOPT_TOP_K - 1; j > i; j--) {
      top_hits[j]  = top_hits[j - 1];
      top_sites[j] = top_sites[j - 1];
    }
    top_hits[i]  = hits;
    top_sites[i] = site;
    return;
  }
}

// Sums one reason-counter vector.
fn u64 aot_deopt_reason_total(const u64 *counts) {
  u64 total = 0;
  for (u32 reason = 0; reason < AOT_DEOPT_REASON_COUNT; reason++) {
    total += __atomic_load_n(&counts[reason], __ATOMIC_RELAXED);
  }
  return total;
}

// Prints reason counters for one telemetry stream.
fn void aot_deopt_dump_reasons(FILE *f, const char *prefix, const u64 *counts) {
  for (u32 reason = 0; reason < AOT_DEOPT_REASON_COUNT; reason++) {
    u64 count = __atomic_load_n(&counts[reason], __ATOMIC_RELAXED);
    if (count == 0) {
      continue;
    }
    fprintf(f, "%s reason[%u]=%s hits=%llu\n", prefix, reason, aot_deopt_reason_name(reason), (unsigned long long)count);
  }
}

// Prints top emission sites for one telemetry stream.
fn void aot_deopt_dump_top_sites(FILE *f, const char *prefix, const u64 *site_counts) {
  u32 top_sites[AOT_DEOPT_TOP_K] = {0};
  u64 top_hits[AOT_DEOPT_TOP_K] = {0};
  for (u32 site = 0; site < AOT_DEOPT_SITE_CAP; site++) {
    u64 hits = __atomic_load_n(&site_counts[site], __ATOMIC_RELAXED);
    if (hits == 0) {
      continue;
    }
    aot_deopt_top_insert(site, hits, top_sites, top_hits);
  }

  if (top_hits[0] == 0) {
    return;
  }

  fprintf(f, "%s top_sites:\n", prefix);
  for (u32 i = 0; i < AOT_DEOPT_TOP_K; i++) {
    if (top_hits[i] == 0) {
      break;
    }
    u32 site   = top_sites[i];
    u32 reason = __atomic_load_n(&AOT_DEOPT_SITE_REASONS[site], __ATOMIC_RELAXED);
    u32 kind   = __atomic_load_n(&AOT_DEOPT_SITE_KINDS[site], __ATOMIC_RELAXED);
    u64 loc    = __atomic_load_n(&AOT_DEOPT_SITE_LOCS[site], __ATOMIC_RELAXED);
    u32 aux    = __atomic_load_n(&AOT_DEOPT_SITE_AUX[site], __ATOMIC_RELAXED);

    fprintf(f, "%s site[%u] hits=%llu reason=%s", prefix, site, (unsigned long long)top_hits[i], aot_deopt_reason_name(reason));
    if (kind != AOT_DEOPT_SITE_KIND_NONE || loc != 0 || aux != 0) {
      fprintf(f, " kind=%s loc=%llu", aot_deopt_site_kind_name(kind), (unsigned long long)loc);
      if (kind == AOT_DEOPT_SITE_KIND_HEAD_SWI_NON_NUM) {
        fprintf(f, " expect_num=%u", aux);
      } else if (kind == AOT_DEOPT_SITE_KIND_HEAD_MAT_NON_CTR) {
        fprintf(f, " expect_ctr_ext=%u", aux);
      } else if (kind == AOT_DEOPT_SITE_KIND_EXPR_UNSUPPORTED_TAG) {
        u32 expr_tag = aot_deopt_aux_unpack_tag(aux);
        u32 expr_ext = aot_deopt_aux_unpack_ext(aux);
        fprintf(f, " expr_tag=%s(%u) expr_ext=%u", aot_term_tag_name(expr_tag), expr_tag, expr_ext);
      } else if (aux != 0) {
        fprintf(f, " aux=%u", aux);
      }
    }
    fprintf(f, "\n");
  }
}

// Prints deopt counters when tracing is enabled.
fn void aot_deopt_dump(FILE *f) {
  if (!aot_deopt_enabled()) {
    return;
  }

  u64 deopt_total = aot_deopt_reason_total(AOT_DEOPT_REASON_COUNTS);
  fprintf(f, "AOT_DEOPT total=%llu\n", (unsigned long long)deopt_total);
  if (deopt_total != 0) {
    aot_deopt_dump_reasons(f, "AOT_DEOPT", AOT_DEOPT_REASON_COUNTS);
    aot_deopt_dump_top_sites(f, "AOT_DEOPT", AOT_DEOPT_SITE_COUNTS);
    u64 overflow = __atomic_load_n(&AOT_DEOPT_SITE_OVERFLOW, __ATOMIC_RELAXED);
    if (overflow != 0) {
      fprintf(f, "AOT_DEOPT site_overflow=%llu cap=%u\n", (unsigned long long)overflow, AOT_DEOPT_SITE_CAP);
    }
  }

  u64 resid_total = aot_deopt_reason_total(AOT_RESID_REASON_COUNTS);
  fprintf(f, "AOT_RESID total=%llu\n", (unsigned long long)resid_total);
  if (resid_total != 0) {
    aot_deopt_dump_reasons(f, "AOT_RESID", AOT_RESID_REASON_COUNTS);
    aot_deopt_dump_top_sites(f, "AOT_RESID", AOT_RESID_SITE_COUNTS);
    u64 overflow = __atomic_load_n(&AOT_RESID_SITE_OVERFLOW, __ATOMIC_RELAXED);
    if (overflow != 0) {
      fprintf(f, "AOT_RESID site_overflow=%llu cap=%u\n", (unsigned long long)overflow, AOT_DEOPT_SITE_CAP);
    }
  }
}

// Rebuilds an ALO node from a live lexical environment bind-list head.
fn Term aot_fallback_alo(u64 tm_loc, u16 len, u64 ls_loc, u32 reason, u32 site) {
  aot_deopt_hit(reason, site);
  return term_new_alo(ls_loc, len, tm_loc);
}

// Reapplies arguments [from, argc) to a head term.
fn Term aot_reapply(Term head, u16 argc, const Term *args, u16 from) {
  for (u16 i = from; i < argc; i++) {
    head = term_new_app(head, args[i]);
  }
  return head;
}

// Returns one copy-free DUP value predicate used by generated code.
fn int aot_is_copy_free(Term term) {
  u8 tag = term_tag(term);
  if (tag == NUM) {
    return 1;
  }
  if (tag == C00) {
    return 1;
  }
  return 0;
}

// Returns 1 when strict head forcing can expose a new WHNF scrutinee tag.
fn int aot_can_force_strict_tag(u8 tag) {
  switch (tag) {
    case APP:
    case ALO:
    case REF: {
      return 1;
    }
    case OP2:
    case USE: {
      return 1;
    }
    default: {
      return 0;
    }
  }
}

// Forces one term to WHNF using a stack segment above caller-owned frames.
fn Term aot_force_whnf_local(Term term, u32 stack_top) {
  u32 saved_sp = WNF_S_POS;
  u8  saved_tc = AOT_TRY_CALL_ENABLED;
  if (stack_top == 0) {
    stack_top = 1;
  }
  WNF_S_POS = stack_top;
  AOT_TRY_CALL_ENABLED = 0;
  Term out = wnf(term);
  AOT_TRY_CALL_ENABLED = saved_tc;
  WNF_S_POS = saved_sp;
  return out;
}

// Tries bounded conservative DP forcing for a strict AOT scrutinee.
// Unknown payloads are restored and returned unchanged.
fn Term aot_force_dup(Term term) {
  for (u32 fuel = AOT_FORCE_DUP_FUEL; fuel > 0; --fuel) {
    u8 tm_tag = term_tag(term);
    if (tm_tag != DP0 && tm_tag != DP1) {
      return term;
    }

    u8  side = tm_tag == DP0 ? 0 : 1;
    u64 loc  = term_val(term);
    u32 lab  = term_ext(term);
    Term cell = heap_take(loc);

    if (term_sub_get(cell)) {
      Term val = term_sub_set(cell, 0);
      heap_set_rel(loc, cell);
      term = val;
      continue;
    }

    Term next;
    switch (term_tag(cell)) {
      case NAM:
      case BJV:
      case BJ0:
      case BJ1: {
        next = wnf_dup_nam(lab, loc, side, cell);
        break;
      }
      case LAM: {
        next = wnf_dup_lam(lab, loc, side, cell);
        break;
      }
      case SUP: {
        next = wnf_dup_sup(lab, loc, side, cell);
        break;
      }
      case DRY:
      case MAT:
      case SWI:
      case USE:
      case INC:
      case ERA:
      case ANY:
      case PRI:
      case NUM:
      case C00 ... C16: {
        next = wnf_dup_nod(lab, loc, side, cell);
        break;
      }
      default: {
        // Unknown/non-WNF payloads are left untouched so compiled code can deopt.
        heap_set_rel(loc, cell);
        return term;
      }
    }

    term = next;
  }
  return term;
}

// Tries conservative strict forcing for compiled SWI/MAT scrutinees.
fn Term aot_force_strict(Term term, u32 stack_top) {
  for (u32 fuel = AOT_FORCE_STRICT_FUEL; fuel > 0; --fuel) {
    Term prev = term;
    u8 tag = term_tag(term);
    if (tag == DP0 || tag == DP1) {
      term = aot_force_dup(term);
      if (term == prev) {
        return term;
      }
      continue;
    }
    if (aot_can_force_strict_tag(tag)) {
      term = aot_force_whnf_local(term, stack_top);
      if (term == prev) {
        return term;
      }
      continue;
    }
    return term;
  }
  return term;
}

// Calls
// -----

// Returns current compiled recursion depth.
fn u32 aot_call_depth(void) {
  return AOT_CALL_DEPTH;
}

// Calls one compiled ref using current stack slice, else returns residual REF application.
fn Term aot_call_ref(u32 ref_id, Term *stack, u32 *s_pos, u32 base) {
  if (AOT_CALL_DEPTH >= AOT_MAX_DEPTH) {
    return term_new_ref(ref_id);
  }

  HvmAotFn fun = AOT_FNS[ref_id];
  if (fun == NULL) {
    return term_new_ref(ref_id);
  }

  AOT_CALL_DEPTH++;
  Term out = fun(stack, s_pos, base);
  AOT_CALL_DEPTH--;

  return out;
}

// Dispatch
// --------

// Tries to execute a compiled function for a REF; returns 0 when absent.
fn int aot_try_call(u32 id, Term *stack, u32 *s_pos, u32 base, Term *out) {
  if (!AOT_TRY_CALL_ENABLED) {
    return 0;
  }

  if (AOT_CALL_DEPTH >= AOT_TRY_CALL_MAX_DEPTH || AOT_CALL_DEPTH >= AOT_MAX_DEPTH) {
    return 0;
  }

  if (STEPS_ITRS_LIM != 0) {
    return 0;
  }

  if (id >= BOOK_CAP) {
    return 0;
  }

  HvmAotFn fun = AOT_FNS[id];
  if (fun == NULL) {
    return 0;
  }

  AOT_CALL_DEPTH++;
  *out = fun(stack, s_pos, base);
  AOT_CALL_DEPTH--;
  return 1;
}

// Utils
// -----

// Converts a symbol name into a filesystem-safe alnum/_ identifier.
fn char *aot_sanitize(const char *name) {
  size_t len = strlen(name);
  size_t cap = (len * 4) + 1;
  char *out  = malloc(cap);
  if (out == NULL) {
    return NULL;
  }

  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    u8 c = (u8)name[i];
    if (isalnum(c) || c == '_') {
      out[j++] = (char)c;
      continue;
    }

    if (j + 4 >= cap) {
      free(out);
      return NULL;
    }

    static const char HEX[] = "0123456789ABCDEF";
    out[j++] = '_';
    out[j++] = 'x';
    out[j++] = HEX[(c >> 4) & 0xF];
    out[j++] = HEX[c & 0xF];
  }

  out[j] = '\0';
  return out;
}
