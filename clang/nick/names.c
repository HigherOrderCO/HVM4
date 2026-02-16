fn u32 table_find(const char *name, u32 len);

// Built-in constructor symbols (initialized at runtime).
static u32 SYM_ZER = 0;
static u32 SYM_SUC = 0;
static u32 SYM_NIL = 0;
static u32 SYM_CON = 0;
static u32 SYM_CHR = 0;

fn void symbols_init(void) {
  SYM_ZER = table_find("ZER", 3);
  SYM_SUC = table_find("SUC", 3);
  SYM_NIL = table_find("NIL", 3);
  SYM_CON = table_find("CON", 3);
  SYM_CHR = table_find("CHR", 3);
}
