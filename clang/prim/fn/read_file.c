#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
fn int term_string_to_utf8_cstr(Term src, char *dst, int cap, int *out_len, HStrErr *err);
fn Term term_string_from_hstrerr(const char *prim, const char *arg, int cap, HStrErr err);
fn Term term_string_printf(const char *fmt, ...);
fn int utf8_decode_next_bytes(const u8 *s, u32 len, u32 *idx, u32 *cp);

// %read_file(path)
// ----------------
// #OK{List<#CHR{NUM}>} | #ERR{String}
fn Term prim_fn_read_file(Term *args) {
  int MAX_PATH = 1024;
  char path[MAX_PATH]; // UTF-8 bytes
  const char *OPEN_PATH_ERR_FMT = "ERROR(read_file): failed to open path '%s': %s (errno=%d)";
  const char *READ_IO_ERR_FMT = "ERROR(read_file): I/O error while reading '%s': %s (errno=%d)";
  const char *INVALID_UTF8_FMT = "ERROR(read_file): invalid UTF-8 at byte index %i";
  const char *TRUNC_UTF8_FMT = "ERROR(read_file): truncated UTF-8 sequence at byte index %i";

  // Decode HVM path string (#CHR list) into `path` as UTF-8 bytes.
  HStrErr path_err;
  if (!term_string_to_utf8_cstr(args[0], path, MAX_PATH, NULL, &path_err)) {
    return term_string_from_hstrerr("read_file", "path", MAX_PATH, path_err);
  }

  FILE *file = fopen(path, "rb");
  if (!file) {
    int err = errno;
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(OPEN_PATH_ERR_FMT, path, strerror(err), err) });
  }

  Term Nil = term_new_ctr(NAM_NIL, 0, 0);
  Term result = Nil;
  Term curr = Nil;
  u8   has_node = 0;

  // Incremental UTF-8 decoder state.
  // `seq` stores bytes of the current candidate codepoint.
  u8 seq[4];
  int seq_len = 0;
  int byte_i = 0;

  u8 b;
  while (fread(&b, 1, 1, file) == 1) {
    if (seq_len >= 4) {
      int seq_start = byte_i - seq_len;
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(INVALID_UTF8_FMT, seq_start) });
    }
    seq[seq_len] = b;
    seq_len += 1;

    // Try to decode the current sequence from byte slice (not NUL-terminated).
    u32 seq_idx = 0;
    u32 cp = 0;
    int dec = utf8_decode_next_bytes(seq, (u32)seq_len, &seq_idx, &cp);
    if (dec == -2) {
      // Need more bytes for the current codepoint.
      byte_i += 1;
      continue;
    }
    if (dec < 0) {
      int seq_start = byte_i - (seq_len - 1);
      fclose(file);
      return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(INVALID_UTF8_FMT, seq_start) });
    }

    Term num = term_new_num(cp);
    Term chr = term_new_ctr(NAM_CHR, 1, &num);
    Term h_t[2] = {chr, Nil};
    Term node = term_new_ctr(NAM_CON, 2, h_t);

    if (!has_node) {
      result = node;
      has_node = 1;
    } else {
      heap_set(term_val(curr) + 1, node);
    }
    curr = node;

    seq_len = 0;
    byte_i += 1;
  }

  if (ferror(file)) {
    // Capture errno before fclose because fclose may overwrite it.
    int err = errno;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(READ_IO_ERR_FMT, path, strerror(err), err) });
  }

  if (seq_len != 0) {
    int seq_start = byte_i - seq_len;
    fclose(file);
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(TRUNC_UTF8_FMT, seq_start) });
  }

  fclose(file);
  return term_new_ctr(NAM_OK, 1, &result);
}

fn void prim_read_file_init(void) {
  prim_register("read_file", 9, 1, prim_fn_read_file);
}
