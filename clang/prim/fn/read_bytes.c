#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
fn void print_term(Term term);
fn int term_string_to_utf8_cstr(Term src, char *dst, int cap, int *out_len, HStrErr *err);
fn Term term_string_from_hstrerr(const char *prim, const char *arg, int cap, HStrErr err);
fn Term term_string_printf(const char *fmt, ...);

// %read_bytes(path)
// ----------------
// #OK{List<#BYT{NUM}>} | #ERR{String}
fn Term prim_fn_read_bytes(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  const char *OPEN_PATH_ERR_FMT = "ERROR(read_bytes): failed to open path '%s': %s (errno=%d)";
  const char *READ_IO_ERR_FMT = "ERROR(read_bytes): I/O error while reading '%s': %s (errno=%d)";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("read_bytes", "path", MAX_PATH, path_err);
  }

  // Open file and build an HVM list of #BYT{NUM}.
  FILE *file = fopen(path, "rb");
  if (!file) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(OPEN_PATH_ERR_FMT, path, strerror(err), err) });
  }

  // Initialize output list (empty file => #OK{#NIL}).
  Term Nil = term_new_ctr(NAM_NIL, 0, 0);
  unsigned char c;
  // First read distinguishes empty file (EOF) from read error.
  if (fread(&c, 1,1, file) != 1) {
    if (ferror(file)) {
      // Capture errno before fclose because fclose may overwrite it.
      int err = errno;
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(READ_IO_ERR_FMT, path, strerror(err), err) });
    }
    // First read hit EOF: file is empty, so payload is #NIL.
    fclose(file);
    return term_new_ctr(NAM_OK, 1, &Nil);
  }
  Term byt[1] = {term_new_num(c)};
  Term h_t[2] = {term_new_ctr(NAM_BYT, 1, byt), Nil};
  Term result = term_new_ctr(NAM_CON, 2, h_t); // at each step, the list ends in #NIL

  // `curr` is the last #CON in the output List<#BYT{NUM}>.
  Term curr = result;
  while (fread(&c, 1,1, file) == 1) {
    byt[0] = term_new_num(c);
    h_t[0] = term_new_ctr(NAM_BYT, 1, byt);
    // Append #CON{#BYT{NUM}, #NIL} at curr tail.
    heap_set(term_val(curr) + 1, term_new_ctr(NAM_CON, 2, h_t));
    curr = heap_read(term_val(curr) + 1);
  }

  if (ferror(file)) {
    // Capture errno before fclose because fclose may overwrite it.
    int err = errno;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(READ_IO_ERR_FMT, path, strerror(err), err) });
  }
  fclose(file);
  return term_new_ctr(NAM_OK, 1, &result);
}

fn void prim_read_bytes_init(void) {
  prim_register("read_bytes", 10, 1, prim_fn_read_bytes);
}
