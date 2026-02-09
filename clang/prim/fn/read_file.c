#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
fn int term_string_to_utf8_cstr(Term src, char *dst, int cap, int *out_len, HStrErr *err);
fn Term term_string_from_hstrerr(const char *prim, const char *arg, int cap, HStrErr err);
fn Term term_string_printf(const char *fmt, ...);

fn int utf8_expected_len(u8 b0) {
  if (b0 < 0x80) {
    return 1;
  }
  if ((b0 & 0xE0) == 0xC0) {
    return 2;
  }
  if ((b0 & 0xF0) == 0xE0) {
    return 3;
  }
  if ((b0 & 0xF8) == 0xF0) {
    return 4;
  }
  return -1;
}

fn int utf8_decode_seq(const u8 seq[4], int n, u32 *cp) {
  u8 b0 = seq[0];
  if (n == 1) {
    *cp = b0;
    return 1;
  }

  if (n == 2) {
    u8 b1 = seq[1];
    if ((b1 & 0xC0) != 0x80) {
      return 0;
    }
    u32 x = ((u32)(b0 & 0x1F) << 6) | (u32)(b1 & 0x3F);
    if (x < 0x80) {
      return 0;
    }
    *cp = x;
    return 1;
  }

  if (n == 3) {
    u8 b1 = seq[1];
    u8 b2 = seq[2];
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
      return 0;
    }
    u32 x = ((u32)(b0 & 0x0F) << 12)
          | ((u32)(b1 & 0x3F) << 6)
          | ((u32)(b2 & 0x3F));
    if (x < 0x800 || (x >= 0xD800 && x <= 0xDFFF)) {
      return 0;
    }
    *cp = x;
    return 1;
  }

  if (n == 4) {
    u8 b1 = seq[1];
    u8 b2 = seq[2];
    u8 b3 = seq[3];
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
      return 0;
    }
    u32 x = ((u32)(b0 & 0x07) << 18)
          | ((u32)(b1 & 0x3F) << 12)
          | ((u32)(b2 & 0x3F) << 6)
          | ((u32)(b3 & 0x3F));
    if (x < 0x10000 || x > 0x10FFFF) {
      return 0;
    }
    *cp = x;
    return 1;
  }

  return 0;
}

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
  u8 seq[4];
  int seq_len = 0;
  int seq_need = 0;
  int byte_i = 0;

  u8 b;
  while (fread(&b, 1, 1, file) == 1) {
    if (seq_len == 0) {
      seq[0] = b;
      seq_len = 1;
      seq_need = utf8_expected_len(b);
      if (seq_need < 0) {
        fclose(file);
        return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(INVALID_UTF8_FMT, byte_i) });
      }
    } else {
      // Continuation bytes must start with bits `10`.
      if ((b & 0xC0) != 0x80) {
        int seq_start = byte_i - seq_len;
        fclose(file);
        return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(INVALID_UTF8_FMT, seq_start) });
      }
      seq[seq_len] = b;
      seq_len += 1;
    }

    if (seq_len == seq_need) {
      u32 cp = 0;
      if (!utf8_decode_seq(seq, seq_need, &cp)) {
        int seq_start = byte_i - (seq_need - 1);
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
      seq_need = 0;
    }

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
