#include <unistd.h>

static _Atomic u64 UID_PREFIX = 0;
static _Atomic u64 UID_COUNT  = 0;

fn u64 uid_get_prefix(void) {
  u64 prefix = atomic_load_explicit(&UID_PREFIX, memory_order_relaxed);
  if (prefix != 0) {
    return prefix;
  }

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  u64 seed = ((u64)ts.tv_sec << 32) ^ (u64)ts.tv_nsec ^ (u64)getpid();
  if (seed == 0) {
    seed = 1;
  }

  u64 expected = 0;
  if (atomic_compare_exchange_strong_explicit(
        &UID_PREFIX, &expected, seed, memory_order_relaxed, memory_order_relaxed)) {
    return seed;
  }

  return atomic_load_explicit(&UID_PREFIX, memory_order_relaxed);
}

// %uid(dummy)
// -----------
// String (guaranteed unique per process)
fn Term prim_fn_uid(Term *args) {
  (void)args[0];

  u64 prefix = uid_get_prefix();
  u64 n      = atomic_fetch_add_explicit(&UID_COUNT, 1, memory_order_relaxed);

  return term_string_printf("uid-%016llx-%016llx",
                            (unsigned long long)prefix,
                            (unsigned long long)n);
}

fn void prim_uid_init(void) {
  prim_register("uid", 3, 1, prim_fn_uid);
}
