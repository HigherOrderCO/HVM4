fn void sys_error(const char *msg);

fn u32 nick_from_str(const char *name, u32 len) {
  if (len == 0) {
    sys_error("Empty name in name_from_str");
  }
  if (!nick_is_init(name[0])) {
    sys_error("Invalid name in name_from_str");
  }
  u32 k = 0;
  for (u32 i = 0; i < len; i++) {
    char c = name[i];
    if (!nick_is_char(c)) {
      sys_error("Invalid name in name_from_str");
    }
    k = ((k << 6) + nick_letter_to_b64(c)) & EXT_MASK;
  }
  return k;
}
