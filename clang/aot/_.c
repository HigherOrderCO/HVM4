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

typedef Term (*HvmAotFn)(Term *stack, u32 *s_pos, u32 base, Term *a_args, u32 *a_pos);

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
#define AOT_ENV_CAP   64
#define AOT_MAX_DEPTH 4096
#define AOT_TRY_CALL_MAX_DEPTH 1024
#define AOT_FORCE_DUP_FUEL 32
#define AOT_FORCE_STRICT_FUEL 64
#define AOT_DEOPT_SITE_CAP (1u << 16)
#define AOT_DEOPT_TOP_K    16

// Thread-local compiled-call depth for recursion cutoff.
static _Thread_local u32 AOT_CALL_DEPTH = 0;
static _Thread_local u8  AOT_TRY_CALL_ENABLED = 1;

// Heap attribution tracks allocator traffic caused while compiled code runs.
#define AOT_HEAP_BUCKET_COUNT 10
static int AOT_HEAP_ATTR_ENABLED = -1;
static u64 AOT_HEAP_CALLS = 0;
static u64 AOT_HEAP_NODES = 0;
static u64 AOT_HEAP_BUCKET_CALLS[AOT_HEAP_BUCKET_COUNT] = {0};
static u64 AOT_HEAP_BUCKET_NODES[AOT_HEAP_BUCKET_COUNT] = {0};
static u64 AOT_HEAP_KIND_CALLS[AOT_HEAP_KIND_COUNT] = {0};
static u64 AOT_HEAP_KIND_NODES[AOT_HEAP_KIND_COUNT] = {0};
static _Thread_local u32 AOT_HEAP_COMPILED_DEPTH = 0;
static int AOT_MAT_DP_ENABLED = -1;
static u64 AOT_MAT_DP_PROBES = 0;
static u64 AOT_MAT_DP_HITS = 0;
static u64 AOT_MAT_DP_NON_DP = 0;
static u64 AOT_MAT_DP_SUB = 0;
static u64 AOT_MAT_DP_NON_CTR = 0;
static u64 AOT_MAT_DP_EXT_MISS = 0;
static u64 AOT_MAT_DP_FORCE = 0;
static u64 AOT_MAT_DP_FORCE_TO_CTR = 0;

// Returns 1 when global heap attribution is enabled via HVM_AOT_HEAP_ATTR.
fn int aot_heap_attr_enabled(void) {
  int enabled = __atomic_load_n(&AOT_HEAP_ATTR_ENABLED, __ATOMIC_RELAXED);
  if (enabled >= 0) {
    return enabled;
  }

  const char *env = getenv("HVM_AOT_HEAP_ATTR");
  enabled = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
  __atomic_store_n(&AOT_HEAP_ATTR_ENABLED, enabled, __ATOMIC_RELAXED);
  return enabled;
}

// Maps one allocation size to one fixed histogram bucket index.
fn u32 aot_heap_bucket(u64 size) {
  if (size <= 8) {
    return (u32)(size - 1);
  }
  if (size <= 16) {
    return 8;
  }
  return 9;
}

// Returns one human-readable label for one heap-size histogram bucket.
fn const char *aot_heap_bucket_name(u32 bucket) {
  switch (bucket) {
    case 0: return "1";
    case 1: return "2";
    case 2: return "3";
    case 3: return "4";
    case 4: return "5";
    case 5: return "6";
    case 6: return "7";
    case 7: return "8";
    case 8: return "9-16";
    case 9: return "17+";
    default: return "?";
  }
}

// Returns one human-readable label for one heap-allocation kind.
fn const char *aot_heap_kind_name(u32 kind) {
  switch (kind) {
    case AOT_HEAP_KIND_NONE:         return "UNCLASSIFIED";
    case AOT_HEAP_KIND_TERM_APP:     return "TERM_APP";
    case AOT_HEAP_KIND_TERM_OP2:     return "TERM_OP2";
    case AOT_HEAP_KIND_TERM_CTR:     return "TERM_CTR";
    case AOT_HEAP_KIND_TERM_DUP:     return "TERM_DUP";
    case AOT_HEAP_KIND_TERM_SUP:     return "TERM_SUP";
    case AOT_HEAP_KIND_TERM_MAT:     return "TERM_MAT";
    case AOT_HEAP_KIND_TERM_SWI:     return "TERM_SWI";
    case AOT_HEAP_KIND_TERM_DRY:     return "TERM_DRY";
    case AOT_HEAP_KIND_TERM_DSU:     return "TERM_DSU";
    case AOT_HEAP_KIND_TERM_DDU:     return "TERM_DDU";
    case AOT_HEAP_KIND_TERM_ALO:     return "TERM_ALO";
    case AOT_HEAP_KIND_TERM_LAM:     return "TERM_LAM";
    case AOT_HEAP_KIND_TERM_USE:     return "TERM_USE";
    case AOT_HEAP_KIND_TERM_INC:     return "TERM_INC";
    case AOT_HEAP_KIND_TERM_UNS:     return "TERM_UNS";
    case AOT_HEAP_KIND_TERM_EQL:     return "TERM_EQL";
    case AOT_HEAP_KIND_TERM_AND:     return "TERM_AND";
    case AOT_HEAP_KIND_TERM_OR:      return "TERM_OR";
    case AOT_HEAP_KIND_TERM_PRI:     return "TERM_PRI";
    case AOT_HEAP_KIND_TERM_GENERIC: return "TERM_GENERIC";
    case AOT_HEAP_KIND_TERM_CLONE:   return "TERM_CLONE";
    case AOT_HEAP_KIND_AOT_APP_HEAD: return "AOT_APP_FRAME_HEAD";
    case AOT_HEAP_KIND_AOT_APP_MAT:  return "AOT_APP_FRAME_MAT";
    case AOT_HEAP_KIND_AOT_DUP_CELL: return "AOT_DUP_CELL";
    case AOT_HEAP_KIND_AOT_CTR:      return "AOT_CTR_FIELDS";
    case AOT_HEAP_KIND_AOT_ENV_BIND: return "AOT_ENV_BIND";
    case AOT_HEAP_KIND_WNF_ALO_DUP:  return "WNF_ALO_DUP";
    case AOT_HEAP_KIND_WNF_DUP_LAM:  return "WNF_DUP_LAM";
    case AOT_HEAP_KIND_WNF_DUP_SUP:  return "WNF_DUP_SUP";
    case AOT_HEAP_KIND_WNF_ALO_LAM:  return "WNF_ALO_LAM";
    case AOT_HEAP_KIND_WNF_UNS:      return "WNF_UNS";
    case AOT_HEAP_KIND_WNF_DUP_NOD:  return "WNF_DUP_NOD";
    case AOT_HEAP_KIND_WNF_APP_MAT:  return "WNF_APP_MAT_CTR";
    default:                         return "UNKNOWN";
  }
}

// Marks entry into compiled execution for allocator attribution.
fn void aot_heap_compiled_enter(void) {
  AOT_HEAP_COMPILED_DEPTH++;
}

// Marks exit from compiled execution for allocator attribution.
fn void aot_heap_compiled_leave(void) {
  if (AOT_HEAP_COMPILED_DEPTH > 0) {
    AOT_HEAP_COMPILED_DEPTH--;
  }
}

// Attributes one heap allocation performed while compiled code is active.
fn void aot_heap_alloc_note(u64 size) {
  if (!aot_heap_attr_enabled()) {
    return;
  }
  if (AOT_HEAP_COMPILED_DEPTH == 0) {
    return;
  }
  if (size == 0) {
    return;
  }

  u32 bucket = aot_heap_bucket(size);
  __atomic_fetch_add(&AOT_HEAP_CALLS, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&AOT_HEAP_NODES, size, __ATOMIC_RELAXED);
  __atomic_fetch_add(&AOT_HEAP_BUCKET_CALLS[bucket], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&AOT_HEAP_BUCKET_NODES[bucket], size, __ATOMIC_RELAXED);
}

// Attributes one heap allocation to one semantic term family when available.
fn void aot_heap_alloc_note_kind(u32 kind, u64 size) {
  if (!aot_heap_attr_enabled()) {
    return;
  }
  if (AOT_HEAP_COMPILED_DEPTH == 0) {
    return;
  }
  if (size == 0) {
    return;
  }
  if (kind >= AOT_HEAP_KIND_COUNT) {
    kind = AOT_HEAP_KIND_NONE;
  }

  __atomic_fetch_add(&AOT_HEAP_KIND_CALLS[kind], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&AOT_HEAP_KIND_NODES[kind], size, __ATOMIC_RELAXED);
}

// Prints compiled-context heap allocation counters when enabled.
fn void aot_heap_attr_dump(FILE *f) {
  if (!aot_heap_attr_enabled()) {
    return;
  }

  u64 calls = __atomic_load_n(&AOT_HEAP_CALLS, __ATOMIC_RELAXED);
  u64 nodes = __atomic_load_n(&AOT_HEAP_NODES, __ATOMIC_RELAXED);
  fprintf(f, "AOT_HEAP total_calls=%llu total_nodes=%llu\n",
    (unsigned long long)calls,
    (unsigned long long)nodes);

  for (u32 i = 0; i < AOT_HEAP_BUCKET_COUNT; i++) {
    u64 bcalls = __atomic_load_n(&AOT_HEAP_BUCKET_CALLS[i], __ATOMIC_RELAXED);
    u64 bnodes = __atomic_load_n(&AOT_HEAP_BUCKET_NODES[i], __ATOMIC_RELAXED);
    if (bcalls == 0 && bnodes == 0) {
      continue;
    }
    fprintf(f, "AOT_HEAP size[%s] calls=%llu nodes=%llu\n",
      aot_heap_bucket_name(i),
      (unsigned long long)bcalls,
      (unsigned long long)bnodes);
  }

  for (u32 i = 0; i < AOT_HEAP_KIND_COUNT; i++) {
    u64 bcalls = __atomic_load_n(&AOT_HEAP_KIND_CALLS[i], __ATOMIC_RELAXED);
    u64 bnodes = __atomic_load_n(&AOT_HEAP_KIND_NODES[i], __ATOMIC_RELAXED);
    if (bcalls == 0 && bnodes == 0) {
      continue;
    }
    fprintf(f, "AOT_HEAP kind[%u]=%s calls=%llu nodes=%llu\n",
      i,
      aot_heap_kind_name(i),
      (unsigned long long)bcalls,
      (unsigned long long)bnodes);
  }
}

// Returns 1 when MAT-DP probe telemetry is enabled via HVM_AOT_MAT_DP.
fn int aot_mat_dp_enabled(void) {
  int enabled = __atomic_load_n(&AOT_MAT_DP_ENABLED, __ATOMIC_RELAXED);
  if (enabled >= 0) {
    return enabled;
  }

  const char *env = getenv("HVM_AOT_MAT_DP");
  enabled = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
  __atomic_store_n(&AOT_MAT_DP_ENABLED, enabled, __ATOMIC_RELAXED);
  return enabled;
}

// Prints MAT-DP probe counters when enabled.
fn void aot_mat_dp_dump(FILE *f) {
  if (!aot_mat_dp_enabled()) {
    return;
  }

  u64 probes   = __atomic_load_n(&AOT_MAT_DP_PROBES, __ATOMIC_RELAXED);
  u64 hits     = __atomic_load_n(&AOT_MAT_DP_HITS, __ATOMIC_RELAXED);
  u64 non_dp   = __atomic_load_n(&AOT_MAT_DP_NON_DP, __ATOMIC_RELAXED);
  u64 sub      = __atomic_load_n(&AOT_MAT_DP_SUB, __ATOMIC_RELAXED);
  u64 non_ctr  = __atomic_load_n(&AOT_MAT_DP_NON_CTR, __ATOMIC_RELAXED);
  u64 ext_miss = __atomic_load_n(&AOT_MAT_DP_EXT_MISS, __ATOMIC_RELAXED);
  u64 force    = __atomic_load_n(&AOT_MAT_DP_FORCE, __ATOMIC_RELAXED);
  u64 force_ctr = __atomic_load_n(&AOT_MAT_DP_FORCE_TO_CTR, __ATOMIC_RELAXED);

  fprintf(f, "AOT_MAT_DP probes=%llu hits=%llu non_dp=%llu sub=%llu non_ctr=%llu ext_miss=%llu force=%llu force_to_ctr=%llu\n",
    (unsigned long long)probes,
    (unsigned long long)hits,
    (unsigned long long)non_dp,
    (unsigned long long)sub,
    (unsigned long long)non_ctr,
    (unsigned long long)ext_miss,
    (unsigned long long)force,
    (unsigned long long)force_ctr);
}

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

// Allocation Telemetry
// --------------------

typedef enum {
  AOT_ALLOC_APP_FRAME_HEAD = 0,
  AOT_ALLOC_APP_FRAME_MAT,
  AOT_ALLOC_DUP_CELL,
  AOT_ALLOC_CTR_FIELDS,
  AOT_ALLOC_ENV_BIND,
  AOT_ALLOC_CLASS_COUNT,
} AotAllocClass;

static int AOT_ALLOC_ENABLED = -1;
static u64 AOT_ALLOC_CALLS[AOT_ALLOC_CLASS_COUNT] = {0};
static u64 AOT_ALLOC_NODES[AOT_ALLOC_CLASS_COUNT] = {0};

// Returns one human-readable name for one allocation class.
fn const char *aot_alloc_class_name(u32 cls) {
  static const char *NAMES[AOT_ALLOC_CLASS_COUNT] = {
    "APP_FRAME_HEAD",
    "APP_FRAME_MAT",
    "DUP_CELL",
    "CTR_FIELDS",
    "ENV_BIND",
  };
  if (cls >= AOT_ALLOC_CLASS_COUNT) {
    return "UNKNOWN";
  }
  return NAMES[cls];
}

// Maps one AOT allocation class to one heap-kind telemetry label.
fn u32 aot_alloc_heap_kind(u32 cls) {
  switch (cls) {
    case AOT_ALLOC_APP_FRAME_HEAD: return AOT_HEAP_KIND_AOT_APP_HEAD;
    case AOT_ALLOC_APP_FRAME_MAT:  return AOT_HEAP_KIND_AOT_APP_MAT;
    case AOT_ALLOC_DUP_CELL:       return AOT_HEAP_KIND_AOT_DUP_CELL;
    case AOT_ALLOC_CTR_FIELDS:     return AOT_HEAP_KIND_AOT_CTR;
    case AOT_ALLOC_ENV_BIND:       return AOT_HEAP_KIND_AOT_ENV_BIND;
    default:                       return AOT_HEAP_KIND_NONE;
  }
}

// Returns 1 when allocation-class telemetry is enabled via HVM_AOT_ALLOC.
fn int aot_alloc_enabled(void) {
  int enabled = __atomic_load_n(&AOT_ALLOC_ENABLED, __ATOMIC_RELAXED);
  if (enabled >= 0) {
    return enabled;
  }

  const char *env = getenv("HVM_AOT_ALLOC");
  enabled = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
  __atomic_store_n(&AOT_ALLOC_ENABLED, enabled, __ATOMIC_RELAXED);
  return enabled;
}

// Allocates one heap chunk and attributes it to one AOT allocation class.
fn u64 aot_alloc(u32 size, u32 cls) {
  if (aot_alloc_enabled() && cls < AOT_ALLOC_CLASS_COUNT) {
    __atomic_fetch_add(&AOT_ALLOC_CALLS[cls], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&AOT_ALLOC_NODES[cls], size, __ATOMIC_RELAXED);
  }
  return heap_alloc_kind(size, aot_alloc_heap_kind(cls));
}

// Prints allocation-class counters when telemetry is enabled.
fn void aot_alloc_dump(FILE *f) {
  if (!aot_alloc_enabled()) {
    return;
  }

  u64 total_calls = 0;
  u64 total_nodes = 0;
  for (u32 cls = 0; cls < AOT_ALLOC_CLASS_COUNT; cls++) {
    total_calls += __atomic_load_n(&AOT_ALLOC_CALLS[cls], __ATOMIC_RELAXED);
    total_nodes += __atomic_load_n(&AOT_ALLOC_NODES[cls], __ATOMIC_RELAXED);
  }

  fprintf(f, "AOT_ALLOC total_calls=%llu total_nodes=%llu\n", (unsigned long long)total_calls, (unsigned long long)total_nodes);
  for (u32 cls = 0; cls < AOT_ALLOC_CLASS_COUNT; cls++) {
    u64 calls = __atomic_load_n(&AOT_ALLOC_CALLS[cls], __ATOMIC_RELAXED);
    u64 nodes = __atomic_load_n(&AOT_ALLOC_NODES[cls], __ATOMIC_RELAXED);
    if (calls == 0 && nodes == 0) {
      continue;
    }
    fprintf(f, "AOT_ALLOC class[%u]=%s calls=%llu nodes=%llu\n",
      cls,
      aot_alloc_class_name(cls),
      (unsigned long long)calls,
      (unsigned long long)nodes);
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
static u8  AOT_DEOPT_SITE_GOT_VALID[AOT_DEOPT_SITE_CAP] = {0};
static u8  AOT_DEOPT_SITE_GOT_TAG[AOT_DEOPT_SITE_CAP] = {0};
static u32 AOT_DEOPT_SITE_GOT_EXT[AOT_DEOPT_SITE_CAP] = {0};
static u64 AOT_DEOPT_MAT_GOT_TAG_COUNTS[TAG_MASK + 1] = {0};
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

// Records one runtime observed term for one strict deopt site.
fn void aot_deopt_site_observe_term(u32 site, Term got) {
  if (!aot_deopt_enabled()) {
    return;
  }
  if (site >= AOT_DEOPT_SITE_CAP) {
    return;
  }

  u8  tag = term_tag(got);
  u32 ext = term_ext(got);

  __atomic_store_n(&AOT_DEOPT_SITE_GOT_TAG[site], tag, __ATOMIC_RELAXED);
  __atomic_store_n(&AOT_DEOPT_SITE_GOT_EXT[site], ext, __ATOMIC_RELAXED);
  __atomic_store_n(&AOT_DEOPT_SITE_GOT_VALID[site], 1, __ATOMIC_RELAXED);

  u32 kind = __atomic_load_n(&AOT_DEOPT_SITE_KINDS[site], __ATOMIC_RELAXED);
  if (kind == AOT_DEOPT_SITE_KIND_HEAD_MAT_NON_CTR && tag <= TAG_MASK) {
    __atomic_fetch_add(&AOT_DEOPT_MAT_GOT_TAG_COUNTS[tag], 1, __ATOMIC_RELAXED);
  }
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

// Prints runtime observed-tag histogram for HEAD_MAT_NON_CTR sites.
fn void aot_deopt_dump_head_mat_tags(FILE *f, const char *prefix) {
  u64 total = 0;
  for (u32 tag = 0; tag <= TAG_MASK; tag++) {
    total += __atomic_load_n(&AOT_DEOPT_MAT_GOT_TAG_COUNTS[tag], __ATOMIC_RELAXED);
  }
  if (total == 0) {
    return;
  }

  fprintf(f, "%s head_mat_non_ctr_got_tags total=%llu\n", prefix, (unsigned long long)total);
  for (u32 tag = 0; tag <= TAG_MASK; tag++) {
    u64 hits = __atomic_load_n(&AOT_DEOPT_MAT_GOT_TAG_COUNTS[tag], __ATOMIC_RELAXED);
    if (hits == 0) {
      continue;
    }
    fprintf(f, "%s head_mat_non_ctr got_tag=%s(%u) hits=%llu\n",
      prefix,
      aot_term_tag_name(tag),
      tag,
      (unsigned long long)hits);
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
    u32 got_ok = __atomic_load_n(&AOT_DEOPT_SITE_GOT_VALID[site], __ATOMIC_RELAXED);
    u32 got_tag = __atomic_load_n(&AOT_DEOPT_SITE_GOT_TAG[site], __ATOMIC_RELAXED);
    u32 got_ext = __atomic_load_n(&AOT_DEOPT_SITE_GOT_EXT[site], __ATOMIC_RELAXED);

    fprintf(f, "%s site[%u] hits=%llu reason=%s", prefix, site, (unsigned long long)top_hits[i], aot_deopt_reason_name(reason));
    if (kind != AOT_DEOPT_SITE_KIND_NONE || loc != 0 || aux != 0) {
      fprintf(f, " kind=%s loc=%llu", aot_deopt_site_kind_name(kind), (unsigned long long)loc);
      if (kind == AOT_DEOPT_SITE_KIND_HEAD_SWI_NON_NUM) {
        fprintf(f, " expect_num=%u", aux);
      } else if (kind == AOT_DEOPT_SITE_KIND_HEAD_MAT_NON_CTR) {
        fprintf(f, " expect_ctr_ext=%u", aux);
        if (got_ok) {
          fprintf(f, " got_tag=%s(%u) got_ext=%u", aot_term_tag_name(got_tag), got_tag, got_ext);
        }
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
    aot_deopt_dump_head_mat_tags(f, "AOT_DEOPT");
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

// Ensures one lexical bind-list prefix exists in heap and returns its head.
fn u64 aot_env_head(Term *env_cells, u64 *env_locs, u32 len) {
  if (len == 0) {
    return 0ULL;
  }

  if (len > AOT_ENV_CAP) {
    len = AOT_ENV_CAP;
  }

  for (u32 i = 0; i < len; i++) {
    u64 prev = i == 0 ? 0ULL : env_locs[i - 1];
    u64 bind = env_locs[i];
    if (bind == 0ULL) {
      bind = aot_alloc(2, AOT_ALLOC_ENV_BIND);
      heap_set(bind + 0, env_cells[i]);
      heap_set(bind + 1, term_new(0, NUM, 0, prev));
      env_locs[i] = bind;
      continue;
    }

    Term link = heap_read(bind + 1);
    if (term_tag(link) != NUM || term_val(link) != prev) {
      heap_set_rel(bind + 1, term_new(0, NUM, 0, prev));
    }
  }

  return env_locs[len - 1];
}

// Ensures one lexical binder slot exists in heap and returns its location.
fn u64 aot_env_get(Term *env_cells, u64 *env_locs, u32 idx) {
  if (idx >= AOT_ENV_CAP) {
    return 0ULL;
  }
  u64 bind = env_locs[idx];
  if (bind != 0ULL) {
    return bind;
  }

  bind = aot_alloc(2, AOT_ALLOC_ENV_BIND);
  heap_set(bind + 0, env_cells[idx]);
  heap_set(bind + 1, term_new(0, NUM, 0, 0ULL));
  env_locs[idx] = bind;
  return bind;
}

// Pushes one raw argument as one runtime APP frame.
fn void aot_stack_push_arg(Term *stack, u32 *s_pos, Term arg, u32 cls) {
  u64 cell = aot_alloc(2, cls);
  heap_set(cell + 0, term_new_era());
  heap_set(cell + 1, arg);
  stack[*s_pos] = term_new(0, APP, 0, cell);
  (*s_pos)++;
}

// Reifies pending local compiled args back to runtime APP frames.
fn void aot_stack_reify_args(Term *stack, u32 *s_pos, const Term *a_args, u32 a_pos, u32 cls) {
  for (u32 i = 0; i < a_pos; i++) {
    aot_stack_push_arg(stack, s_pos, a_args[i], cls);
  }
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
  switch (tag) {
    case NUM:
    case C00:
    case ERA:
    case ANY:
    case NAM:
    case BJV:
    case BJ0:
    case BJ1: {
      return 1;
    }
    default: {
      return 0;
    }
  }
}

fn int aot_can_force_strict_tag(u8 tag);
fn Term aot_force_whnf_local(Term term, u32 stack_top);

// Tries one strict-MAT fast path for DP scrutinees over matching constructors.
// On success, returns 1 and outputs constructor metadata for lazy field projections.
fn int aot_try_mat_dp_ctr_match(Term term, u32 ctr_ext, u32 stack_top, u8 *side_out, u32 *lab_out, u32 *ari_out, u64 *ctr_loc_out) {
  int trace = aot_mat_dp_enabled();
  if (trace) {
    __atomic_fetch_add(&AOT_MAT_DP_PROBES, 1, __ATOMIC_RELAXED);
  }

  u8 tag = term_tag(term);
  if (tag != DP0 && tag != DP1) {
    if (trace) {
      __atomic_fetch_add(&AOT_MAT_DP_NON_DP, 1, __ATOMIC_RELAXED);
    }
    return 0;
  }

  u8  side = tag == DP0 ? 0 : 1;
  u64 loc  = term_val(term);
  u32 lab  = term_ext(term);
  Term cell = heap_take(loc);

  if (term_sub_get(cell)) {
    if (trace) {
      __atomic_fetch_add(&AOT_MAT_DP_SUB, 1, __ATOMIC_RELAXED);
    }
    heap_set_rel(loc, cell);
    return 0;
  }

  u8 cell_tag = term_tag(cell);
  if (cell_tag < C00 || cell_tag > C16) {
    if (!aot_can_force_strict_tag(cell_tag)) {
      if (trace) {
        __atomic_fetch_add(&AOT_MAT_DP_NON_CTR, 1, __ATOMIC_RELAXED);
      }
      heap_set_rel(loc, cell);
      return 0;
    }

    if (trace) {
      __atomic_fetch_add(&AOT_MAT_DP_FORCE, 1, __ATOMIC_RELAXED);
    }
    heap_set_rel(loc, cell);
    Term forced = aot_force_whnf_local(cell, stack_top);
    if (forced != cell) {
      heap_set_rel(loc, forced);
    }

    cell = forced;
    cell_tag = term_tag(cell);
    if (cell_tag < C00 || cell_tag > C16) {
      if (trace) {
        __atomic_fetch_add(&AOT_MAT_DP_NON_CTR, 1, __ATOMIC_RELAXED);
      }
      return 0;
    }
    if (trace) {
      __atomic_fetch_add(&AOT_MAT_DP_FORCE_TO_CTR, 1, __ATOMIC_RELAXED);
    }
  }
  if (term_ext(cell) != ctr_ext) {
    if (trace) {
      __atomic_fetch_add(&AOT_MAT_DP_EXT_MISS, 1, __ATOMIC_RELAXED);
    }
    heap_set_rel(loc, cell);
    return 0;
  }

  if (side_out != NULL) {
    *side_out = side;
  }
  if (lab_out != NULL) {
    *lab_out = lab;
  }
  if (ari_out != NULL) {
    *ari_out = (u32)(cell_tag - C00);
  }
  if (ctr_loc_out != NULL) {
    *ctr_loc_out = term_val(cell);
  }

  if (trace) {
    __atomic_fetch_add(&AOT_MAT_DP_HITS, 1, __ATOMIC_RELAXED);
  }
  heap_set_rel(loc, cell);
  return 1;
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
  if (stack_top == 0) {
    stack_top = 1;
  }
  WNF_S_POS = stack_top;
  Term out = wnf(term);
  WNF_S_POS = saved_sp;
  return out;
}

// Tries bounded conservative DP forcing for a strict AOT scrutinee.
// Unknown payloads are locally WHNF-forced once and then retried by caller.
fn Term aot_force_dup(Term term, u32 stack_top, int *progress) {
  if (progress != NULL) {
    *progress = 0;
  }

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
      if (progress != NULL) {
        *progress = 1;
      }
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
        // Unknown/non-WNF payloads are forced once, then retried by caller fuel.
        heap_set_rel(loc, cell);
        Term forced = aot_force_whnf_local(cell, stack_top);
        if (forced != cell) {
          heap_set_rel(loc, forced);
          if (progress != NULL) {
            *progress = 1;
          }
        }
        return term;
      }
    }

    term = next;
    if (progress != NULL) {
      *progress = 1;
    }
  }
  return term;
}

// Tries conservative strict forcing for compiled SWI/MAT scrutinees.
fn Term aot_force_strict(Term term, u32 stack_top) {
  for (u32 fuel = AOT_FORCE_STRICT_FUEL; fuel > 0; --fuel) {
    u8 tag = term_tag(term);
    if (tag == DP0 || tag == DP1) {
      int progress = 0;
      Term next = aot_force_dup(term, stack_top, &progress);
      if (!progress && next == term) {
        return term;
      }
      term = next;
      continue;
    }
    if (aot_can_force_strict_tag(tag)) {
      term = aot_force_whnf_local(term, stack_top);
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

// Calls one compiled ref using current stack slice and pending local args.
fn Term aot_call_ref(u32 ref_id, Term *stack, u32 *s_pos, u32 base, Term *a_args, u32 *a_pos) {
  if (AOT_CALL_DEPTH >= AOT_MAX_DEPTH) {
    return term_new_ref(ref_id);
  }

  HvmAotFn fun = AOT_FNS[ref_id];
  if (fun == NULL) {
    return term_new_ref(ref_id);
  }

  AOT_CALL_DEPTH++;
  aot_heap_compiled_enter();
  Term out = fun(stack, s_pos, base, a_args, a_pos);
  aot_heap_compiled_leave();
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

  Term a_args[AOT_ARG_CAP] = {0};
  u32  a_pos = 0;
  AOT_CALL_DEPTH++;
  aot_heap_compiled_enter();
  *out = fun(stack, s_pos, base, a_args, &a_pos);
  aot_heap_compiled_leave();
  AOT_CALL_DEPTH--;
  if (a_pos > 0) {
    aot_stack_reify_args(stack, s_pos, a_args, a_pos, AOT_ALLOC_APP_FRAME_HEAD);
  }
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
