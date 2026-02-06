// ============================================================================
// HVM4 Metal Test Harness
// ============================================================================
//
// Builds small term graphs in the heap by hand, runs metal_normalize,
// and checks the results. Tests the base interactions:
//   APP-LAM, APP-SUP, DUP-LAM, DUP-SUP, DUP-NOD(era/num)
// Plus church encoding tests from the C test suite (_book_.hvm4):
//   cnot, cadd, cmul on c1/c2/c4/c8
// ============================================================================

#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include "host.h"

// --- Term encoding (must match hvm4.metal) ---
typedef uint64_t Term;

#define TAG_APP 0
#define TAG_VAR 1
#define TAG_LAM 2
#define TAG_DP0 3
#define TAG_DP1 4
#define TAG_SUP 5
#define TAG_DUP 6
#define TAG_ERA 11
#define TAG_NUM 30

#define LAM_ERA_MASK 0x800000

// Labels for church encoding dups (matching _book_.hvm4 conventions)
#define LAB_C 1  // church numeral internal dups (&C)
#define LAB_B 2  // cadd dup (&B)

static inline Term mk(uint8_t sub, uint8_t tag, uint32_t ext, uint32_t val) {
  return ((uint64_t)(sub & 1) << 63)
       | ((uint64_t)(tag & 0x7F) << 56)
       | ((uint64_t)(ext & 0xFFFFFF) << 32)
       | ((uint64_t)(val & 0xFFFFFFFF));
}

static inline uint8_t  t_tag(Term t) { return (uint8_t)((t >> 56) & 0x7F); }
static inline uint32_t t_ext(Term t) { return (uint32_t)((t >> 32) & 0xFFFFFF); }
static inline uint32_t t_val(Term t) { return (uint32_t)(t & 0xFFFFFFFF); }

// Build terms directly in Metal shared buffer (UMA, no memcpy needed).
static uint64_t *HEAP = NULL;  // set to metal_heap_ptr() after init
static uint32_t heap_top = 1;  // skip word 0 (null sentinel)

static uint32_t alloc(uint32_t n) {
  uint32_t at = heap_top;
  heap_top += n;
  return at;
}

static void heap_set(uint32_t loc, Term val) { HEAP[loc] = val; }

#define RESET() do { memset(HEAP, 0, (size_t)heap_top * sizeof(uint64_t)); heap_top = 1; } while(0)

// --- Term constructors ---
static Term new_num(uint32_t n)    { return mk(0, TAG_NUM, 0, n); }
static Term new_era(void)          { return mk(0, TAG_ERA, 0, 0); }
static Term new_var(uint32_t loc)  { return mk(0, TAG_VAR, 0, loc); }

static Term new_lam(uint32_t ext, Term body) {
  uint32_t loc = alloc(1);
  heap_set(loc, body);
  return mk(0, TAG_LAM, ext, loc);
}

static Term new_app(Term fun, Term arg) {
  uint32_t loc = alloc(2);
  heap_set(loc, fun);
  heap_set(loc + 1, arg);
  return mk(0, TAG_APP, 0, loc);
}

static Term new_sup(uint32_t lab, Term a, Term b) {
  uint32_t loc = alloc(2);
  heap_set(loc, a);
  heap_set(loc + 1, b);
  return mk(0, TAG_SUP, lab, loc);
}

static Term new_dp0(uint32_t lab, uint32_t loc) { return mk(0, TAG_DP0, lab, loc); }
static Term new_dp1(uint32_t lab, uint32_t loc) { return mk(0, TAG_DP1, lab, loc); }

// Build λx.x (identity)
static Term new_id(void) {
  uint32_t loc = alloc(1);
  heap_set(loc, new_var(loc));
  return mk(0, TAG_LAM, 0, loc);
}

// --- Helpers ---
static const char* tag_name(uint8_t tag) {
  switch (tag) {
    case TAG_APP: return "APP";
    case TAG_VAR: return "VAR";
    case TAG_LAM: return "LAM";
    case TAG_DP0: return "DP0";
    case TAG_DP1: return "DP1";
    case TAG_SUP: return "SUP";
    case TAG_DUP: return "DUP";
    case TAG_ERA: return "ERA";
    case TAG_NUM: return "NUM";
    default: return "???";
  }
}

static void print_term_shallow(Term t) {
  printf("%s(ext=%u, val=%u)", tag_name(t_tag(t)), t_ext(t), t_val(t));
}

// --- Deep term printer (reads from Metal heap after normalize) ---
// Tracks seen dup locations to print bindings at first occurrence.
#define MAX_SEEN_DUPS 1024
static uint32_t seen_dups[MAX_SEEN_DUPS];
static int       seen_dup_count = 0;

static int dup_was_seen(uint32_t loc) {
  for (int i = 0; i < seen_dup_count; i++)
    if (seen_dups[i] == loc) return 1;
  return 0;
}

static void dup_mark_seen(uint32_t loc) {
  if (seen_dup_count < MAX_SEEN_DUPS) seen_dups[seen_dup_count++] = loc;
}

static void print_metal_term_rec(Term t, int depth) {
  if (depth > 60) { printf("..."); return; }
  uint8_t tag  = t_tag(t);
  uint32_t ext = t_ext(t);
  uint32_t val = t_val(t);
  uint8_t sub  = (uint8_t)((t >> 63) & 1);

  switch (tag) {
    case TAG_LAM:
      if (ext & LAM_ERA_MASK)
        printf("λ_.");
      else
        printf("λx%u.", val);
      print_metal_term_rec(metal_heap_read(val), depth + 1);
      break;
    case TAG_APP:
      printf("(");
      print_metal_term_rec(metal_heap_read(val), depth + 1);
      printf(" ");
      print_metal_term_rec(metal_heap_read(val + 1), depth + 1);
      printf(")");
      break;
    case TAG_VAR: {
      Term v = metal_heap_read(val);
      if ((v >> 63) & 1) {
        // substituted - follow
        Term cleared = v & ~((uint64_t)1 << 63);
        print_metal_term_rec(cleared, depth + 1);
      } else {
        printf("x%u", val);
      }
      break;
    }
    case TAG_DP0:
    case TAG_DP1: {
      Term dval = metal_heap_read(val);
      if ((dval >> 63) & 1) {
        // substituted - follow
        Term cleared = dval & ~((uint64_t)1 << 63);
        print_metal_term_rec(cleared, depth + 1);
      } else {
        // unresolved dup - print binding at first occurrence
        if (!dup_was_seen(val)) {
          dup_mark_seen(val);
          printf("[!&%u@%u=", ext, val);
          print_metal_term_rec(dval, depth + 1);
          printf("]");
        }
        printf("%s&%u@%u", tag == TAG_DP0 ? "a" : "b", ext, val);
      }
      break;
    }
    case TAG_SUP: {
      printf("&%u{", ext);
      print_metal_term_rec(metal_heap_read(val), depth + 1);
      printf(",");
      print_metal_term_rec(metal_heap_read(val + 1), depth + 1);
      printf("}");
      break;
    }
    case TAG_ERA: printf("*"); break;
    case TAG_NUM: printf("#%u", val); break;
    default: printf("?%u(%u,%u)", tag, ext, val); break;
  }
}

static void print_metal_term(Term t) {
  seen_dup_count = 0;
  print_metal_term_rec(t, 0);
  printf("\n");
}

// --- Run helpers ---
static int run_check_num(uint32_t root, uint32_t expected) {
  metal_set_alloc_cursor(heap_top);
  metal_normalize(root);
  Term result = metal_heap_read(root);
  if (t_tag(result) == TAG_NUM && t_val(result) == expected) {
    printf("PASS\n");
    return 0;
  }
  printf("FAIL (got ");
  print_term_shallow(result);
  printf(")\n");
  return 1;
}

static int run_check_tag(uint32_t root, uint8_t expected_tag) {
  metal_set_alloc_cursor(heap_top);
  metal_normalize(root);
  Term result = metal_heap_read(root);
  if (t_tag(result) == expected_tag) {
    printf("PASS\n");
    return 0;
  }
  printf("FAIL (got ");
  print_term_shallow(result);
  printf(")\n");
  return 1;
}

// ============================================================================
// Church Encoding Builders
// ============================================================================
// These mirror the definitions in test/_book_.hvm4:
//   @ctru = λt. λf. t
//   @cfal = λt. λf. f
//   @cnot = λb. λt. λf. b(f, t)
//   @c1   = λs. λx. s(x)
//   @c2   = λs. !S0&C=s; λx0. S0₀(S0₁(x0))
//   @c4   = λs. !S0&C=s; !S1&C=λx0.S0₀(S0₁(x0)); λx1.S1₀(S1₁(x1))
//   @c8   = λs. !S0&C=s; !S1&C=...; !S2&C=...; λx3.S2₀(S2₁(x3))
//   @cadd = λa. λb. λs. λz. !S&B=s; a(S₀, b(S₁, z))
//   @cmul = λa. λb. λs. λz. a(b(s), z)

// ctru = λt.λ_.t  (church true: select first)
static Term build_ctru(void) {
  uint32_t t_loc = alloc(1);
  Term inner = new_lam(LAM_ERA_MASK, new_var(t_loc));
  heap_set(t_loc, inner);
  return mk(0, TAG_LAM, 0, t_loc);
}

// cnot = λb.λt.λf. b(f, t)
static Term build_cnot(void) {
  uint32_t b_loc = alloc(1);
  uint32_t t_loc = alloc(1);
  uint32_t f_loc = alloc(1);
  Term body = new_app(new_app(new_var(b_loc), new_var(f_loc)), new_var(t_loc));
  heap_set(f_loc, body);
  Term lam_f = mk(0, TAG_LAM, 0, f_loc);
  heap_set(t_loc, lam_f);
  Term lam_t = mk(0, TAG_LAM, 0, t_loc);
  heap_set(b_loc, lam_t);
  return mk(0, TAG_LAM, 0, b_loc);
}

// c1 = λs.λx. s(x)
static Term build_c1(void) {
  uint32_t s_loc = alloc(1);
  uint32_t x_loc = alloc(1);
  Term body = new_app(new_var(s_loc), new_var(x_loc));
  heap_set(x_loc, body);
  Term lam_x = mk(0, TAG_LAM, 0, x_loc);
  heap_set(s_loc, lam_x);
  return mk(0, TAG_LAM, 0, s_loc);
}

// c2 = λs. λx. S0₀(S0₁(x))  where S0 dups s
static Term build_c2(void) {
  uint32_t s_loc  = alloc(1);
  uint32_t dp_loc = alloc(1);
  uint32_t x_loc  = alloc(1);
  Term body = new_app(new_dp0(LAB_C, dp_loc),
                      new_app(new_dp1(LAB_C, dp_loc), new_var(x_loc)));
  heap_set(x_loc, body);
  Term lam_x = mk(0, TAG_LAM, 0, x_loc);
  heap_set(dp_loc, new_var(s_loc));
  heap_set(s_loc, lam_x);
  return mk(0, TAG_LAM, 0, s_loc);
}

// c4 = λs. λx1. S1₀(S1₁(x1))
//   where S1 dups λx0.S0₀(S0₁(x0)), and S0 dups s
static Term build_c4(void) {
  uint32_t s_loc   = alloc(1);
  uint32_t dp0_loc = alloc(1);
  uint32_t x0_loc  = alloc(1);
  uint32_t dp1_loc = alloc(1);
  uint32_t x1_loc  = alloc(1);

  // Final: S1₀(S1₁(x1))
  Term final_body = new_app(new_dp0(LAB_C, dp1_loc),
                            new_app(new_dp1(LAB_C, dp1_loc), new_var(x1_loc)));
  heap_set(x1_loc, final_body);
  Term lam_x1 = mk(0, TAG_LAM, 0, x1_loc);

  // Mid: λx0. S0₀(S0₁(x0))
  Term mid_body = new_app(new_dp0(LAB_C, dp0_loc),
                          new_app(new_dp1(LAB_C, dp0_loc), new_var(x0_loc)));
  heap_set(x0_loc, mid_body);
  Term lam_x0 = mk(0, TAG_LAM, 0, x0_loc);

  heap_set(dp1_loc, lam_x0);
  heap_set(dp0_loc, new_var(s_loc));
  heap_set(s_loc, lam_x1);
  return mk(0, TAG_LAM, 0, s_loc);
}

// c8 = λs. λx3. S2₀(S2₁(x3))
//   where S2 dups λx1.S1₀(S1₁(x1)),
//         S1 dups λx0.S0₀(S0₁(x0)),
//         S0 dups s
static Term build_c8(void) {
  uint32_t s_loc   = alloc(1);
  uint32_t dp0_loc = alloc(1);
  uint32_t x0_loc  = alloc(1);
  uint32_t dp1_loc = alloc(1);
  uint32_t x1_loc  = alloc(1);
  uint32_t dp2_loc = alloc(1);
  uint32_t x3_loc  = alloc(1);

  // Level 3: S2₀(S2₁(x3))
  Term body3 = new_app(new_dp0(LAB_C, dp2_loc),
                       new_app(new_dp1(LAB_C, dp2_loc), new_var(x3_loc)));
  heap_set(x3_loc, body3);
  Term lam_x3 = mk(0, TAG_LAM, 0, x3_loc);

  // Level 2: λx1. S1₀(S1₁(x1))
  Term body2 = new_app(new_dp0(LAB_C, dp1_loc),
                       new_app(new_dp1(LAB_C, dp1_loc), new_var(x1_loc)));
  heap_set(x1_loc, body2);
  Term lam_x1 = mk(0, TAG_LAM, 0, x1_loc);

  // Level 1: λx0. S0₀(S0₁(x0))
  Term body1 = new_app(new_dp0(LAB_C, dp0_loc),
                       new_app(new_dp1(LAB_C, dp0_loc), new_var(x0_loc)));
  heap_set(x0_loc, body1);
  Term lam_x0 = mk(0, TAG_LAM, 0, x0_loc);

  heap_set(dp2_loc, lam_x1);
  heap_set(dp1_loc, lam_x0);
  heap_set(dp0_loc, new_var(s_loc));
  heap_set(s_loc, lam_x3);
  return mk(0, TAG_LAM, 0, s_loc);
}

// cadd = λa.λb.λs.λz. !S&B=s; a(S₀, b(S₁, z))
static Term build_cadd(void) {
  uint32_t a_loc  = alloc(1);
  uint32_t b_loc  = alloc(1);
  uint32_t s_loc  = alloc(1);
  uint32_t z_loc  = alloc(1);
  uint32_t dp_loc = alloc(1);

  // body: a(S₀, b(S₁, z))
  Term b_app = new_app(new_app(new_var(b_loc), new_dp1(LAB_B, dp_loc)), new_var(z_loc));
  Term body  = new_app(new_app(new_var(a_loc), new_dp0(LAB_B, dp_loc)), b_app);

  heap_set(dp_loc, new_var(s_loc));
  heap_set(z_loc, body);
  Term lam_z = mk(0, TAG_LAM, 0, z_loc);
  heap_set(s_loc, lam_z);
  Term lam_s = mk(0, TAG_LAM, 0, s_loc);
  heap_set(b_loc, lam_s);
  Term lam_b = mk(0, TAG_LAM, 0, b_loc);
  heap_set(a_loc, lam_b);
  return mk(0, TAG_LAM, 0, a_loc);
}

// cmul = λa.λb.λs.λz. a(b(s), z)
static Term build_cmul(void) {
  uint32_t a_loc = alloc(1);
  uint32_t b_loc = alloc(1);
  uint32_t s_loc = alloc(1);
  uint32_t z_loc = alloc(1);

  Term bs   = new_app(new_var(b_loc), new_var(s_loc));
  Term body = new_app(new_app(new_var(a_loc), bs), new_var(z_loc));

  heap_set(z_loc, body);
  Term lam_z = mk(0, TAG_LAM, 0, z_loc);
  heap_set(s_loc, lam_z);
  Term lam_s = mk(0, TAG_LAM, 0, s_loc);
  heap_set(b_loc, lam_s);
  Term lam_b = mk(0, TAG_LAM, 0, b_loc);
  heap_set(a_loc, lam_b);
  return mk(0, TAG_LAM, 0, a_loc);
}

// ============================================================================
// Test cases - basic interactions
// ============================================================================

static int test_app_lam(void) {
  printf("test APP-LAM: (λx.x 42) → 42 ... ");
  RESET();
  uint32_t lam_loc = alloc(1);
  heap_set(lam_loc, new_var(lam_loc));
  Term lam = mk(0, TAG_LAM, 0, lam_loc);
  Term app = new_app(lam, new_num(42));
  uint32_t root = alloc(1);
  heap_set(root, app);
  return run_check_num(root, 42);
}

static int test_app_lam_era(void) {
  printf("test APP-LAM-ERA: (λ_.7 99) → 7 ... ");
  RESET();
  Term app = new_app(new_lam(LAM_ERA_MASK, new_num(7)), new_num(99));
  uint32_t root = alloc(1);
  heap_set(root, app);
  return run_check_num(root, 7);
}

static int test_app_era(void) {
  printf("test APP-ERA: (ERA 42) → ERA ... ");
  RESET();
  Term app = new_app(new_era(), new_num(42));
  uint32_t root = alloc(1);
  heap_set(root, app);
  return run_check_tag(root, TAG_ERA);
}

static int test_nested_app_lam(void) {
  printf("test NESTED: ((λa.a) ((λb.b) 5)) → 5 ... ");
  RESET();
  Term inner = new_app(new_id(), new_num(5));
  Term outer = new_app(new_id(), inner);
  uint32_t root = alloc(1);
  heap_set(root, outer);
  return run_check_num(root, 5);
}

static int test_dup_num(void) {
  printf("test DUP-NUM: !x = 42; x → 42 ... ");
  RESET();
  uint32_t dp_loc = alloc(1);
  heap_set(dp_loc, new_num(42));
  uint32_t root = alloc(1);
  heap_set(root, new_dp0(0, dp_loc));
  return run_check_num(root, 42);
}

static int test_dup_sup_match(void) {
  printf("test DUP-SUP (match): !x &0 = &0{1,2}; x₀ → 1 ... ");
  RESET();
  Term sup = new_sup(0, new_num(1), new_num(2));
  uint32_t dp_loc = alloc(1);
  heap_set(dp_loc, sup);
  uint32_t root = alloc(1);
  heap_set(root, new_dp0(0, dp_loc));
  return run_check_num(root, 1);
}

static int test_dup_sup_match_side1(void) {
  printf("test DUP-SUP (match, side1): !x &0 = &0{1,2}; x₁ → 2 ... ");
  RESET();
  Term sup = new_sup(0, new_num(1), new_num(2));
  uint32_t dp_loc = alloc(1);
  heap_set(dp_loc, sup);
  uint32_t root = alloc(1);
  heap_set(root, new_dp1(0, dp_loc));
  return run_check_num(root, 2);
}

static int test_dup_sup_mismatch(void) {
  printf("test DUP-SUP (mismatch): !x &0 = &1{10,20}; x₀ → &1{10,20} ... ");
  RESET();
  Term sup = new_sup(1, new_num(10), new_num(20));
  uint32_t dp_loc = alloc(1);
  heap_set(dp_loc, sup);
  uint32_t root = alloc(1);
  heap_set(root, new_dp0(0, dp_loc));

  metal_set_alloc_cursor(heap_top);
  metal_normalize(root);
  Term result = metal_heap_read(root);
  if (t_tag(result) != TAG_SUP || t_ext(result) != 1) {
    printf("FAIL (got ");
    print_term_shallow(result);
    printf(")\n");
    return 1;
  }
  uint32_t r_loc = t_val(result);
  Term c0 = metal_heap_read(r_loc);
  Term c1 = metal_heap_read(r_loc + 1);
  if (t_tag(c0) == TAG_NUM && t_val(c0) == 10 &&
      t_tag(c1) == TAG_NUM && t_val(c1) == 20) {
    printf("PASS\n");
    return 0;
  }
  printf("FAIL (children: ");
  print_term_shallow(c0);
  printf(", ");
  print_term_shallow(c1);
  printf(")\n");
  return 1;
}

static int test_app_sup(void) {
  printf("test APP-SUP: (&0{λx.x, λy.y} 42) → &0{42, 42} ... ");
  RESET();
  Term sup = new_sup(0, new_id(), new_id());
  Term app = new_app(sup, new_num(42));
  uint32_t root = alloc(1);
  heap_set(root, app);

  metal_set_alloc_cursor(heap_top);
  metal_normalize(root);
  Term result = metal_heap_read(root);
  if (t_tag(result) != TAG_SUP) {
    printf("FAIL (expected SUP, got ");
    print_term_shallow(result);
    printf(")\n");
    return 1;
  }
  uint32_t r_loc = t_val(result);
  Term c0 = metal_heap_read(r_loc);
  Term c1 = metal_heap_read(r_loc + 1);
  if (t_tag(c0) == TAG_NUM && t_val(c0) == 42 &&
      t_tag(c1) == TAG_NUM && t_val(c1) == 42) {
    printf("PASS\n");
    return 0;
  }
  printf("FAIL (children: ");
  print_term_shallow(c0);
  printf(", ");
  print_term_shallow(c1);
  printf(")\n");
  return 1;
}

static int test_dup_lam(void) {
  printf("test DUP-LAM: !f &0 = λx.x; (f₀ 10) → 10 ... ");
  RESET();
  uint32_t dp_loc = alloc(1);
  heap_set(dp_loc, new_id());
  Term app = new_app(new_dp0(0, dp_loc), new_num(10));
  uint32_t root = alloc(1);
  heap_set(root, app);
  return run_check_num(root, 10);
}

static int test_church_2(void) {
  // ((λf.λx.(f (f x)) id) 0) → 0
  printf("test CHURCH-2: ((λf.λx.(f (f x)) id) 0) → 0 ... ");
  RESET();
  uint32_t f_loc    = alloc(1);
  uint32_t dp_f_loc = alloc(1);
  uint32_t x_loc    = alloc(1);
  Term f1_x  = new_app(new_dp1(0, dp_f_loc), new_var(x_loc));
  Term f0_f1x = new_app(new_dp0(0, dp_f_loc), f1_x);
  heap_set(x_loc, f0_f1x);
  Term lam_x = mk(0, TAG_LAM, 0, x_loc);
  heap_set(dp_f_loc, new_var(f_loc));
  heap_set(f_loc, lam_x);
  Term lam_f = mk(0, TAG_LAM, 0, f_loc);
  Term expr = new_app(new_app(lam_f, new_id()), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 0);
}

// ============================================================================
// Test cases - additional base interactions
// ============================================================================

static int test_dup_era(void) {
  // !x &0 = ERA; x₀ → ERA
  printf("test DUP-ERA: !x &0 = ERA; x₀ → ERA ... ");
  RESET();
  uint32_t dp_loc = alloc(1);
  heap_set(dp_loc, new_era());
  uint32_t root = alloc(1);
  heap_set(root, new_dp0(0, dp_loc));
  return run_check_tag(root, TAG_ERA);
}

static int test_deep_beta(void) {
  // (λa.a)((λb.b)((λc.c) 7)) → 7
  printf("test DEEP-BETA: id(id(id(7))) → 7 ... ");
  RESET();
  Term expr = new_app(new_id(), new_app(new_id(), new_app(new_id(), new_num(7))));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 7);
}

static int test_dup_lam_both(void) {
  // !f &0 = λx.x; (f₀ (f₁ 5)) → 5
  printf("test DUP-LAM-BOTH: !f = id; (f₀ (f₁ 5)) → 5 ... ");
  RESET();
  uint32_t dp_loc = alloc(1);
  heap_set(dp_loc, new_id());
  Term expr = new_app(new_dp0(0, dp_loc),
                      new_app(new_dp1(0, dp_loc), new_num(5)));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 5);
}

static int test_double_dup(void) {
  // !f &0 = λx.x; !g &1 = f₀; (g₀ (g₁ (f₁ 5))) → 5
  printf("test DOUBLE-DUP: dup(dup(id)) applied → 5 ... ");
  RESET();
  uint32_t dp_f = alloc(1);
  heap_set(dp_f, new_id());
  uint32_t dp_g = alloc(1);
  heap_set(dp_g, new_dp0(0, dp_f));
  Term expr = new_app(new_dp0(1, dp_g),
              new_app(new_dp1(1, dp_g),
              new_app(new_dp1(0, dp_f), new_num(5))));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 5);
}

// ============================================================================
// Test cases - church encoding (from test/_book_.hvm4)
// ============================================================================
// All church tests apply the result to ctru/cnot then to NUM(1)/NUM(0)
// to reduce the final church boolean to a concrete number:
//   ctru(1,0) = 1,  cfal(1,0) = 0
//   cnot^n(ctru) = ctru if n even, cfal if n odd

static int test_cnot_true(void) {
  // cnot(ctru, 1, 0) → 0  (cnot flips true to false)
  printf("test CNOT-TRUE: cnot(ctru, 1, 0) → 0 ... ");
  RESET();
  Term expr = new_app(new_app(new_app(build_cnot(), build_ctru()),
                               new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 0);
}

static int test_cnot_cnot(void) {
  // cnot(cnot(ctru), 1, 0) → 1  (double flip = identity)
  printf("test CNOT-CNOT: cnot(cnot(ctru), 1, 0) → 1 ... ");
  RESET();
  Term inner = new_app(build_cnot(), build_ctru());
  Term expr  = new_app(new_app(new_app(build_cnot(), inner),
                                new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

static int test_c2_not_t(void) {
  // c2(cnot, ctru, 1, 0) → 1  (cnot^2 = even → ctru → 1)
  printf("test C2-NOT-T: c2(cnot, ctru, 1, 0) → 1 ... ");
  RESET();
  Term expr = new_app(new_app(
              new_app(new_app(build_c2(), build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

static int test_c4_not_t(void) {
  // c4(cnot, ctru, 1, 0) → 1  (cnot^4 = even → ctru → 1)
  printf("test C4-NOT-T: c4(cnot, ctru, 1, 0) → 1 ... ");
  RESET();
  Term expr = new_app(new_app(
              new_app(new_app(build_c4(), build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

static int test_c8_not_t(void) {
  // c8(cnot, ctru, 1, 0) → 1  (cnot^8 = even → ctru → 1)
  printf("test C8-NOT-T: c8(cnot, ctru, 1, 0) → 1 ... ");
  RESET();
  Term expr = new_app(new_app(
              new_app(new_app(build_c8(), build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

static int test_cadd_c1_c4(void) {
  // cadd(c1, c4, cnot, ctru, 1, 0) → 0  (c5 odd → cfal → 0)
  printf("test CADD(c1,c4): c5(cnot, ctru, 1, 0) → 0 ... ");
  RESET();
  Term cn = new_app(new_app(build_cadd(), build_c1()), build_c4());
  Term expr = new_app(new_app(
              new_app(new_app(cn, build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 0);
}

static int test_cadd_c2_c1(void) {
  // cadd(c2, c1, cnot, ctru, 1, 0) → 0  (c3 odd → cfal → 0)
  printf("test CADD(c2,c1): c3(cnot, ctru, 1, 0) → 0 ... ");
  RESET();
  Term cn = new_app(new_app(build_cadd(), build_c2()), build_c1());
  Term expr = new_app(new_app(
              new_app(new_app(cn, build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 0);
}

static int test_cadd_c2_c4(void) {
  // cadd(c2, c4, cnot, ctru, 1, 0) → 1  (c6 even → ctru → 1)
  printf("test CADD(c2,c4): c6(cnot, ctru, 1, 0) → 1 ... ");
  RESET();
  Term cn = new_app(new_app(build_cadd(), build_c2()), build_c4());
  Term expr = new_app(new_app(
              new_app(new_app(cn, build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

static int test_cadd_c4_c4(void) {
  // cadd(c4, c4, cnot, ctru, 1, 0) → 1  (c8 even → ctru → 1)
  printf("test CADD(c4,c4): c8(cnot, ctru, 1, 0) → 1 ... ");
  RESET();
  Term cn = new_app(new_app(build_cadd(), build_c4()), build_c4());
  Term expr = new_app(new_app(
              new_app(new_app(cn, build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

static int test_cmul_c4_c2(void) {
  // cmul(c4, c2, cnot, ctru, 1, 0) → 1  (c8 even → ctru → 1)
  printf("test CMUL(c4,c2): c8(cnot, ctru, 1, 0) → 1 ... ");
  RESET();
  Term cn = new_app(new_app(build_cmul(), build_c4()), build_c2());
  Term expr = new_app(new_app(
              new_app(new_app(cn, build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

static int test_cmul_c4_c4(void) {
  // cmul(c4, c4, cnot, ctru, 1, 0) → 1  (c16 even → ctru → 1)
  printf("test CMUL(c4,c4): c16(cnot, ctru, 1, 0) → 1 ... ");
  RESET();
  Term cn = new_app(new_app(build_cmul(), build_c4()), build_c4());
  Term expr = new_app(new_app(
              new_app(new_app(cn, build_cnot()), build_ctru()),
              new_num(1)), new_num(0));
  uint32_t root = alloc(1);
  heap_set(root, expr);
  return run_check_num(root, 1);
}

// ============================================================================
// Benchmark: P_n(cnot)(ctru)(1)(0) -- applies cnot 2^n times
// ============================================================================
// P_n repeatedly doubles a function via dup+compose:
//   P_n(f) = f^(2^n)
// Applied to cnot and ctru, it performs 2^n church boolean negations.

#define LAB_P    0       // label for P_n dup chain
#define LAB_TREE 0xFFFF  // label for parallel benchmark SUP tree

// cnot for benchmarking: λx. ((x cfal) ctru) with cfal/ctru inlined in body.
// This matches the C version's @cnot = λx.x(@cfal,@ctru) which embeds
// the boolean constants, causing them to be duplicated along with cnot.
static Term build_cnot_bench(void) {
  // cfal = λt.λ_.f  (select second: t erased)
  uint32_t cfal_t = alloc(1);
  uint32_t cfal_f = alloc(1);
  heap_set(cfal_f, new_var(cfal_f));              // body of λf = f
  heap_set(cfal_t, mk(0, TAG_LAM, 0, cfal_f));   // body of λt = λf.f
  Term cfal = mk(0, TAG_LAM, LAM_ERA_MASK, cfal_t);  // λ_.λf.f

  // ctru = λt.λ_.t  (select first: f erased)
  uint32_t ctru_t = alloc(1);
  uint32_t ctru_f = alloc(1);
  heap_set(ctru_f, new_var(ctru_t));              // body of λf = t
  heap_set(ctru_t, mk(0, TAG_LAM, LAM_ERA_MASK, ctru_f));  // body of λt = λ_.t
  Term ctru = mk(0, TAG_LAM, 0, ctru_t);         // λt.λ_.t

  // λx. ((x cfal) ctru)
  uint32_t x_loc = alloc(1);
  Term body = new_app(new_app(new_var(x_loc), cfal), ctru);
  heap_set(x_loc, body);
  return mk(0, TAG_LAM, 0, x_loc);
}

// Build P_n applied to f directly (inlines the λf application).
// Returns a lambda representing f^(2^n).
static Term build_p_n(uint32_t n, Term f) {
  // d_prev = shared cell holding f
  uint32_t d_prev = alloc(1);
  heap_set(d_prev, f);

  // n-1 compose+dup steps
  for (uint32_t i = 0; i < n - 1; i++) {
    uint32_t k_loc = alloc(1);
    Term inner = new_app(new_dp1(LAB_P, d_prev), new_var(k_loc));
    Term outer = new_app(new_dp0(LAB_P, d_prev), inner);
    heap_set(k_loc, outer);
    Term lam_k = mk(0, TAG_LAM, 0, k_loc);
    uint32_t d_next = alloc(1);
    heap_set(d_next, lam_k);
    d_prev = d_next;
  }

  // Final compose (no dup)
  uint32_t k_loc = alloc(1);
  Term inner = new_app(new_dp1(LAB_P, d_prev), new_var(k_loc));
  Term outer = new_app(new_dp0(LAB_P, d_prev), inner);
  heap_set(k_loc, outer);
  return mk(0, TAG_LAM, 0, k_loc);
}

// Build P_n with DISTINCT labels per level to defeat optimal sharing.
// Forces DUP-SUP mismatch at every level, creating O(2^n) genuine work.
static Term build_p_n_expand(uint32_t n, Term f) {
  uint32_t d_prev = alloc(1);
  heap_set(d_prev, f);

  for (uint32_t i = 0; i < n - 1; i++) {
    uint32_t lab = i + 1;  // distinct label per level
    uint32_t k_loc = alloc(1);
    Term inner = new_app(new_dp1(lab, d_prev), new_var(k_loc));
    Term outer = new_app(new_dp0(lab, d_prev), inner);
    heap_set(k_loc, outer);
    Term lam_k = mk(0, TAG_LAM, 0, k_loc);
    uint32_t d_next = alloc(1);
    heap_set(d_next, lam_k);
    d_prev = d_next;
  }

  uint32_t final_lab = n;
  uint32_t k_loc = alloc(1);
  Term inner = new_app(new_dp1(final_lab, d_prev), new_var(k_loc));
  Term outer = new_app(new_dp0(final_lab, d_prev), inner);
  heap_set(k_loc, outer);
  return mk(0, TAG_LAM, 0, k_loc);
}

static void bench_cnots(uint32_t n) {
  printf("\n--- Benchmark: P%u(cnot)(ctru) ---\n", n);
  RESET();

  Term cnot_fn = build_cnot_bench();
  Term pn_cnot = build_p_n(n, cnot_fn);
  Term ctru_t  = build_ctru();

  // P_n(cnot)(ctru) — matches C benchmark @main = @P_n(@cnot, @ctru)
  Term expr = new_app(pn_cnot, ctru_t);
  uint32_t root = alloc(1);
  heap_set(root, expr);

  printf("initial heap: %u words\n", heap_top);

  metal_set_alloc_cursor(heap_top);

  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);

  uint64_t t0 = mach_absolute_time();
  uint64_t itrs = metal_normalize(root);
  uint64_t t1 = mach_absolute_time();

  double ns = (double)(t1 - t0) * tb.numer / tb.denom;
  double ms = ns / 1e6;

  Term result = metal_heap_read(root);

  printf("result: %s(ext=%u, val=%u)\n",
         tag_name(t_tag(result)), t_ext(result), t_val(result));
  printf("interactions: %llu\n", (unsigned long long)itrs);
  printf("time: %.2f ms\n", ms);
  if (ns > 0 && itrs > 0) {
    double mips = (double)itrs / (ns / 1e9) / 1e6;
    printf("speed: %.2f MIPS\n", mips);
  }

  if (t_tag(result) == TAG_LAM) {
    printf("PASS (result is LAM): ");
    print_metal_term(result);
  } else {
    printf("FAIL (expected LAM): ");
    print_metal_term(result);
  }
}

// ============================================================================
// Parallel Benchmark: 2^D independent P_k(cnot)(ctru) leaves in a SUP tree
// ============================================================================
// Total work ≈ 2^D × ~7×2^k interactions. With D+k=24, total ≈ 117M (same as P24).
// Increasing D increases parallelism while keeping total work constant.

static Term build_par_tree(uint32_t depth, uint32_t k) {
  if (depth == 0) {
    Term cnot_fn = build_cnot_bench();
    Term pn_cnot = build_p_n(k, cnot_fn);
    Term ctru_t  = build_ctru();
    return new_app(pn_cnot, ctru_t);
  }
  Term left  = build_par_tree(depth - 1, k);
  Term right = build_par_tree(depth - 1, k);
  return new_sup(LAB_TREE, left, right);
}

static void bench_parallel(uint32_t depth, uint32_t k) {
  uint32_t n_tasks = 1u << depth;
  printf("\n--- Parallel Benchmark: %u × P%u(cnot)(ctru) ---\n", n_tasks, k);
  RESET();

  Term tree = build_par_tree(depth, k);
  uint32_t root = alloc(1);
  heap_set(root, tree);

  printf("initial heap: %u words (%u tasks)\n", heap_top, n_tasks);

  metal_set_alloc_cursor(heap_top);

  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);

  uint64_t t0 = mach_absolute_time();
  uint64_t itrs = metal_normalize(root);
  uint64_t t1 = mach_absolute_time();

  double ns = (double)(t1 - t0) * tb.numer / tb.denom;
  double ms = ns / 1e6;

  Term result = metal_heap_read(root);

  printf("result: %s(ext=%u, val=%u)\n",
         tag_name(t_tag(result)), t_ext(result), t_val(result));
  printf("interactions: %llu\n", (unsigned long long)itrs);
  printf("time: %.2f ms\n", ms);
  if (ns > 0 && itrs > 0) {
    double mips = (double)itrs / (ns / 1e9) / 1e6;
    printf("speed: %.2f MIPS\n", mips);
  }

  if (depth == 0) {
    if (t_tag(result) == TAG_LAM) {
      printf("PASS (result is LAM)\n");
    } else {
      printf("FAIL (expected LAM, got %s)\n", tag_name(t_tag(result)));
    }
  } else {
    if (t_tag(result) == TAG_SUP) {
      printf("PASS (result is SUP)\n");
    } else {
      printf("FAIL (expected SUP, got %s)\n", tag_name(t_tag(result)));
    }
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    if (metal_init() != 0) {
      fprintf(stderr, "Failed to initialize Metal\n");
      return 1;
    }
    HEAP = metal_heap_ptr();

    int failures = 0;
    printf("--- HVM4 Metal Tests ---\n");

    // Basic interactions
    failures += test_app_lam();
    failures += test_app_lam_era();
    failures += test_app_era();
    failures += test_nested_app_lam();
    failures += test_dup_num();
    failures += test_dup_sup_match();
    failures += test_dup_sup_match_side1();
    failures += test_dup_sup_mismatch();
    failures += test_app_sup();
    failures += test_dup_lam();
    failures += test_church_2();

    // Additional base interactions
    failures += test_dup_era();
    failures += test_deep_beta();
    failures += test_dup_lam_both();
    failures += test_double_dup();

    // Church encoding (from _book_.hvm4)
    failures += test_cnot_true();
    failures += test_cnot_cnot();
    failures += test_c2_not_t();
    failures += test_c4_not_t();
    failures += test_c8_not_t();
    failures += test_cadd_c1_c4();
    failures += test_cadd_c2_c1();
    failures += test_cadd_c2_c4();
    failures += test_cadd_c4_c4();
    failures += test_cmul_c4_c2();
    failures += test_cmul_c4_c4();

    printf("--- %d failures ---\n", failures);

    // Sequential benchmarks (P8/P12/P16 only; P20/P24 take >1min)
    bench_cnots(8);
    bench_cnots(12);
    bench_cnots(16);

    // Parallel benchmarks (D+k=24, total work ≈ 117M interactions)
    bench_parallel(4, 20);   // 16 × P20
    bench_parallel(8, 16);   // 256 × P16
    bench_parallel(10, 14);  // 1024 × P14
    bench_parallel(11, 13);  // 2048 × P13
    bench_parallel(12, 12);  // 4096 × P12
    bench_parallel(14, 10);  // 16384 × P10
    bench_parallel(16, 8);   // 65536 × P8

    metal_shutdown();
    return failures > 0 ? 1 : 0;
  }
}
