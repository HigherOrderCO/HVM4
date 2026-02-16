fn u32 parse_char_esc(PState *s) {
  if (parse_peek(s) == 0) {
    parse_error(s, "character", 0);
  }
  if (parse_peek(s) == '\\') {
    parse_advance(s);
    char c = parse_peek(s);
    if (c == 0) {
      parse_error(s, "character", 0);
    }
    parse_advance(s);
    switch (c) {
      case 'n': return '\n';
      case 't': return '\t';
      case 'r': return '\r';
      case '0': return '\0';
      case '\\': return '\\';
      case '\'': return '\'';
      case '"': return '"';
      default: return (u32)(u8)c;
    }
  }
  return parse_utf8(s);
}

fn u32 parse_char_lit(PState *s) {
  parse_advance(s);
  u32 c = parse_char_esc(s);
  if (parse_peek(s) != '\'') {
    parse_error(s, "'", parse_peek(s));
  }
  parse_advance(s);
  return c;
}

fn Term parse_term_chr(PState *s) {
  u32 c = parse_char_lit(s);
  Term n = term_new_num(c);
  return term_new_ctr(SYM_CHR, 1, &n);
}
