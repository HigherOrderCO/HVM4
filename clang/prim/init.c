fn void prim_init(void) {
  prim_log_init();
  prim_panic_init();
  prim_rand_init();
  prim_process_init();
  prim_timer_init();
  prim_read_bytes_init();
  prim_write_bytes_init();
  prim_read_file_init();
  prim_write_file_init();
}
