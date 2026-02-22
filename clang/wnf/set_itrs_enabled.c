// WNF Interaction Counter Toggle
// ==============================
// Controls whether WNF/AOT interaction counters are active.

// Enables or disables interaction counting.
fn void wnf_set_itrs_enabled(int enabled) {
  ITRS_ENABLED = enabled != 0;
}
