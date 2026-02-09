#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
fn int term_string_to_utf8_cstr(Term src, char *dst, int cap, int *out_len, HStrErr *err);
fn Term term_string_from_hstrerr(const char *prim, const char *arg, int cap, HStrErr err);
fn Term term_string_printf(const char *fmt, ...);

// %write_bytes(path, data)
// ------------------------
// #OK{#NIL} | #ERR{String}
fn Term prim_fn_write_bytes(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  int  data_i = 0;
  Term data_item = args[1];
  const char *DATA_EXPECTED = "ERROR(write_bytes): invalid `data`; expected #NIL or #CON(#BYT{NUM}, tail)";
  const char *OPEN_PATH_ERR_FMT = "ERROR(write_bytes): failed to open path '%s': %s (errno=%d)";
  const char *DATA_INVALID_BYTE_FMT = "ERROR(write_bytes): invalid byte %llu at `data` index %i; expected 0..255";
  const char *WRITE_IO_ERR_FMT = "ERROR(write_bytes): I/O error while writing '%s': %s (errno=%d)";
  const char *FLUSH_ERR_FMT = "ERROR(write_bytes): failed to flush '%s': %s (errno=%d)";
  const char *CLOSE_ERR_FMT = "ERROR(write_bytes): failed to close '%s': %s (errno=%d)";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("write_bytes", "path", MAX_PATH, path_err);
  }

  FILE *file = fopen(path, "wb");
  if (!file) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(OPEN_PATH_ERR_FMT, path, strerror(err), err) });
  }

  // Write hvm4 bytes list (#BYT[]) into `file`.
  data_item = wnf(data_item);
  while (term_tag(data_item) == C02) {
    // wnf(data_item) must be List<#BYT{b}>
    if (term_ext(data_item) != NAM_CON) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    Term head_loc = term_val(data_item);
    Term head     = heap_read(head_loc + 0);
    Term tail     = heap_read(head_loc + 1);
    head = wnf(head);

    // wnf(head) must be #BYT{b}
    if (term_tag(head) != C01 || term_ext(head) != NAM_BYT) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    Term b_loc = term_val(head);
    Term b_trm = heap_read(b_loc);
    b_trm = wnf(b_trm);

    // b in #BYT{b} must be NUM
    if (term_tag(b_trm) != NUM) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf("%s", DATA_EXPECTED) });
    }

    // NUM must fit one byte.
    u32 b = term_val(b_trm);
    if (b > 0xFF) {
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(DATA_INVALID_BYTE_FMT, (unsigned long long)b, data_i) });
    }

    unsigned char out = (unsigned char)b;
    if (fwrite(&out, 1, 1, file) != 1) {
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

fn void prim_write_bytes_init(void) {
  prim_register("write_bytes", 11, 2, prim_fn_write_bytes);
}
