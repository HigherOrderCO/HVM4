// JIT Module: Interaction Accounting
// ----------------------------------
// Maps compact JIT interaction codes back to runtime interaction counters.

// Maps compact JIT interaction codes to runtime interaction counters.
void jit_itr(u8 code) {
  switch (code) {
    case JIT_I_LAM: {
      ITRS_INC("APP-LAM");
      return;
    }
    case JIT_I_CTR_H: {
      ITRS_INC("APP-MAT-CTR-MAT");
      return;
    }
    case JIT_I_CTR_M: {
      ITRS_INC("APP-MAT-CTR-MIS");
      return;
    }
    case JIT_I_NUM_H: {
      ITRS_INC("APP-MAT-NUM-MAT");
      return;
    }
    case JIT_I_NUM_M: {
      ITRS_INC("APP-MAT-NUM-MIS");
      return;
    }
    default: {
      return;
    }
  }
}
