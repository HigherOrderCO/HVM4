// %process_wait(proc)
// -------------------
// %process_wait_go_proc(proc)
fn Term prim_fn_process_wait(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("process_wait_go_proc", 20), 1, args0);
  return wnf(t);
}

// %process_wait_go_proc(proc)
// ---------------------------
// Lift `proc` over ERA/INC/SUP; default forwards to io stage.
fn Term process_wait_go_proc(Term *args) {
  Term proc_wnf = wnf(args[0]);

  switch (term_tag(proc_wnf)) {
    case ERA: {
      // %process_wait_go_proc(&{})
      // --------------------------- process-wait-go-proc-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %process_wait_go_proc(↑x)
      // -------------------------- process-wait-go-proc-inc
      // ↑(%process_wait(x))
      u32  inc_loc = term_val(proc_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("process_wait", 12), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %process_wait_go_proc(&L{x,y})
      // ------------------------------- process-wait-go-proc-sup
      // &L{%process_wait(x), %process_wait(y)}
      u32  lab     = term_ext(proc_wnf);
      u32  sup_loc = term_val(proc_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("process_wait", 12), 1, &x);
      Term t1      = term_new_pri(table_find("process_wait", 12), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %process_wait_go_proc(proc)
      // --------------------------- process-wait-go-proc-default
      // %process_wait_go_io(proc)
      Term args0[1] = {proc_wnf};
      Term t        = term_new_pri(table_find("process_wait_go_io", 18), 1, args0);
      return wnf(t);
    }
  }
}

// %process_wait_go_io(proc)
// -------------------------
// #OK{#Rdy{#Proc{id,seq+1},#Exit{n}|#Sig{n}}} | #ERR{String}
fn Term prim_fn_process_wait_go_io(Term *args) {
  u32 id  = 0;
  u32 seq = 0;
  if (!process_parse_handle(args[0], &id, &seq)) {
    return process_new_err("process_wait", PROCESS_ERR_BAD_HANDLE, "invalid `proc`; expected #Proc{id,seq}");
  }

  if (!process_is_valid_id(id)) {
    return process_new_err("process_wait", PROCESS_ERR_BAD_HANDLE, "unknown process id");
  }

  pid_t pid      = 0;
  u8    finished = 0;
  u8    signaled = 0;
  u32   code     = 0;
  if (!process_claim(id, seq, &pid, &finished, &signaled, &code)) {
    return process_new_err("process_wait", PROCESS_ERR_STALE, "stale process handle");
  }

  if (finished) {
    return process_new_ok(process_new_rdy(id, seq + 1, signaled, code));
  }

  int status = 0;
  pid_t got  = 0;
  while (1) {
    got = waitpid(pid, &status, 0);
    if (got >= 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    return process_new_err("process_wait", PROCESS_ERR_IO, strerror(errno));
  }

  process_status_from_wait(status, &signaled, &code);
  process_set_finished(id, signaled, code);
  return process_new_ok(process_new_rdy(id, seq + 1, signaled, code));
}

fn void prim_process_wait_init(void) {
  prim_register("process_wait",         12, 1, prim_fn_process_wait);
  prim_register("process_wait_go_proc", 20, 1, process_wait_go_proc);
  prim_register("process_wait_go_io",   18, 1, prim_fn_process_wait_go_io);
}
