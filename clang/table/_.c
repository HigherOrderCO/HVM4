// Name table globals
// ==================
// Single global intern table shared by refs/defs/primitives/ctors/labels/names.
// IDs are 24-bit and stored in EXT fields.

typedef struct {
  char **data;
  u32    len;
} NameTable;

static NameTable TABLE = {0};
