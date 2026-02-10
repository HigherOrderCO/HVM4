// %stream_stdin_open(seed)
// ------------------------
// %stream_stdin_open_go_seed(seed)
fn Term prim_fn_stream_stdin_open(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("stream_stdin_open_go_seed", 25), 1, args0);
  return wnf(t);
}

// %stream_stdin_open_go_seed(seed)
// --------------------------------
// Lift `seed` over ERA/INC/SUP; default forwards to io stage.
fn Term stream_stdin_open_go_seed(Term *args) {
  Term seed_wnf = wnf(args[0]);

  switch (term_tag(seed_wnf)) {
    case ERA: {
      // %stream_stdin_open_go_seed(&{})
      // ------------------------------- stream-stdin-open-go-seed-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %stream_stdin_open_go_seed(↑x)
      // ------------------------------ stream-stdin-open-go-seed-inc
      // ↑(%stream_stdin_open(x))
      u32  inc_loc = term_val(seed_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("stream_stdin_open", 17), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %stream_stdin_open_go_seed(&L{x,y})
      // ------------------------------------ stream-stdin-open-go-seed-sup
      // &L{%stream_stdin_open(x), %stream_stdin_open(y)}
      u32  lab     = term_ext(seed_wnf);
      u32  sup_loc = term_val(seed_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("stream_stdin_open", 17), 1, &x);
      Term t1      = term_new_pri(table_find("stream_stdin_open", 17), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %stream_stdin_open_go_seed(seed)
      // -------------------------------- stream-stdin-open-go-seed-default
      // %stream_stdin_open_go_io(seed)
      Term args0[1] = {seed_wnf};
      Term t        = term_new_pri(table_find("stream_stdin_open_go_io", 23), 1, args0);
      return wnf(t);
    }
  }
}

// %stream_stdin_open_go_io(seed)
// -------------------------------
// #OK{#Strm{id,0}} | #ERR{String}
fn Term prim_fn_stream_stdin_open_go_io(Term *args) {
  (void)args;

  pthread_mutex_lock(&STREAM_LOCK);

  u32 id = STREAM_NEXT_ID;
  if (id >= STREAM_CAP) {
    pthread_mutex_unlock(&STREAM_LOCK);
    return stream_new_err("stream_stdin_open", STREAM_ERR_FULL, "stream table is full");
  }

  STREAM_NEXT_ID = id + 1;
  STREAM_SLOTS[id].expected_seq = 0;
  STREAM_SLOTS[id].kind         = STREAM_KIND_STDIN;
  STREAM_SLOTS[id].closed       = 0;
  STREAM_SLOTS[id].fd           = 0;

  pthread_mutex_unlock(&STREAM_LOCK);
  return stream_new_ok(stream_new_handle(id, 0));
}

fn void prim_stream_stdin_open_init(void) {
  prim_register("stream_stdin_open",         17, 1, prim_fn_stream_stdin_open);
  prim_register("stream_stdin_open_go_seed", 25, 1, stream_stdin_open_go_seed);
  prim_register("stream_stdin_open_go_io",   23, 1, prim_fn_stream_stdin_open_go_io);
}
