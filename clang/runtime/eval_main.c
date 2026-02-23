// Runtime Main Evaluator
// ======================
// Runs one top-level entrypoint using shared CLI/AOT evaluation behavior.

// Forward declarations
// --------------------

fn Term eval_normalize(Term term);
fn void eval_collapse(Term root, int limit, int stats, int silent);
fn void wnf_set_itrs_enabled(int enabled);

#if HVM_WINDOWS
static LARGE_INTEGER eval_main_perf_freq;
static int eval_main_perf_freq_init = 0;

static inline double eval_main_now_sec(void) {
  if (!eval_main_perf_freq_init) {
    QueryPerformanceFrequency(&eval_main_perf_freq);
    eval_main_perf_freq_init = 1;
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart / (double)eval_main_perf_freq.QuadPart;
}
#else
static inline double eval_main_now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

// Runtime Eval Main
// -----------------

// Evaluates one entrypoint and prints output/stats according to cfg.
fn void runtime_eval_main(u32 main_id, const RuntimeEvalCfg *cfg) {
  RuntimeEvalCfg run = {
    .do_collapse   = 0,
    .collapse_limit = -1,
    .stats         = 0,
    .silent        = 0,
    .step_by_step  = 0,
  };

  if (cfg != NULL) {
    run = *cfg;
  }

  int enable_itrs = run.stats || run.silent || run.step_by_step;
  wnf_set_itrs_enabled(enable_itrs);

  double start = eval_main_now_sec();

  Term main_ref = term_new_ref(main_id);
  if (run.do_collapse) {
    eval_collapse(main_ref, run.collapse_limit, run.stats, run.silent);
  } else {
    Term result = eval_normalize(main_ref);
    if (!run.silent && !run.step_by_step) {
      print_term(result);
      printf("\n");
    }
  }

  double end = eval_main_now_sec();

  u64 total_itrs = wnf_itrs_total();
  if (run.stats) {
    double dt = end - start;
    double ips = total_itrs / dt;
    u64 total_heap = heap_alloc_total();

    printf("- Itrs: %llu interactions\n", (unsigned long long)total_itrs);
    if (thread_get_count() > 1) {
      for (u32 t = 0; t < thread_get_count(); t++) {
        printf("- Itrs[%u]: %llu interactions\n", t, (unsigned long long)wnf_itrs_thread(t));
      }
    }
    printf("- Heap: %llu nodes\n", (unsigned long long)total_heap);
    printf("- Time: %.3f seconds\n", dt);
    printf("- Perf: %.2f M interactions/s\n", ips / 1e6);
  } else if (run.silent) {
    printf("- Itrs: %llu interactions\n", (unsigned long long)total_itrs);
  }
}
