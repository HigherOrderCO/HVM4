fn void cdef_reset(u32 cap) {
  if (CDEF_FUNS != NULL) {
    free(CDEF_FUNS);
    CDEF_FUNS = NULL;
  }
  if (CDEF_ARIS != NULL) {
    free(CDEF_ARIS);
    CDEF_ARIS = NULL;
  }
  CDEF_CAP = 0;

  if (cap == 0) {
    return;
  }

  CDEF_FUNS = (CDefFun*)calloc(cap, sizeof(CDefFun));
  CDEF_ARIS = (u32*)calloc(cap, sizeof(u32));
  if (CDEF_FUNS == NULL || CDEF_ARIS == NULL) {
    sys_error("compiled-def table allocation failed");
  }
  CDEF_CAP = cap;
}
