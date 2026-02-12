typedef Term (*Hvm4DefFun)(Term *args, u32 argc);

typedef struct {
  Term (*def_apply)(u32 fid, Term *args, u32 argc);
  Term (*term_new_num)(u32 n);
  Term (*term_new_ref)(u32 nam);
  Term (*term_new_app)(Term fun, Term arg);
} Hvm4DefApi;

typedef void (*Hvm4DefRegister)(const Hvm4DefApi *api, Hvm4DefFun *out_fun, u32 *out_ari, u32 out_cap);
