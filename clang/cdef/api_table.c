static const Hvm4DefApi CDEF_API = {
  .def_apply    = cdef_apply,
  .term_new_num = term_new_num,
  .term_new_ref = term_new_ref,
  .term_new_app = term_new_app,
};
