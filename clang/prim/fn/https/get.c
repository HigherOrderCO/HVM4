// %https_get(url)
// ---------------
// %https_get_go_url(url)
fn Term prim_fn_https_get(Term *args) {
  Term args0[1] = {args[0]};
  Term t        = term_new_pri(table_find("https_get_go_url", 16), 1, args0);
  return wnf(t);
}

// %https_get_go_url(url)
// ----------------------
// Lift `url` over ERA/INC/SUP; default forwards to io stage.
fn Term https_get_go_url(Term *args) {
  Term url_wnf = wnf(args[0]);

  switch (term_tag(url_wnf)) {
    case ERA: {
      // %https_get_go_url(&{})
      // ---------------------- https-get-go-url-era
      // &{}
      return term_new_era();
    }
    case INC: {
      // %https_get_go_url(↑x)
      // --------------------- https-get-go-url-inc
      // ↑(%https_get(x))
      u32  inc_loc = term_val(url_wnf);
      Term inner   = heap_read(inc_loc);
      Term next    = term_new_pri(table_find("https_get", 9), 1, &inner);
      heap_set(inc_loc, next);
      return term_new(0, INC, 0, inc_loc);
    }
    case SUP: {
      // %https_get_go_url(&L{x,y})
      // -------------------------- https-get-go-url-sup
      // &L{%https_get(x), %https_get(y)}
      u32  lab     = term_ext(url_wnf);
      u32  sup_loc = term_val(url_wnf);
      Term x       = heap_read(sup_loc + 0);
      Term y       = heap_read(sup_loc + 1);
      Term t0      = term_new_pri(table_find("https_get", 9), 1, &x);
      Term t1      = term_new_pri(table_find("https_get", 9), 1, &y);
      return term_new_sup(lab, t0, t1);
    }
    default: {
      // %https_get_go_url(url)
      // ---------------------- https-get-go-url-default
      // %https_get_go_io(url)
      Term args0[1] = {url_wnf};
      Term t        = term_new_pri(table_find("https_get_go_io", 15), 1, args0);
      return wnf(t);
    }
  }
}

fn int https_open_trunc_file(const char *path) {
  while (1) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      return fd;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }
}

fn int https_dup2_retry(int old_fd, int new_fd) {
  while (1) {
    if (dup2(old_fd, new_fd) >= 0) {
      return 1;
    }
    if (errno == EINTR) {
      continue;
    }
    return 0;
  }
}

fn void https_child_exec_get(
  const char *url,
  const char *hdr_path,
  const char *body_path,
  const char *meta_path,
  const char *err_path
) {
  int meta_fd = https_open_trunc_file(meta_path);
  if (meta_fd < 0) {
    _exit(127);
  }

  int err_fd = https_open_trunc_file(err_path);
  if (err_fd < 0) {
    close(meta_fd);
    _exit(127);
  }

  if (!https_dup2_retry(meta_fd, STDOUT_FILENO)) {
    close(meta_fd);
    close(err_fd);
    _exit(127);
  }

  if (!https_dup2_retry(err_fd, STDERR_FILENO)) {
    close(meta_fd);
    close(err_fd);
    _exit(127);
  }

  close(meta_fd);
  close(err_fd);

  execlp(
    "curl",
    "curl",
    "-sS",
    "-L",
    "-D", hdr_path,
    "-o", body_path,
    "-w", "%{http_code}\n",
    url,
    (char *)NULL
  );

  _exit(127);
}

// %https_get_go_io(url)
// ---------------------
// #OK{#Http{id,0}} | #ERR{String}
fn Term prim_fn_https_get_go_io(Term *args) {
  int MAX_URL = 8192;
  char url[MAX_URL];

  HStrErr url_err;
  if (!term_string_to_utf8_cstr(args[0], url, MAX_URL, NULL, &url_err)) {
    return term_string_from_hstrerr("https_get", "url", MAX_URL, url_err);
  }

  char *tmp_dir   = NULL;
  char *hdr_path  = NULL;
  char *body_path = NULL;
  char *meta_path = NULL;
  char *err_path  = NULL;

  if (!https_make_paths(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path)) {
    return https_new_err("https_get", HTTPS_ERR_IO, "failed to allocate temporary files");
  }

  pid_t pid = fork();
  if (pid < 0) {
    int err = errno;
    https_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path);
    return https_new_err("https_get", HTTPS_ERR_IO, strerror(err));
  }

  if (pid == 0) {
    https_child_exec_get(url, hdr_path, body_path, meta_path, err_path);
  }

  pthread_mutex_lock(&HTTPS_LOCK);

  u32 id = HTTPS_NEXT_ID;
  if (id >= HTTPS_CAP) {
    pthread_mutex_unlock(&HTTPS_LOCK);

    if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
      https_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path);
      return https_new_err("https_get", HTTPS_ERR_IO, strerror(errno));
    }

    int status = 0;
    pid_t got  = https_waitpid_retry(pid, &status, 0);
    if (got < 0 && errno != ECHILD) {
      https_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path);
      return https_new_err("https_get", HTTPS_ERR_IO, strerror(errno));
    }

    https_paths_free(&tmp_dir, &hdr_path, &body_path, &meta_path, &err_path);
    return https_new_err("https_get", HTTPS_ERR_FULL, "https table is full");
  }

  HTTPS_NEXT_ID = id + 1;

  HttpsSlot *slot       = &HTTPS_SLOTS[id];
  slot->expected_seq    = 0;
  slot->pid             = pid;
  slot->finished        = 0;
  slot->canceled        = 0;
  slot->signaled        = 0;
  slot->parsed          = 0;
  slot->code            = 0;
  slot->outcome         = term_new_era();
  slot->tmp_dir         = tmp_dir;
  slot->hdr_path        = hdr_path;
  slot->body_path       = body_path;
  slot->meta_path       = meta_path;
  slot->err_path        = err_path;

  pthread_mutex_unlock(&HTTPS_LOCK);
  return https_new_ok(https_new_http(id, 0));
}

fn void prim_https_get_init(void) {
  prim_register("https_get",        9,  1, prim_fn_https_get);
  prim_register("https_get_go_url", 16, 1, https_get_go_url);
  prim_register("https_get_go_io",  15, 1, prim_fn_https_get_go_io);
}
