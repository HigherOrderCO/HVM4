#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// Type helpers
typedef enum HStrErrKind HStrErrKind;
typedef struct HStrErr HStrErr;

// UTF-8 helpers
fn int utf8_encode_scalar(u32 cp, char out[4]);
fn int utf8_decode_next_cstr(const char *s, u32 *idx, u32 *cp);

// Term <-> c string, conversion helpers
fn int  term_chr_from_scalar(u32 cp, Term *out);
fn Term term_string_from_utf8(const char *s);
fn Term term_string_vprintf(const char *fmt, va_list ap);
fn Term term_string_printf(const char *fmt, ...);
fn int  term_string_to_utf8_cstr(Term src, char *dst, int cap, int *out_len, HStrErr *err);
fn Term term_string_from_hstrerr(const char *prim, const char *arg, int cap, HStrErr err);

// Local utility
fn void hstr_set(HStrErr *err, HStrErrKind kind, int index, int bytes, u64 cp);

enum HStrErrKind {
  HSTR_OK = 0,
  HSTR_BAD_SHAPE, // Not #NIL / #CON(#CHR{NUM}, tail)
  HSTR_BAD_CP,    // Invalid UTF-32 scalar value in #CHR{NUM}
  HSTR_TOO_LONG,  // Destination C buffer cannot fit UTF-8 + trailing '\0'
};

struct HStrErr {
  HStrErrKind kind;
  int         index; // Codepoint index in the source HVM list.
  int         bytes; // Current/required UTF-8 bytes in destination buffer.
  u64         cp;    // Offending codepoint for HSTR_BAD_CP.
};


// Small helper to fill an error struct only when caller asked for diagnostics.
fn void hstr_set(HStrErr *err, HStrErrKind kind, int index, int bytes, u64 cp) {
  if (err == NULL) {
    return;
  }
  err->kind  = kind;
  err->index = index;
  err->bytes = bytes;
  err->cp    = cp;
}

// Encode one UTF-32 codepoint into UTF-8 bytes.
// Returns 1..4 on success, -1 on invalid scalar value.
fn int utf8_encode_scalar(u32 cp, char out[4]) {
  if (cp <= 0x7F) {
    out[0] = (char)cp;
    return 1;
  }

  if (cp <= 0x7FF) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }

  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return -1;
  }

  if (cp <= 0xFFFF) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }

  if (cp <= 0x10FFFF) {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }

  return -1;
}

// Decode one UTF-8 codepoint from a NUL-terminated C string at byte index `*idx`.
// Returns: 0=end of string, 1..4=bytes consumed, -1=invalid/truncated sequence.
fn int utf8_decode_next_cstr(const char *s, u32 *idx, u32 *cp) {
  const u8 *p = (const u8 *)s + *idx;
  u8        b0 = p[0];

  if (b0 == 0) {
    return 0;
  }

  if (b0 < 0x80) {
    *cp  = b0;
    *idx = *idx + 1;
    return 1;
  }

  if ((b0 & 0xE0) == 0xC0) {
    u8 b1 = p[1];
    if (b1 == 0 || (b1 & 0xC0) != 0x80) {
      return -1;
    }
    u32 x = ((u32)(b0 & 0x1F) << 6) | (u32)(b1 & 0x3F);
    if (x < 0x80) {
      return -1;
    }
    *cp  = x;
    *idx = *idx + 2;
    return 2;
  }

  if ((b0 & 0xF0) == 0xE0) {
    u8 b1 = p[1];
    if (b1 == 0 || (b1 & 0xC0) != 0x80) {
      return -1;
    }
    u8 b2 = p[2];
    if (b2 == 0 || (b2 & 0xC0) != 0x80) {
      return -1;
    }
    u32 x = ((u32)(b0 & 0x0F) << 12)
          | ((u32)(b1 & 0x3F) << 6)
          | ((u32)(b2 & 0x3F));
    if (x < 0x800 || (x >= 0xD800 && x <= 0xDFFF)) {
      return -1;
    }
    *cp  = x;
    *idx = *idx + 3;
    return 3;
  }

  if ((b0 & 0xF8) == 0xF0) {
    u8 b1 = p[1];
    if (b1 == 0 || (b1 & 0xC0) != 0x80) {
      return -1;
    }
    u8 b2 = p[2];
    if (b2 == 0 || (b2 & 0xC0) != 0x80) {
      return -1;
    }
    u8 b3 = p[3];
    if (b3 == 0 || (b3 & 0xC0) != 0x80) {
      return -1;
    }
    u32 x = ((u32)(b0 & 0x07) << 18)
          | ((u32)(b1 & 0x3F) << 12)
          | ((u32)(b2 & 0x3F) << 6)
          | ((u32)(b3 & 0x3F));
    if (x < 0x10000 || x > 0x10FFFF) {
      return -1;
    }
    *cp  = x;
    *idx = *idx + 4;
    return 4;
  }

  return -1;
}

// Build #CHR{NUM} from a UTF-32 scalar.
// Returns 1 on success, 0 if scalar is invalid.
fn int term_chr_from_scalar(u32 cp, Term *out) {
  if (cp > 0x10FFFF) {
    return 0;
  }

  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return 0;
  }

  Term num = term_new_num(cp);
  *out = term_new_ctr(NAM_CHR, 1, &num);
  return 1;
}

// NOTE: Assumes `s` is a NUL-terminated UTF-8 C string.
fn Term term_string_from_utf8(const char *s) {
  Term nil      = term_new_ctr(NAM_NIL, 0, 0);
  Term out      = nil;
  Term cur      = nil;
  u32  idx      = 0;
  u8   has_node = 0;

  while (1) {
    u32 cp = 0;
    int n  = utf8_decode_next_cstr(s, &idx, &cp);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      // Replace one invalid byte with U+FFFD and move forward.
      cp  = 0xFFFD;
      idx = idx + 1;
    }

    Term chr;
    if (!term_chr_from_scalar(cp, &chr)) {
      Term repl;
      term_chr_from_scalar(0xFFFD, &repl);
      chr = repl;
    }

    Term args[2] = {chr, nil};
    Term node    = term_new_ctr(NAM_CON, 2, args);

    if (!has_node) {
      out      = node;
      has_node = 1;
    } else {
      heap_set(term_val(cur) + 1, node);
    }
    cur = node;
  }

  return out;
}

// Format with libc and convert the resulting UTF-8 C string to HVM String.
// On failure, returns sentinel strings like "<format-error>" or "<oom>".
fn Term term_string_vprintf(const char *fmt, va_list ap) {
  // First pass asks libc for the exact byte count (without NUL).
  va_list ap_len;
  va_copy(ap_len, ap);
  int need = vsnprintf(NULL, 0, fmt, ap_len);
  va_end(ap_len);

  if (need < 0) {
    return term_string_from_utf8("<format-error>");
  }

  size_t cap = (size_t)need + 1;
  char  *buf = malloc(cap);
  if (buf == NULL) {
    return term_string_from_utf8("<oom>");
  }

  // Second pass emits the formatted UTF-8 bytes, then we decode to HVM #CHR list.
  va_list ap_fmt;
  va_copy(ap_fmt, ap);
  int got = vsnprintf(buf, cap, fmt, ap_fmt);
  va_end(ap_fmt);

  if (got < 0 || (size_t)got >= cap) {
    free(buf);
    return term_string_from_utf8("<format-error>");
  }

  Term out = term_string_from_utf8(buf);
  free(buf);
  return out;
}

// Variadic wrapper over `term_string_vprintf`.
fn Term term_string_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  Term out = term_string_vprintf(fmt, ap);
  va_end(ap);
  return out;
}

// Decode an HVM String into a NUL-terminated UTF-8 C string.
//
// src: #NIL or #CON(#CHR{NUM}, tail).
// dst: ptr to destinatation c string
// cap: max size of `dst` includes space for the trailing '\0'.
// out_len: final size of dst
// err: ptr to HStrErr to keep error information
//
// Returns 1 on success.
// Returns 0 on failure and fills `err` when provided.
// On failure, `dst` may be partially written.
fn int term_string_to_utf8_cstr(Term src, char *dst, int cap, int *out_len, HStrErr *err) {
  // Walk the HVM list, validate each node, encode codepoints to UTF-8, append to `dst`.
  int  len = 0;        // UTF-8 bytes already written to dst (without trailing '\0')
  int  i   = 0;        // Codepoint index in the HVM string, for error printing
  Term cur = wnf(src); // Current list node while traversing `src`

  if (dst == NULL || cap <= 0) {
    hstr_set(err, HSTR_TOO_LONG, 0, 0, 0);
    return 0;
  }

  while (term_tag(cur) == C02 && len < cap) {
    // wnf(cur) must be List<#CHR{c}>
    if (term_ext(cur) != NAM_CON) {
      hstr_set(err, HSTR_BAD_SHAPE, i, len, 0);
      return 0;
    }

    Term loc  = term_val(cur);
    Term head = wnf(heap_read(loc + 0));
    Term tail = heap_read(loc + 1);

    // wnf(head) must be #CHR{c}
    if (term_tag(head) != C01 || term_ext(head) != NAM_CHR) {
      hstr_set(err, HSTR_BAD_SHAPE, i, len, 0);
      return 0;
    }

    // c must be NUM
    Term c = wnf(heap_read(term_val(head)));
    if (term_tag(c) != NUM) {
      hstr_set(err, HSTR_BAD_SHAPE, i, len, 0);
      return 0;
    }

    u32 cp = term_val(c); // UTF-32 scalar stored in #CHR{NUM}
    char cp_utf8[4];
    int n = utf8_encode_scalar(cp, cp_utf8);
    if (n < 0) {
      hstr_set(err, HSTR_BAD_CP, i, len, cp);
      return 0;
    }
    if (len + n >= cap) {
      hstr_set(err, HSTR_TOO_LONG, i, len + n, cp);
      return 0;
    }
    memcpy(dst + len, cp_utf8, n);
    len += n;
    i += 1;

    cur = wnf(tail);
  }

  if (len >= cap) {
    hstr_set(err, HSTR_TOO_LONG, i, len, 0);
    return 0;
  }
  if (term_tag(cur) != C00 || term_ext(cur) != NAM_NIL) {
    hstr_set(err, HSTR_BAD_SHAPE, i, len, 0);
    return 0;
  }

  dst[len] = '\0';
  if (out_len != NULL) {
    *out_len = len;
  }
  hstr_set(err, HSTR_OK, i, len, 0);
  return 1;
}

// Convert a decoding error into #ERR{String}.
// Prefix is "ERROR(<prim>): " when `prim` is non-empty, else "ERROR: ".
fn Term term_string_from_hstrerr(const char *prim, const char *arg, int cap, HStrErr err) {
  int has_prim = prim != NULL && prim[0] != '\0'; // Whether to include primitive function name in prefix
  if (arg == NULL || arg[0] == '\0') {
    arg = "arg";
  }

  char prefix[256]; // Shared prefix reused by all error message variants
  if (has_prim) {
    // If prefix formatting fails/overflows, fall back to generic prefix.
    int n = snprintf(prefix, sizeof(prefix), "ERROR(%s): ", prim);
    if (n < 0 || n >= (int)sizeof(prefix)) {
      snprintf(prefix, sizeof(prefix), "ERROR: ");
    }
  } else {
    snprintf(prefix, sizeof(prefix), "ERROR: ");
  }

  Term msg; // Message payload for #ERR
  switch (err.kind) {
    case HSTR_BAD_SHAPE:
      msg = term_string_printf("%sinvalid `%s`; expected #NIL or #CON(#CHR{NUM}, tail)", prefix, arg);
      break;
    case HSTR_BAD_CP:
      msg = term_string_printf("%sinvalid UTF-32 codepoint U+%08llX at `%s` index %i",
                          prefix, (unsigned long long)err.cp, arg, err.index);
      break;
    case HSTR_TOO_LONG:
      msg = term_string_printf("%s`%s` too long at index %i: %i UTF-8 bytes (max %i incl. NUL)",
                          prefix, arg, err.index, err.bytes, cap);
      break;
    default:
      msg = term_string_printf("%sinvalid `%s`", prefix, arg);
      break;
  }
  return term_new_ctr(NAM_ERR, 1, &msg);
}
