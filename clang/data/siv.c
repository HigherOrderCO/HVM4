// data/siv.c - Stable Index Vector for dense, cancel-friendly storage.
//
// Context
// - Companion to ring/deque-based work queues: ring holds ordering (u32 IDs),
//   SIV holds the actual data (u64 values) in a dense, swap-compacted array.
// - IDs are stable: an ID remains valid until explicitly erased, regardless
//   of other insertions or deletions.
//
// Design
// - Dense data[] array: live values packed at indices [0, count).
// - Two maps: id_to_slot (ID -> position in data[]) and slot_to_id (inverse).
// - Push: append at data[count], assign monotonic next_id, update both maps.
// - Erase: swap data[slot] with data[count-1], update maps, decrement count.
// - Both are O(1). Iteration over live entries is a cache-friendly linear scan.
//
// Notes
// - Single-threaded (owner only). For work-stealing, the owner pushes IDs into
//   a concurrent ring; thieves read SIV data via the ID after stealing.
//   Thread-safety contract for SIV+Ring integration:
//     * Owner: siv_push (then publish ID to ring), siv_erase (after thief is done).
//     * Thief: siv_valid + siv_get (after stealing ID from ring).
//     * Requires: release barrier after siv_push (before ring push), acquire
//       barrier in thief (after ring steal, before siv_get).
//     * Race: if owner calls siv_erase while thief is in siv_get for the same ID,
//       the thief may read a stale/swapped value. The thief must copy the value
//       before the owner can erase, or the cancel protocol must ensure the owner
//       only erases IDs that no thief is currently reading.
// - ID space is monotonic u32: wraps to 0 after 4 billion pushes total (not live).
//   For HVM4 work queues this is unreachable in practice. If needed, a reset or
//   generation counter can be added to the protocol.
// - Separate capacity tracking for data[] (count-bound) and id_to_slot[] (id-bound).
// - Erase of an already-erased ID is a safe no-op.

#include <stdlib.h>
#include <string.h>

#define SIV_INVALID 0xFFFFFFFFu

typedef struct {
  u64 *data;         // dense array of live values [0, count)
  u32 *id_to_slot;   // id -> slot in data[] (SIV_INVALID if erased/unassigned)
  u32 *slot_to_id;   // slot -> id (inverse map for swap-on-erase)
  u32  count;        // number of live entries
  u32  cap;          // capacity of data[] and slot_to_id[]
  u32  next_id;      // monotonic ID counter (total pushes)
  u32  id_cap;       // capacity of id_to_slot[]
} Siv;

// Initialize with given capacity (for both data and ID space).
fn bool siv_init(Siv *s, u32 initial_cap) {
  if (initial_cap == 0) initial_cap = 64;
  s->data       = (u64 *)malloc((size_t)initial_cap * sizeof(u64));
  s->slot_to_id = (u32 *)malloc((size_t)initial_cap * sizeof(u32));
  s->id_to_slot = (u32 *)malloc((size_t)initial_cap * sizeof(u32));
  if (!s->data || !s->slot_to_id || !s->id_to_slot) {
    free(s->data); free(s->slot_to_id); free(s->id_to_slot);
    memset(s, 0, sizeof(*s));
    return false;
  }
  memset(s->id_to_slot, 0xFF, (size_t)initial_cap * sizeof(u32)); // all SIV_INVALID
  s->count   = 0;
  s->cap     = initial_cap;
  s->next_id = 0;
  s->id_cap  = initial_cap;
  return true;
}

fn void siv_free(Siv *s) {
  free(s->data);
  free(s->slot_to_id);
  free(s->id_to_slot);
  memset(s, 0, sizeof(*s));
}

// Grow data[] and slot_to_id[] to 2x capacity.
static inline bool siv_grow_data(Siv *s) {
  u32 new_cap = s->cap * 2;
  u64 *nd = (u64 *)realloc(s->data, (size_t)new_cap * sizeof(u64));
  u32 *ns = (u32 *)realloc(s->slot_to_id, (size_t)new_cap * sizeof(u32));
  if (!nd || !ns) {
    if (nd) s->data = nd;       // partial realloc ok, keep the successful one
    if (ns) s->slot_to_id = ns;
    return false;
  }
  s->data       = nd;
  s->slot_to_id = ns;
  s->cap        = new_cap;
  return true;
}

// Grow id_to_slot[] to accommodate next_id.
static inline bool siv_grow_ids(Siv *s) {
  u32 new_id_cap = s->id_cap * 2;
  u32 *ni = (u32 *)realloc(s->id_to_slot, (size_t)new_id_cap * sizeof(u32));
  if (!ni) return false;
  // Initialize new slots to SIV_INVALID.
  memset(ni + s->id_cap, 0xFF, (size_t)(new_id_cap - s->id_cap) * sizeof(u32));
  s->id_to_slot = ni;
  s->id_cap     = new_id_cap;
  return true;
}

// Insert a value. Returns the stable ID, or SIV_INVALID on allocation failure.
fn u32 siv_push(Siv *s, u64 val) {
  // Ensure data capacity.
  if (s->count >= s->cap) {
    if (!siv_grow_data(s)) return SIV_INVALID;
  }
  // Ensure ID capacity.
  if (s->next_id >= s->id_cap) {
    if (!siv_grow_ids(s)) return SIV_INVALID;
  }

  u32 id   = s->next_id++;
  u32 slot = s->count++;

  s->data[slot]       = val;
  s->slot_to_id[slot] = id;
  s->id_to_slot[id]   = slot;
  return id;
}

// Check if an ID is still live.
fn bool siv_valid(Siv *s, u32 id) {
  return id < s->next_id && id < s->id_cap && s->id_to_slot[id] != SIV_INVALID;
}

// Retrieve value by ID. Caller must check siv_valid() first.
fn u64 siv_get(Siv *s, u32 id) {
  return s->data[s->id_to_slot[id]];
}

// Erase by ID. Swap-deletes from dense array. Safe no-op if already erased.
fn void siv_erase(Siv *s, u32 id) {
  if (id >= s->id_cap || s->id_to_slot[id] == SIV_INVALID) return;

  u32 slot = s->id_to_slot[id];
  u32 last = s->count - 1;

  if (slot != last) {
    // Move last element into the erased slot.
    s->data[slot]       = s->data[last];
    u32 moved_id        = s->slot_to_id[last];
    s->slot_to_id[slot] = moved_id;
    s->id_to_slot[moved_id] = slot;
  }

  s->id_to_slot[id] = SIV_INVALID;
  s->count--;
}

// Live entry count.
fn u32 siv_count(Siv *s) { return s->count; }

// Dense data pointer for iteration: for (u32 i = 0; i < siv_count(s); i++) s->data[i]
fn u64 *siv_data(Siv *s) { return s->data; }

// Get the ID for a given dense slot (for iteration with ID tracking).
fn u32 siv_slot_id(Siv *s, u32 slot) { return s->slot_to_id[slot]; }

// ============================================================
// Self-test
// ============================================================

fn int siv_test(void) {
  Siv s;

  // 1. Init
  if (!siv_init(&s, 4)) {
    fprintf(stderr, "siv_test: init failed\n");
    return 1;
  }
  assert(siv_count(&s) == 0);

  // 2. Push and retrieve
  u32 id0 = siv_push(&s, 100);
  u32 id1 = siv_push(&s, 200);
  u32 id2 = siv_push(&s, 300);
  assert(id0 != SIV_INVALID && id1 != SIV_INVALID && id2 != SIV_INVALID);
  assert(siv_count(&s) == 3);
  assert(siv_get(&s, id0) == 100);
  assert(siv_get(&s, id1) == 200);
  assert(siv_get(&s, id2) == 300);

  // 3. Erase middle — swap compaction
  siv_erase(&s, id1);
  assert(siv_count(&s) == 2);
  assert(!siv_valid(&s, id1));
  assert(siv_valid(&s, id0));
  assert(siv_valid(&s, id2));
  assert(siv_get(&s, id0) == 100);
  assert(siv_get(&s, id2) == 300);

  // 4. Double erase is no-op
  siv_erase(&s, id1);
  assert(siv_count(&s) == 2);

  // 5. Dense iteration sees exactly the live values (order may differ after swap)
  u64 sum = 0;
  for (u32 i = 0; i < siv_count(&s); i++) sum += siv_data(&s)[i];
  assert(sum == 400); // 100 + 300

  // 6. Push after erase reuses dense slot
  u32 id3 = siv_push(&s, 400);
  assert(siv_count(&s) == 3);
  assert(siv_get(&s, id3) == 400);

  // 7. Growth: push beyond initial capacity (was 4)
  u32 id4 = siv_push(&s, 500);
  u32 id5 = siv_push(&s, 600);
  assert(siv_count(&s) == 5);
  assert(siv_get(&s, id4) == 500);
  assert(siv_get(&s, id5) == 600);
  // All earlier IDs still valid
  assert(siv_get(&s, id0) == 100);
  assert(siv_get(&s, id2) == 300);
  assert(siv_get(&s, id3) == 400);

  // 8. ID space growth: push many, erase many, push more
  //    This exercises id_cap growth independently of data cap growth.
  for (u32 i = 0; i < 200; i++) {
    u32 id = siv_push(&s, 1000 + i);
    assert(id != SIV_INVALID);
  }
  assert(siv_count(&s) == 205); // 5 + 200
  // Erase all but last 10
  for (u32 i = 0; i < siv_count(&s) - 10; ) {
    u32 eid = siv_slot_id(&s, i);
    siv_erase(&s, eid);
    // After erase, slot i has the swapped-in element — don't increment
    if (siv_count(&s) <= 10) break;
  }
  assert(siv_count(&s) == 10);
  // Push more — these get new IDs past the old high-water mark
  for (u32 i = 0; i < 50; i++) {
    u32 id = siv_push(&s, 9000 + i);
    assert(id != SIV_INVALID);
  }
  assert(siv_count(&s) == 60);

  // 9. Erase all
  while (siv_count(&s) > 0) {
    siv_erase(&s, siv_slot_id(&s, 0));
  }
  assert(siv_count(&s) == 0);

  // 10. Reuse after full drain
  u32 id_after = siv_push(&s, 42);
  assert(id_after != SIV_INVALID);
  assert(siv_count(&s) == 1);
  assert(siv_get(&s, id_after) == 42);
  siv_erase(&s, id_after);

  // 11. siv_valid rejects garbage IDs
  assert(!siv_valid(&s, SIV_INVALID));     // sentinel value
  assert(!siv_valid(&s, 0xFFFFFFFE));      // near-max u32
  assert(!siv_valid(&s, 999999));          // beyond next_id
  assert(!siv_valid(&s, 0));               // was valid, now erased

  // 12. Independent capacity growth: data cap vs id cap.
  //     Push many, erase all, push many again. id_cap grows with total pushes
  //     while data cap stays small (because count is always low).
  for (u32 round = 0; round < 3; round++) {
    for (u32 i = 0; i < 100; i++) {
      u32 id = siv_push(&s, 8000 + round * 100 + i);
      assert(id != SIV_INVALID);
    }
    // Erase all — data cap doesn't need to grow, but id_cap keeps rising
    while (siv_count(&s) > 0) {
      siv_erase(&s, siv_slot_id(&s, 0));
    }
  }
  // After 3 rounds of 100 push+erase, next_id is 300+ but count is 0.
  // id_cap must have grown, data cap may not have.
  assert(siv_count(&s) == 0);
  assert(s.id_cap >= 300);
  // Push one more to verify everything still works
  u32 id_final = siv_push(&s, 77777);
  assert(id_final != SIV_INVALID);
  assert(siv_get(&s, id_final) == 77777);

  // 13. Erase last element (slot == last, no swap needed)
  u32 id_only = siv_push(&s, 88888);
  // Now count=2. Erase id_only (it's at the last slot).
  siv_erase(&s, id_only);
  assert(siv_count(&s) == 1);
  assert(siv_get(&s, id_final) == 77777); // earlier push still valid

  siv_free(&s);
  fprintf(stderr, "[siv] all tests passed\n");
  return 0;
}
