#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
fn int term_string_to_utf8_cstr(Term src, char *dst, int cap, int *out_len, HStrErr *err);
fn Term term_string_from_hstrerr(const char *prim, const char *arg, int cap, HStrErr err);
fn int utf8_encode_scalar(u32 cp, char out[4]);
fn Term term_string_printf(const char *fmt, ...);

// %write_file(path, data)
// -----------------------
// #OK{#NIL} | #ERR{String}
fn Term prim_fn_write_file(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  int  data_i = 0;
  Term data_item = args[1];
  const char *DATA_EXPECTED = "ERROR(write_file): invalid `data`; expected #NIL or #CON(#CHR{NUM}, tail)";
  const char *OPEN_PATH_ERR_FMT = "ERROR(write_file): failed to open path '%s': %s (errno=%d)";
  const char *DATA_INVALID_CP_FMT = "ERROR(write_file): invalid UTF-32 codepoint U+%08llX at `data` index %i";
  const char *WRITE_IO_ERR_FMT = "ERROR(write_file): I/O error while writing '%s': %s (errno=%d)";
  const char *FLUSH_ERR_FMT = "ERROR(write_file): failed to flush '%s': %s (errno=%d)";
  const char *CLOSE_ERR_FMT = "ERROR(write_file): failed to close '%s': %s (errno=%d)";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("write_file", "path", MAX_PATH, path_err);
  }

  FILE *file = fopen(path, "wb");
  if (!file) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(OPEN_PATH_ERR_FMT, path, strerror(err), err) });
  }

  // Write hvm4 string List<#CHR{NUM}> into file as UTF-8 bytes.
  data_item = wnf(data_item);
  while (term_tag(data_item) == C02) {
    // wnf(data_item) must be List<#CHR{c}>
    if (term_ext(data_item) != NAM_CON) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    Term head_loc = term_val(data_item);
    Term head     = heap_read(head_loc + 0);
    Term tail     = heap_read(head_loc + 1);
    head = wnf(head);

    // wnf(head) must be #CHR{c}
    if (term_tag(head) != C01 || term_ext(head) != NAM_CHR) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    Term c_loc = term_val(head);
    Term c_trm = wnf(heap_read(c_loc));

    // c in #CHR{c} must be NUM
    if (term_tag(c_trm) != NUM) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    // Encode UTF-32 codepoint (NUM) into UTF-8 bytes.
    u32 cp = term_val(c_trm);
    char cp_utf8[4];
    int n_bytes = utf8_encode_scalar(cp, cp_utf8);
    if (n_bytes < 0) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(DATA_INVALID_CP_FMT, (unsigned long long)cp, data_i) });
    }

    if (fwrite(cp_utf8, 1, (size_t)n_bytes, file) != (size_t)n_bytes) {
      // Capture errno before fclose because fclose may overwrite it.
      int err = errno;
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(WRITE_IO_ERR_FMT, path, strerror(err), err) });
    }

    data_i += 1;
    // Recurse
    data_item = wnf(tail);
  }

  if (term_tag(data_item) != C00 || term_ext(data_item) != NAM_NIL) {
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
  }

  if (fflush(file) != 0) {
    // Capture errno before fclose because fclose may overwrite it.
    int err = errno;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(FLUSH_ERR_FMT, path, strerror(err), err) });
  }

  if (fclose(file) != 0) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(CLOSE_ERR_FMT, path, strerror(err), err) });
  }

  Term Nil = term_new_ctr(NAM_NIL, 0, 0);
  return term_new_ctr(NAM_OK, 1, &Nil);
}

fn void prim_write_file_init(void) {
  prim_register("write_file", 10, 2, prim_fn_write_file);
}
