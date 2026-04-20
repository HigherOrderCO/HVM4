// %stream_file_open(path)
// -----------------------
// %stream_file_open_go_path(path)
fn Term prim_fn_stream_file_open(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("stream_file_open_go_path", 24), 1, args0);
  return wnf(t);
}

// %stream_file_open_go_path(path)
// -------------------------------
// Lift `path` over ERA/INC/SUP; default forwards to io stage.
fn Term stream_file_open_go_path(Term *args) {
  Term path_wnf = wnf(args[0]);

  switch (term_tag(path_wnf)) {
    case ERA: {
      // %stream_file_open_go_path(&{})
      // ------------------------------ stream-file-open-go-path-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %stream_file_open_go_path(↑x)
      // ----------------------------- stream-file-open-go-path-inc
      // ↑(%stream_file_open(x))
      u32  inc_loc = term_val(path_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("stream_file_open", 16), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %stream_file_open_go_path(&L{x,y})
      // ----------------------------------- stream-file-open-go-path-sup
      // &L{%stream_file_open(x), %stream_file_open(y)}
      u32  lab     = term_ext(path_wnf);
      u32  sup_loc = term_val(path_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("stream_file_open", 16), 1, &x);
      Term t1      = term_new_pri(table_find("stream_file_open", 16), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %stream_file_open_go_path(path)
      // ------------------------------- stream-file-open-go-path-default
      // %stream_file_open_go_io(path)
      Term args0[1] = {path_wnf};
      Term t        = term_new_pri(table_find("stream_file_open_go_io", 22), 1, args0);
      return wnf(t);
    }
  }
}

// %stream_file_open_go_io(path)
// -----------------------------
// #OK{#Strm{id,0}} | #ERR{String}
fn Term prim_fn_stream_file_open_go_io(Term *args) {
  int MAX_PATH = 4096;
  char path[MAX_PATH];

  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("stream_file_open", "path", MAX_PATH, path_err);
  }

  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return stream_new_err("stream_file_open", STREAM_ERR_IO, strerror(errno));
  }

  pthread_mutex_lock(&STREAM_LOCK);

  u32 id = STREAM_NEXT_ID;
  if (id >= STREAM_CAP) {
    pthread_mutex_unlock(&STREAM_LOCK);
    close(fd);
    return stream_new_err("stream_file_open", STREAM_ERR_FULL, "stream table is full");
  }

  STREAM_NEXT_ID = id + 1;
  STREAM_SLOTS[id].expected_seq = 0;
  STREAM_SLOTS[id].kind         = STREAM_KIND_FILE;
  STREAM_SLOTS[id].closed       = 0;
  STREAM_SLOTS[id].fd           = fd;

  pthread_mutex_unlock(&STREAM_LOCK);
  return stream_new_ok(stream_new_handle(id, 0));
}

fn void prim_stream_file_open_init(void) {
  prim_register("stream_file_open",         16, 1, prim_fn_stream_file_open);
  prim_register("stream_file_open_go_path", 24, 1, stream_file_open_go_path);
  prim_register("stream_file_open_go_io",   22, 1, prim_fn_stream_file_open_go_io);
}
