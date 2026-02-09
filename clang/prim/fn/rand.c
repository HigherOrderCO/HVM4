#include <stdlib.h>

// %rand(dummy)
// ------------ rand
// NUM
fn Term prim_fn_rand(Term *args) {
  (void)args[0]; // dummy arg; arity 1 for now
  return term_new_num((u32)rand());
}

fn void prim_rand_init(void) {
  prim_register("rand", 4, 1, prim_fn_rand);
}
