// Runtime Main Evaluator
// ======================
// Runs one top-level entrypoint using shared CLI/AOT evaluation behavior.

// Forward declarations
// --------------------

fn Term eval_normalize(Term term);
fn void eval_collapse(Term root, int limit, int stats, int silent);
fn void wnf_set_itrs_enabled(int enabled);

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

  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start);

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

  clock_gettime(CLOCK_MONOTONIC, &end);

  u64 total_itrs = wnf_itrs_total();
  if (run.stats) {
    double dt = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double ips = total_itrs / dt;
    u64 total_heap = heap_alloc_total();

    printf("- Itrs: %llu interactions\n", total_itrs);
    if (thread_get_count() > 1) {
      for (u32 t = 0; t < thread_get_count(); t++) {
        printf("- Itrs[%u]: %llu interactions\n", t, wnf_itrs_thread(t));
      }
    }
    printf("- Heap: %llu nodes\n", total_heap);
    printf("- Time: %.3f seconds\n", dt);
    printf("- Perf: %.2f M interactions/s\n", ips / 1e6);
  } else if (run.silent) {
    printf("- Itrs: %llu interactions\n", total_itrs);
  }
}
