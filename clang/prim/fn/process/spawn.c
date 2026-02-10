// %process_spawn(cmd)
// -------------------
// %process_spawn_go_cmd(cmd)
fn Term prim_fn_process_spawn(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("process_spawn_go_cmd", 20), 1, args0);
  return wnf(t);
}

// %process_spawn_go_cmd(cmd)
// --------------------------
// Lift `cmd` over ERA/INC/SUP; default forwards to io stage.
fn Term process_spawn_go_cmd(Term *args) {
  Term cmd_wnf = wnf(args[0]);

  switch (term_tag(cmd_wnf)) {
    case ERA: {
      // %process_spawn_go_cmd(&{})
      // -------------------------- process-spawn-go-cmd-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %process_spawn_go_cmd(↑x)
      // ------------------------- process-spawn-go-cmd-inc
      // ↑(%process_spawn(x))
      u32  inc_loc = term_val(cmd_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("process_spawn", 13), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %process_spawn_go_cmd(&L{x,y})
      // ------------------------------ process-spawn-go-cmd-sup
      // &L{%process_spawn(x), %process_spawn(y)}
      u32  lab     = term_ext(cmd_wnf);
      u32  sup_loc = term_val(cmd_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("process_spawn", 13), 1, &x);
      Term t1      = term_new_pri(table_find("process_spawn", 13), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %process_spawn_go_cmd(cmd)
      // -------------------------- process-spawn-go-cmd-default
      // %process_spawn_go_io(cmd)
      Term args0[1] = {cmd_wnf};
      Term t        = term_new_pri(table_find("process_spawn_go_io", 19), 1, args0);
      return wnf(t);
    }
  }
}

// %process_spawn_go_io(cmd)
// -------------------------
// #OK{#Proc{id,0}} | #ERR{String}
fn Term prim_fn_process_spawn_go_io(Term *args) {
  int MAX_CMD = 4096;
  char cmd[MAX_CMD];

  HStrErr cmd_err;
  if (!term_string_to_utf8_cstr(args[0], cmd, MAX_CMD, NULL, &cmd_err)) {
    return term_string_from_hstrerr("process_spawn", "cmd", MAX_CMD, cmd_err);
  }

  pid_t pid = fork();
  if (pid < 0) {
    int err = errno;
    return process_new_err("process_spawn", PROCESS_ERR_IO, strerror(err));
  }

  if (pid == 0) {
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }

  pthread_mutex_lock(&PROCESS_LOCK);

  u32 id = PROCESS_NEXT_ID;
  if (id >= PROCESS_CAP) {
    pthread_mutex_unlock(&PROCESS_LOCK);
    kill(pid, SIGKILL);
    return process_new_err("process_spawn", PROCESS_ERR_FULL, "process table is full");
  }

  PROCESS_NEXT_ID = id + 1;
  PROCESS_SLOTS[id].expected_seq = 0;
  PROCESS_SLOTS[id].pid          = pid;
  PROCESS_SLOTS[id].finished     = 0;
  PROCESS_SLOTS[id].signaled     = 0;
  PROCESS_SLOTS[id].code         = 0;

  pthread_mutex_unlock(&PROCESS_LOCK);
  return process_new_ok(process_new_proc(id, 0));
}

fn void prim_process_spawn_init(void) {
  prim_register("process_spawn",        13, 1, prim_fn_process_spawn);
  prim_register("process_spawn_go_cmd", 20, 1, process_spawn_go_cmd);
  prim_register("process_spawn_go_io",  19, 1, prim_fn_process_spawn_go_io);
}
