#include <unistd.h>

// %cwd(dummy)
// -----------
// #OK{String} | #ERR{String}
fn Term prim_fn_cwd(Term *args) {
  (void)args[0];

  int MAX_CWD = 4096;
  char cwd[MAX_CWD]; // UTF-8 bytes
  const char *GETCWD_ERR_FMT = "ERROR(cwd): failed to get current directory: %s (errno=%d)";

  if (getcwd(cwd, MAX_CWD) == NULL) {
    int err = errno;
    return term_new_ctr(SYM_ERR, 1, (Term[]){ term_string_printf(GETCWD_ERR_FMT, strerror(err), err) });
  }

  Term out = term_string_from_utf8(cwd);
  return term_new_ctr(SYM_OK, 1, &out);
}

fn void prim_cwd_init(void) {
  prim_register("cwd", 3, 1, prim_fn_cwd);
}
