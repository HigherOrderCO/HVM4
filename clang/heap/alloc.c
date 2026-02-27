fn void aot_heap_alloc_note(u64 size);
fn void aot_heap_alloc_note_kind(u32 kind, u64 size);
fn u64 heap_alloc(u64 size);

// Heap allocation kind tags used for compiled-context attribution.
#define AOT_HEAP_KIND_NONE         0
#define AOT_HEAP_KIND_TERM_APP     1
#define AOT_HEAP_KIND_TERM_OP2     2
#define AOT_HEAP_KIND_TERM_CTR     3
#define AOT_HEAP_KIND_TERM_DUP     4
#define AOT_HEAP_KIND_TERM_SUP     5
#define AOT_HEAP_KIND_TERM_MAT     6
#define AOT_HEAP_KIND_TERM_SWI     7
#define AOT_HEAP_KIND_TERM_DRY     8
#define AOT_HEAP_KIND_TERM_DSU     9
#define AOT_HEAP_KIND_TERM_DDU     10
#define AOT_HEAP_KIND_TERM_ALO     11
#define AOT_HEAP_KIND_TERM_LAM     12
#define AOT_HEAP_KIND_TERM_USE     13
#define AOT_HEAP_KIND_TERM_INC     14
#define AOT_HEAP_KIND_TERM_UNS     15
#define AOT_HEAP_KIND_TERM_EQL     16
#define AOT_HEAP_KIND_TERM_AND     17
#define AOT_HEAP_KIND_TERM_OR      18
#define AOT_HEAP_KIND_TERM_PRI     19
#define AOT_HEAP_KIND_TERM_GENERIC 20
#define AOT_HEAP_KIND_TERM_CLONE   21
#define AOT_HEAP_KIND_AOT_APP_HEAD 22
#define AOT_HEAP_KIND_AOT_APP_MAT  23
#define AOT_HEAP_KIND_AOT_DUP_CELL 24
#define AOT_HEAP_KIND_AOT_CTR      25
#define AOT_HEAP_KIND_AOT_ENV_BIND 26
#define AOT_HEAP_KIND_COUNT        27

static _Thread_local u32 HEAP_ALLOC_KIND_HINT = AOT_HEAP_KIND_NONE;

// Reads current heap-allocation kind hint for the calling thread.
fn u32 heap_alloc_kind_hint_get(void) {
  return HEAP_ALLOC_KIND_HINT;
}

// Sets current heap-allocation kind hint for the calling thread.
fn void heap_alloc_kind_hint_set(u32 kind) {
  HEAP_ALLOC_KIND_HINT = kind;
}

// Allocates one heap chunk with a temporary allocation kind hint.
fn u64 heap_alloc_kind(u64 size, u32 kind) {
  u32 prev = heap_alloc_kind_hint_get();
  heap_alloc_kind_hint_set(kind);
  u64 loc = heap_alloc(size);
  heap_alloc_kind_hint_set(prev);
  return loc;
}

fn u64 heap_alloc(u64 size) {
  u32 tid  = WNF_TID;
  u64 idx  = (u64)tid * HEAP_STRIDE;
  u64 at   = HEAP_NEXT[idx];
  u64 next = at + size;
  if (__builtin_expect(next <= HEAP_END[idx] && next >= at, 1)) {
    HEAP_NEXT[idx] = next;
    u32 kind = heap_alloc_kind_hint_get();
    if (kind >= AOT_HEAP_KIND_COUNT) {
      kind = AOT_HEAP_KIND_NONE;
    }
    aot_heap_alloc_note(size);
    aot_heap_alloc_note_kind(kind, size);
    return at;
  }
  fprintf(stderr,
          "Out of heap memory in thread bank %u (need %llu words)\n",
          tid, (unsigned long long)size);
  exit(1);
}
