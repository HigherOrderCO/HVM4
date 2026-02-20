// JIT Module: Heap Bridge
// -----------------------
// Exposes runtime heap storage to generated JIT code.

// Exposes the runtime heap pointer to generated JIT code.
Term *jit_heap(void) {
  return HEAP;
}
