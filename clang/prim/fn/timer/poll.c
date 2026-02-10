// %timer_poll(time)
// -----------------
// %timer_poll_go_time(time)
fn Term prim_fn_timer_poll(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("timer_poll_go_time", 18), 1, args0);
  return wnf(t);
}

// %timer_poll_go_time(time)
// -------------------------
// Lift `time` over ERA/INC/SUP; default forwards to io stage.
fn Term timer_poll_go_time(Term *args) {
  Term time_wnf = wnf(args[0]);

  switch (term_tag(time_wnf)) {
    case ERA: {
      // %timer_poll_go_time(&{})
      // ------------------------ timer-poll-go-time-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %timer_poll_go_time(↑x)
      // ----------------------- timer-poll-go-time-inc
      // ↑(%timer_poll(x))
      u32  inc_loc = term_val(time_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("timer_poll", 10), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %timer_poll_go_time(&L{x,y})
      // ---------------------------- timer-poll-go-time-sup
      // &L{%timer_poll(x), %timer_poll(y)}
      u32  lab     = term_ext(time_wnf);
      u32  sup_loc = term_val(time_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("timer_poll", 10), 1, &x);
      Term t1      = term_new_pri(table_find("timer_poll", 10), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %timer_poll_go_time(time)
      // ------------------------- timer-poll-go-time-default
      // %timer_poll_go_io(time)
      Term args0[1] = {time_wnf};
      Term t        = term_new_pri(table_find("timer_poll_go_io", 16), 1, args0);
      return wnf(t);
    }
  }
}

// %timer_poll_go_io(time)
// -----------------------
// #Pend{#Time{id,seq+1}} | #Rdy{#Time{id,seq+1}} | #ERR{String}
fn Term prim_fn_timer_poll_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!timer_parse_handle(args[0], &id, &seq)) {
    return timer_new_err("timer_poll", TIMER_ERR_BAD_HANDLE, "invalid `time`; expected #Time{id,seq}");
  }

  if (!timer_is_valid_id(id)) {
    return timer_new_err("timer_poll", TIMER_ERR_BAD_HANDLE, "unknown timer id");
  }

  u64 due_ns = 0;
  if (!timer_claim(id, seq, &due_ns)) {
    return timer_new_err("timer_poll", TIMER_ERR_STALE, "stale timer handle");
  }

  u64 now = timer_now_ns();
  if (now >= due_ns) {
    return timer_new_rdy(id, seq + 1);
  }
  return timer_new_pend(id, seq + 1);
}

fn void prim_timer_poll_init(void) {
  prim_register("timer_poll",         10, 1, prim_fn_timer_poll);
  prim_register("timer_poll_go_time", 18, 1, timer_poll_go_time);
  prim_register("timer_poll_go_io",   16, 1, prim_fn_timer_poll_go_io);
}
