// %timer_start(ms)
// ----------------
// %timer_start_go_ms(ms)
fn Term prim_fn_timer_start(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("timer_start_go_ms", 17), 1, args0);
  return wnf(t);
}

// %timer_start_go_ms(ms)
// ----------------------
// Lift `ms` over ERA/INC/SUP; default forwards to io stage.
fn Term timer_start_go_ms(Term *args) {
  Term ms_wnf = wnf(args[0]);

  switch (term_tag(ms_wnf)) {
    case ERA: {
      // %timer_start_go_ms(&{})
      // ----------------------- timer-start-go-ms-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %timer_start_go_ms(↑x)
      // ---------------------- timer-start-go-ms-inc
      // ↑(%timer_start(x))
      u32  inc_loc = term_val(ms_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("timer_start", 11), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %timer_start_go_ms(&L{x,y})
      // --------------------------- timer-start-go-ms-sup
      // &L{%timer_start(x), %timer_start(y)}
      u32  lab     = term_ext(ms_wnf);
      u32  sup_loc = term_val(ms_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("timer_start", 11), 1, &x);
      Term t1      = term_new_pri(table_find("timer_start", 11), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %timer_start_go_ms(ms)
      // ---------------------- timer-start-go-ms-default
      // %timer_start_go_io(ms)
      Term args0[1] = {ms_wnf};
      Term t        = term_new_pri(table_find("timer_start_go_io", 17), 1, args0);
      return wnf(t);
    }
  }
}

// %timer_start_go_io(ms)
// ----------------------
// #Time{id,0} | #ERR{String}
fn Term prim_fn_timer_start_go_io(Term *args) {
  u32 ms = 0;
  if (!timer_parse_num(args[0], &ms)) {
    return timer_new_err("timer_start", TIMER_ERR_BAD_ARG, "invalid `ms`; expected NUM");
  }

  u64 now = timer_now_ns();
  u64 due = now + (u64)ms * 1000000ull;
  if (due < now) {
    due = UINT64_MAX;
  }

  pthread_mutex_lock(&TIMER_LOCK);

  u32 id = TIMER_NEXT_ID;
  if (id >= TIMER_CAP) {
    pthread_mutex_unlock(&TIMER_LOCK);
    return timer_new_err("timer_start", TIMER_ERR_FULL, "timer table is full");
  }

  TIMER_NEXT_ID = id + 1;
  TIMER_SLOTS[id].expected_seq = 0;
  TIMER_SLOTS[id].due_ns       = due;

  pthread_mutex_unlock(&TIMER_LOCK);
  return timer_new_time(id, 0);
}

fn void prim_timer_start_init(void) {
  prim_register("timer_start",       11, 1, prim_fn_timer_start);
  prim_register("timer_start_go_ms", 17, 1, timer_start_go_ms);
  prim_register("timer_start_go_io", 17, 1, prim_fn_timer_start_go_io);
}
