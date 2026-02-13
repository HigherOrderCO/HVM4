// data/elastic_ring.c - Elastic Cyclic Ouroboros Buffer.
//
// Context
// - General-purpose ring buffer with zero-copy wrap-around via double-mapping.
// - Same physical memory is mapped twice contiguously in virtual address space,
//   so reads/writes that cross the buffer boundary are seamless (no split logic).
// - Elastic: grows via ftruncate + remap, shrinks via ftruncate + remap.
//   Data is preserved through the backing fd; growth copies only live data.
//
// Design
// - Linux: memfd_create for anonymous backing fd.
// - macOS/POSIX: shm_open + immediate shm_unlink for anonymous backing fd.
// - Double-map: reserve 2*cap virtual space, MAP_FIXED both halves to same fd.
// - Modular head/tail indices in [0, cap). Separate count for full/empty.
// - Growth: save live data (contiguous via double-map), extend fd, remap, restore.
// - Shrink: same approach in reverse, halving capacity.
//
// Notes
// - Single-threaded (owner only). Concurrency is handled at a higher layer.
// - Capacity is always a power-of-two multiple of page size.
// - ring_push_ptr / ring_pop_ptr return pointers valid for contiguous access
//   up to (cap - count) and count bytes respectively, even across the boundary.

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#if defined(__APPLE__)
#include <fcntl.h>
#endif

// ============================================================
// Platform backend
// ============================================================

#if defined(__linux__)
#define ERING_HAS_MEMFD 1
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#elif defined(__APPLE__) || _POSIX_SHARED_MEMORY_OBJECTS > 0
#define ERING_HAS_SHM 1
#else
#error "elastic_ring requires memfd_create (Linux) or shm_open (macOS/POSIX)"
#endif

// Create an anonymous file descriptor for backing memory.
static int ering_create_fd(void) {
#if defined(ERING_HAS_MEMFD)
  return (int)syscall(SYS_memfd_create, "ering", MFD_CLOEXEC);
#elif defined(ERING_HAS_SHM)
  static _Atomic u32 ering_shm_id = 0;
  char name[64];
  u32 id = atomic_fetch_add_explicit(&ering_shm_id, 1, memory_order_relaxed);
  snprintf(name, sizeof(name), "/ering_%d_%u", getpid(), id);
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) shm_unlink(name);
  return fd;
#endif
}

// Map the same fd at two contiguous virtual regions for seamless wrap-around.
// Returns base address of the 2*cap virtual region, or NULL on failure.
static void *ering_double_map(int fd, size_t cap) {
  // Reserve 2*cap contiguous virtual address space.
  void *base = mmap(NULL, 2 * cap, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) return NULL;

  // Map first half: [base, base+cap) → fd[0, cap).
  void *p1 = mmap(base, cap, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_FIXED, fd, 0);
  if (p1 == MAP_FAILED) {
    munmap(base, 2 * cap);
    return NULL;
  }

  // Map second half: [base+cap, base+2*cap) → fd[0, cap) (same pages).
  void *p2 = mmap((char *)base + cap, cap, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_FIXED, fd, 0);
  if (p2 == MAP_FAILED) {
    munmap(base, 2 * cap);
    return NULL;
  }

  return base;
}

// ============================================================
// ElasticRing type
// ============================================================

typedef struct {
  u8    *data;   // double-mapped virtual region (size = 2 * cap)
  size_t cap;    // physical capacity in bytes (page-aligned, power of 2)
  size_t mask;   // cap - 1
  size_t head;   // write position in [0, cap)
  size_t tail;   // read position in [0, cap)
  size_t count;  // live bytes in buffer
  int    fd;     // backing fd
  size_t pg;     // system page size
} ElasticRing;

// ============================================================
// Helpers
// ============================================================

static size_t ering_page_size(void) {
  long sz = sysconf(_SC_PAGESIZE);
  return (sz > 0) ? (size_t)sz : 4096;
}

// Round up to the next power of two >= pg.
static size_t ering_round_cap(size_t requested, size_t pg) {
  size_t cap = pg;
  while (cap < requested) cap *= 2;
  return cap;
}

fn size_t ring_used(ElasticRing *r)  { return r->count; }
fn size_t ring_avail(ElasticRing *r) { return r->cap - r->count; }
fn size_t ring_capacity(ElasticRing *r) { return r->cap; }

// ============================================================
// Init / Free
// ============================================================

fn bool ring_init(ElasticRing *r, size_t initial_cap) {
  memset(r, 0, sizeof(*r));
  r->pg = ering_page_size();
  size_t cap = ering_round_cap(initial_cap < r->pg ? r->pg : initial_cap, r->pg);

  r->fd = ering_create_fd();
  if (r->fd < 0) return false;

  if (ftruncate(r->fd, (off_t)cap) != 0) {
    close(r->fd);
    r->fd = -1;
    return false;
  }

  r->data = (u8 *)ering_double_map(r->fd, cap);
  if (!r->data) {
    close(r->fd);
    r->fd = -1;
    return false;
  }

  r->cap  = cap;
  r->mask = cap - 1;
  r->head = 0;
  r->tail = 0;
  r->count = 0;
  return true;
}

fn void ring_free(ElasticRing *r) {
  if (r->data) {
    munmap(r->data, 2 * r->cap);
    r->data = NULL;
  }
  if (r->fd >= 0) {
    close(r->fd);
    r->fd = -1;
  }
  r->cap = r->mask = r->head = r->tail = r->count = 0;
}

// ============================================================
// Grow / Shrink
// ============================================================

// Double the buffer capacity. Live data is preserved.
// Returns true on success, false on failure (buffer unchanged).
//
// Zero-copy path: ftruncate extends the backing fd, and existing data at
// fd offsets [0, old_cap) is preserved. After remapping at 2*cap, the live
// data is still at its original offsets. Two cases:
//   - No wrap (tail < head): data at [tail, head) — already contiguous, no copy.
//   - Wrapped (tail >= head, count > 0): data at [tail, old_cap) and [0, head).
//     After remap, move the [0, head) prefix to [old_cap, old_cap+head) via
//     memmove within the mapping. This makes the live range [tail, old_cap+head)
//     contiguous in the new buffer.
fn bool ring_grow(ElasticRing *r) {
  size_t old_cap  = r->cap;
  size_t new_cap  = old_cap * 2;
  size_t old_head = r->head;
  size_t old_tail = r->tail;

  // Extend backing fd. Existing bytes at [0, old_cap) are preserved.
  if (ftruncate(r->fd, (off_t)new_cap) != 0) {
    return false;
  }

  // Tear down old double-mapping and create a larger one.
  munmap(r->data, 2 * old_cap);
  r->data = (u8 *)ering_double_map(r->fd, new_cap);
  if (!r->data) {
    // Attempt rollback.
    ftruncate(r->fd, (off_t)old_cap);
    r->data = (u8 *)ering_double_map(r->fd, old_cap);
    if (!r->data) {
      fprintf(stderr, "elastic_ring: fatal remap failure during growth\n");
      exit(1);
    }
    return false;
  }

  r->cap  = new_cap;
  r->mask = new_cap - 1;

  // Unwrap if data was wrapped around the old boundary.
  bool wrapped = (r->count > 0 && old_tail >= old_head);
  if (wrapped) {
    // Move the [0, old_head) prefix to [old_cap, old_cap + old_head).
    // These fd regions don't overlap, so memcpy is safe.
    memcpy(r->data + old_cap, r->data, old_head);
    r->head = old_cap + old_head;
    // tail stays the same.
  }

  return true;
}

// Halve the buffer if significantly underutilized (count <= cap/4).
// Best-effort: failure leaves the buffer unchanged.
//
// Before truncating the fd, compact live data into [0, count) so it fits
// entirely within the new smaller capacity. This is a single memmove within
// the double-mapped region (contiguous read from data+tail via ouroboros).
fn void ring_shrink(ElasticRing *r) {
  size_t old_cap = r->cap;
  size_t new_cap = old_cap / 2;
  if (r->count > new_cap || new_cap < r->pg) return;

  size_t used = r->count;

  // Compact live data to fd offset 0 so it survives the truncate.
  // memmove handles overlap (src and dst may share pages via double-map).
  if (used > 0 && r->tail != 0) {
    memmove(r->data, r->data + r->tail, used);
  }
  // Now live data is at fd[0, used). Safe to truncate [new_cap, old_cap).

  munmap(r->data, 2 * old_cap);

  if (ftruncate(r->fd, (off_t)new_cap) != 0) {
    // Restore original mapping. Data is already compacted at offset 0.
    r->data = (u8 *)ering_double_map(r->fd, old_cap);
    if (!r->data) {
      fprintf(stderr, "elastic_ring: fatal remap failure during shrink\n");
      exit(1);
    }
    r->tail = 0;
    r->head = used;
    return;
  }

  r->data = (u8 *)ering_double_map(r->fd, new_cap);
  if (!r->data) {
    ftruncate(r->fd, (off_t)old_cap);
    r->data = (u8 *)ering_double_map(r->fd, old_cap);
    if (!r->data) {
      fprintf(stderr, "elastic_ring: fatal remap failure during shrink\n");
      exit(1);
    }
    r->tail = 0;
    r->head = used;
    return;
  }

  r->cap  = new_cap;
  r->mask = new_cap - 1;
  r->tail = 0;
  r->head = used;
}

// ============================================================
// Push / Pop (pointer-based, zero-copy)
// ============================================================

// Get a writable pointer for `len` bytes. Auto-grows if needed.
// The returned pointer is valid for contiguous write of `len` bytes
// even if it crosses the physical buffer boundary (Ouroboros property).
// Returns NULL only on allocation failure.
// Caller MUST call ring_push_commit(r, len) after writing.
fn void *ring_push_ptr(ElasticRing *r, size_t len) {
  while (r->count + len > r->cap) {
    if (!ring_grow(r)) return NULL;
  }
  return r->data + r->head;
}

// Advance head after a successful ring_push_ptr write.
fn void ring_push_commit(ElasticRing *r, size_t len) {
  r->head = (r->head + len) & r->mask;
  r->count += len;
}

// Get a readable pointer for `len` bytes.
// The returned pointer is valid for contiguous read of `len` bytes
// even if it crosses the physical buffer boundary (Ouroboros property).
// Returns NULL if fewer than `len` bytes are available.
// Caller MUST call ring_pop_commit(r, len) after reading.
fn void *ring_pop_ptr(ElasticRing *r, size_t len) {
  if (r->count < len) return NULL;
  return r->data + r->tail;
}

// Advance tail after a successful ring_pop_ptr read.
fn void ring_pop_commit(ElasticRing *r, size_t len) {
  r->tail = (r->tail + len) & r->mask;
  r->count -= len;
}

// ============================================================
// Convenience: u64 element push/pop
// ============================================================

fn bool ring_push_u64(ElasticRing *r, u64 val) {
  void *p = ring_push_ptr(r, sizeof(u64));
  if (!p) return false;
  *(u64 *)p = val;
  ring_push_commit(r, sizeof(u64));
  return true;
}

fn bool ring_pop_u64(ElasticRing *r, u64 *out) {
  void *p = ring_pop_ptr(r, sizeof(u64));
  if (!p) return false;
  *out = *(u64 *)p;
  ring_pop_commit(r, sizeof(u64));
  return true;
}

// ============================================================
// Self-test
// ============================================================

fn int ring_test(void) {
  ElasticRing r;
  size_t pg = ering_page_size();

  // 1. Init
  if (!ring_init(&r, pg)) {
    fprintf(stderr, "ring_test: init failed\n");
    return 1;
  }
  assert(r.cap == pg);
  assert(r.count == 0);

  // 2. Basic push/pop
  u64 v;
  assert(ring_push_u64(&r, 42));
  assert(ring_pop_u64(&r, &v) && v == 42);
  assert(ring_used(&r) == 0);

  // 3. Fill to capacity, drain
  size_t n = r.cap / sizeof(u64);
  for (size_t i = 0; i < n; i++) assert(ring_push_u64(&r, i + 100));
  assert(ring_avail(&r) == 0);
  for (size_t i = 0; i < n; i++) {
    assert(ring_pop_u64(&r, &v));
    assert(v == i + 100);
  }
  assert(ring_used(&r) == 0);

  // 4. Ouroboros wrap-around: advance head/tail past boundary, then
  //    push data that straddles the physical buffer end.
  for (size_t i = 0; i < n * 3 / 4; i++) assert(ring_push_u64(&r, 0xDEAD));
  for (size_t i = 0; i < n * 3 / 4; i++) assert(ring_pop_u64(&r, &v));
  // tail & head are now at 3/4 of cap. Push n/2 — head wraps past cap.
  for (size_t i = 0; i < n / 2; i++) assert(ring_push_u64(&r, i + 5000));
  for (size_t i = 0; i < n / 2; i++) {
    assert(ring_pop_u64(&r, &v));
    assert(v == i + 5000);
  }

  // 5. Contiguous bulk read across wrap boundary.
  for (size_t i = 0; i < n * 3 / 4; i++) assert(ring_push_u64(&r, 0xBEEF));
  for (size_t i = 0; i < n * 3 / 4; i++) assert(ring_pop_u64(&r, &v));
  size_t wn = n / 2;
  for (size_t i = 0; i < wn; i++) assert(ring_push_u64(&r, i + 9000));
  void *bulk = ring_pop_ptr(&r, wn * sizeof(u64));
  assert(bulk != NULL);
  u64 *arr = (u64 *)bulk;
  for (size_t i = 0; i < wn; i++) assert(arr[i] == i + 9000);
  ring_pop_commit(&r, wn * sizeof(u64));

  // 6. Growth preserves data.
  size_t old_cap = r.cap;
  for (size_t i = 0; i < n + 1; i++) assert(ring_push_u64(&r, i + 7000));
  assert(r.cap > old_cap);
  for (size_t i = 0; i < n + 1; i++) {
    assert(ring_pop_u64(&r, &v));
    assert(v == i + 7000);
  }

  // 7. Growth with wrapped data: fill 3/4, pop 1/4, push full cap → forces
  //    growth while live data wraps around the old buffer boundary.
  n = r.cap / sizeof(u64);  // recalc after growth
  for (size_t i = 0; i < n * 3 / 4; i++) assert(ring_push_u64(&r, i + 100000));
  for (size_t i = 0; i < n / 4; i++) {
    assert(ring_pop_u64(&r, &v));
    assert(v == i + 100000);
  }
  // Now used = n/2, head is past 3/4, tail is at 1/4.
  // Push enough to overflow → triggers growth while data wraps.
  size_t to_push = n;  // more than avail → forces growth
  for (size_t i = 0; i < to_push; i++) assert(ring_push_u64(&r, i + 200000));
  // Verify the earlier data.
  for (size_t i = n / 4; i < n * 3 / 4; i++) {
    assert(ring_pop_u64(&r, &v));
    assert(v == i + 100000);
  }
  // Verify the new data.
  for (size_t i = 0; i < to_push; i++) {
    assert(ring_pop_u64(&r, &v));
    assert(v == i + 200000);
  }
  assert(ring_used(&r) == 0);

  // 8. Shrink.
  assert(ring_push_u64(&r, 12345));
  ring_shrink(&r);
  assert(ring_pop_u64(&r, &v) && v == 12345);

  ring_free(&r);
  fprintf(stderr, "[elastic_ring] all tests passed\n");
  return 0;
}
