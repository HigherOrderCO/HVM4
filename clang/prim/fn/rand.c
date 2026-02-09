fn Term rand_go_dummy(Term *args);

// %rand(dummy)
// ------------
// %rand_go_dummy(dummy)
fn Term prim_fn_rand(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("rand_go_dummy", 13), 1, args0);
  return wnf(t);
}

// %rand_go_dummy(dummy)
// ---------------------
// Lift dummy over ERA/INC/SUP; default ignores dummy and returns fresh NUM.
fn Term rand_go_dummy(Term *args) {
  Term dummy_wnf = wnf(args[0]);

  switch (term_tag(dummy_wnf)) {
    case ERA: {
      // %rand_go_dummy(&{})
      // ------------------- rand-go-dummy-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %rand_go_dummy(↑x)
      // ------------------ rand-go-dummy-inc
      // ↑(%rand(x))
      u32  inc_loc = term_val(dummy_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("rand", 4), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %rand_go_dummy(&L{x,y})
      // ----------------------- rand-go-dummy-sup
      // &L{%rand(x), %rand(y)}
      u32  lab     = term_ext(dummy_wnf);
      u32  sup_loc = term_val(dummy_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("rand", 4), 1, &x);
      Term t1      = term_new_pri(table_find("rand", 4), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %rand_go_dummy(x)
      // ----------------- rand-go-dummy-default
      // NUM
      (void)dummy_wnf;
      return term_new_num((u32)rand());
    }
  }
}

fn void prim_rand_init(void) {
  prim_register("rand", 4, 1, prim_fn_rand);
  prim_register("rand_go_dummy", 13, 1, rand_go_dummy);
}
