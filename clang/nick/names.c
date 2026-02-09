fn u32 table_find(const char *name, u32 len);

// Built-in constructor symbols (initialized at runtime).
static u32 SYM_ZER = 0;
static u32 SYM_SUC = 0;
static u32 SYM_NIL = 0;
static u32 SYM_CON = 0;
static u32 SYM_CHR = 0;
static u32 SYM_U8  = 0;
static u32 SYM_BYT = 0;
static u32 SYM_OK  = 0;
static u32 SYM_ERR = 0;

// Backward-compatible aliases used across the runtime.
#define NAM_ZER SYM_ZER
#define NAM_SUC SYM_SUC
#define NAM_NIL SYM_NIL
#define NAM_CON SYM_CON
#define NAM_CHR SYM_CHR
#define NAM_U8  SYM_U8
#define NAM_BYT SYM_BYT
#define NAM_OK  SYM_OK
#define NAM_ERR SYM_ERR

fn void symbols_init(void) {
  SYM_ZER = table_find("ZER", 3);
  SYM_SUC = table_find("SUC", 3);
  SYM_NIL = table_find("NIL", 3);
  SYM_CON = table_find("CON", 3);
  SYM_CHR = table_find("CHR", 3);
  SYM_U8  = table_find("U8", 2);
  SYM_BYT = table_find("BYT", 3);
  SYM_OK  = table_find("OK", 2);
  SYM_ERR = table_find("ERR", 3);
}
