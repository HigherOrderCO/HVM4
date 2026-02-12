#ifndef HVM4_METAL_HOST_H
#define HVM4_METAL_HOST_H

#include <stdint.h>

// Default configuration
#define METAL_HEAP_WORDS    (1u << 29)  // 512M words = 4GB
#define METAL_MAX_FRONTIER  (1u << 23)  // 8M entries
#define METAL_MAX_DISPATCH  2048        // thread coarsening cap (M4: 10 cores)

// Initialize the Metal runtime. Returns 0 on success, -1 on failure.
int metal_init(void);

// Copy heap data into the Metal buffer.
// `src` is the host-side heap, `count` is the number of 64-bit words to copy.
void metal_heap_upload(const uint64_t *src, uint32_t count);

// Get direct pointer to the Metal heap buffer (UMA shared memory).
// Allows building terms directly without memcpy.
uint64_t* metal_heap_ptr(void);

// Set the allocation cursor (call after building terms directly in metal_heap_ptr).
void metal_set_alloc_cursor(uint32_t cursor);

// Upload immutable definition-book data used by REF/ALO:
// - `book`: name id -> static term location (BOOK table)
// - `book_heap`: static term heap words indexed by book locations
// Returns 0 on success, -1 on allocation/setup failure.
// Fails if either table exceeds the per-threadgroup local-copy limit.
int metal_book_upload(const uint32_t *book, uint32_t book_count,
                      const uint64_t *book_heap, uint32_t book_heap_words);

// Run frontier-based BFS normalization starting at `root_loc`.
// The heap is modified in-place (shared memory mode).
// Returns total interaction count.
uint64_t metal_normalize(uint32_t root_loc);

// Read the term at `loc` from the Metal heap buffer.
uint64_t metal_heap_read(uint32_t loc);

// Returns non-zero if the last metal_normalize call ended with a runtime error
// (e.g., out-of-heap, frontier overflow, GPU command error).
int metal_last_error(void);

// Shut down Metal runtime and release buffers.
void metal_shutdown(void);

#endif
