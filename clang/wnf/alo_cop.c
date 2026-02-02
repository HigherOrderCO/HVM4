// @{s} n₀
// ------- ALO-DP0
// s[n]₀ or n₀ when substitution missing (n is a de Bruijn level)
//
// @{s} n₁
// ------- ALO-DP1
// s[n]₁ or n₁ when substitution missing (n is a de Bruijn level)
fn Term wnf_alo_cop(u32 ls, u32 len, u32 lvl, u32 lab, u8 side, u8 tag) {
  if (lvl == 0 || lvl > len) {
    return term_new(0, tag, lab, lvl);
  }
  u32 idx = len - lvl;
  u32 it  = ls;
  for (u32 i = 0; i < idx && it != 0; i++) {
    it = (u32)(heap_read(it) & 0xFFFFFFFF);
  }
  u32 bind = (it != 0) ? (u32)(heap_read(it) >> 32) : 0;
  u8  rtag;
  if (tag == BG0 || tag == BG1) {
    rtag = side == 0 ? GT0 : GT1;
  } else {
    rtag = side == 0 ? DP0 : DP1;
  }
  return bind ? term_new(0, rtag, lab, bind) : term_new(0, tag, lab, lvl);
}
