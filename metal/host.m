// ============================================================================
// HVM4 Metal Host - Objective-C Metal setup and normalize dispatch loop
// ============================================================================
//
// CPU-side orchestration for GPU-based normalization.
// Manages Metal device/queue/pipeline, allocates shared buffers, and runs
// the frontier-based BFS normalize loop (one kernel dispatch per pass).
// ============================================================================

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include "host.h"

// --- Kernel parameter struct (must match hvm4.metal Params) ---
typedef struct {
  uint32_t frontier_count;
} MetalParams;

// Lock array size: 1 bit per heap word, packed into uint32_t
#define LOCK_WORDS ((METAL_HEAP_WORDS + 31) / 32)

// --- Global Metal state ---
static id<MTLDevice>               g_device       = nil;
static id<MTLCommandQueue>         g_queue        = nil;
static id<MTLComputePipelineState> g_pipeline     = nil;
static id<MTLBuffer>               g_heap         = nil;  // ulong[]
static id<MTLBuffer>               g_frontier_a   = nil;  // uint[]
static id<MTLBuffer>               g_frontier_b   = nil;  // uint[]
static id<MTLBuffer>               g_next_count   = nil;  // atomic_uint (single)
static id<MTLBuffer>               g_params       = nil;  // Params struct
static id<MTLBuffer>               g_locks        = nil;  // atomic_uint[] (bitpacked DP locks)
static id<MTLBuffer>               g_itr_count    = nil;  // atomic_uint (interaction counter)
static id<MTLBuffer>               g_alloc_buf    = nil;  // atomic_uint (global alloc cursor)
static uint32_t                    g_alloc_cursor = 0;
static uint32_t                    g_lock_hwm     = 0;    // lock clear high-water mark

// ============================================================================
// Initialization
// ============================================================================

int metal_init(void) {
  @autoreleasepool {
    g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) {
      fprintf(stderr, "metal_init: no Metal device found\n");
      return -1;
    }
    fprintf(stderr, "metal: using device '%s'\n",
            [[g_device name] UTF8String]);

    g_queue = [g_device newCommandQueue];
    if (!g_queue) {
      fprintf(stderr, "metal_init: failed to create command queue\n");
      return -1;
    }

    // --- Load metallib ---
    NSError *error = nil;
    NSString *libPath = nil;
    NSArray *candidates = @[
      @"metal/hvm4.metallib",
      @"hvm4.metallib",
      @"../metal/hvm4.metallib",
    ];
    for (NSString *path in candidates) {
      if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
        libPath = path;
        break;
      }
    }

    id<MTLLibrary> library = nil;
    if (libPath) {
      NSURL *url = [NSURL fileURLWithPath:libPath];
      library = [g_device newLibraryWithURL:url error:&error];
    }
    if (!library) {
      fprintf(stderr, "metal_init: failed to load metallib: %s\n",
              error ? [[error localizedDescription] UTF8String] : "not found");
      return -1;
    }

    id<MTLFunction> kernelFn =
      [library newFunctionWithName:@"normalize_pass"];
    if (!kernelFn) {
      fprintf(stderr, "metal_init: kernel 'normalize_pass' not found\n");
      return -1;
    }

    g_pipeline = [g_device newComputePipelineStateWithFunction:kernelFn
                                                        error:&error];
    if (!g_pipeline) {
      fprintf(stderr, "metal_init: pipeline creation failed: %s\n",
              [[error localizedDescription] UTF8String]);
      return -1;
    }

    // --- Allocate buffers (MTLStorageModeShared for CPU+GPU access) ---
    NSUInteger heapBytes     = (NSUInteger)METAL_HEAP_WORDS * sizeof(uint64_t);
    NSUInteger frontierBytes = (NSUInteger)METAL_MAX_FRONTIER * sizeof(uint32_t);
    NSUInteger countBytes    = sizeof(uint32_t);
    NSUInteger paramsBytes   = sizeof(MetalParams);
    NSUInteger lockBytes     = (NSUInteger)LOCK_WORDS * sizeof(uint32_t);

    g_heap       = [g_device newBufferWithLength:heapBytes
                    options:MTLResourceStorageModeShared];
    g_frontier_a = [g_device newBufferWithLength:frontierBytes
                    options:MTLResourceStorageModeShared];
    g_frontier_b = [g_device newBufferWithLength:frontierBytes
                    options:MTLResourceStorageModeShared];
    g_next_count = [g_device newBufferWithLength:countBytes
                    options:MTLResourceStorageModeShared];
    g_params     = [g_device newBufferWithLength:paramsBytes
                    options:MTLResourceStorageModeShared];
    g_locks      = [g_device newBufferWithLength:lockBytes
                    options:MTLResourceStorageModeShared];
    g_itr_count  = [g_device newBufferWithLength:2 * sizeof(uint32_t)
                    options:MTLResourceStorageModeShared];
    g_alloc_buf  = [g_device newBufferWithLength:sizeof(uint32_t)
                    options:MTLResourceStorageModeShared];

    if (!g_heap || !g_frontier_a || !g_frontier_b ||
        !g_next_count || !g_params || !g_locks || !g_itr_count || !g_alloc_buf) {
      fprintf(stderr, "metal_init: buffer allocation failed\n");
      return -1;
    }

    memset([g_heap contents], 0, heapBytes);
    memset([g_locks contents], 0, lockBytes);
    g_alloc_cursor = 0;

    fprintf(stderr, "metal: initialized (heap=%uMB, locks=%uMB, max_frontier=%u)\n",
            (uint32_t)(heapBytes / (1024*1024)),
            (uint32_t)(lockBytes / (1024*1024)),
            METAL_MAX_FRONTIER);
    return 0;
  }
}

// ============================================================================
// Heap upload
// ============================================================================

void metal_heap_upload(const uint64_t *src, uint32_t count) {
  uint64_t *dst = (uint64_t *)[g_heap contents];
  memcpy(dst, src, (size_t)count * sizeof(uint64_t));
  g_alloc_cursor = count < 1 ? 1 : count;
}

// ============================================================================
// Normalize (BFS frontier loop)
// ============================================================================

uint64_t metal_normalize(uint32_t root_loc) {
  @autoreleasepool {
    // --- Seed frontier with root ---
    uint32_t *fA = (uint32_t *)[g_frontier_a contents];
    fA[0] = root_loc;
    uint32_t frontier_count = 1;

    // --- Clear interaction + bailout counters ---
    uint32_t *itr_ptr = (uint32_t *)[g_itr_count contents];
    itr_ptr[0] = 0;
    itr_ptr[1] = 0;

    id<MTLBuffer> cur_frontier  = g_frontier_a;
    id<MTLBuffer> next_frontier = g_frontier_b;

    // --- Set global alloc cursor ---
    *(uint32_t *)[g_alloc_buf contents] = g_alloc_cursor;

    // --- Clear stale locks from previous normalize call ---
    if (g_lock_hwm > 0) {
      uint32_t hwm_clear = (g_lock_hwm + 31) / 32;
      memset([g_locks contents], 0, (size_t)hwm_clear * sizeof(uint32_t));
    }

    uint32_t pass = 0;

    NSUInteger tg_size = [g_pipeline maxTotalThreadsPerThreadgroup];
    if (tg_size > 256) tg_size = 256;

    while (frontier_count > 0) {
      // --- Check allocation space ---
      uint32_t cur_alloc = *(uint32_t *)[g_alloc_buf contents];
      if (cur_alloc >= METAL_HEAP_WORDS) {
        fprintf(stderr,
                "metal_normalize: out of heap (pass %u, alloc %u/%u)\n",
                pass, cur_alloc, METAL_HEAP_WORDS);
        break;
      }

      // --- Set kernel parameters ---
      MetalParams *p = (MetalParams *)[g_params contents];
      p->frontier_count = frontier_count;

      // --- Reset next_count and clear locks (only up to alloc cursor) ---
      uint32_t *nc = (uint32_t *)[g_next_count contents];
      *nc = 0;
      uint32_t lock_clear_words = (cur_alloc + 31) / 32;
      memset([g_locks contents], 0, (size_t)lock_clear_words * sizeof(uint32_t));

      // --- Encode and dispatch ---
      id<MTLCommandBuffer> cmd = [g_queue commandBuffer];
      id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

      [enc setComputePipelineState:g_pipeline];
      [enc setBuffer:g_heap        offset:0 atIndex:0];
      [enc setBuffer:cur_frontier  offset:0 atIndex:1];
      [enc setBuffer:next_frontier offset:0 atIndex:2];
      [enc setBuffer:g_next_count  offset:0 atIndex:3];
      [enc setBuffer:g_params      offset:0 atIndex:4];
      [enc setBuffer:g_locks       offset:0 atIndex:5];
      [enc setBuffer:g_itr_count   offset:0 atIndex:6];
      [enc setBuffer:g_alloc_buf   offset:0 atIndex:7];

      MTLSize grid = MTLSizeMake(frontier_count, 1, 1);
      MTLSize tg   = MTLSizeMake(
        frontier_count < tg_size ? frontier_count : tg_size, 1, 1);

      [enc dispatchThreads:grid threadsPerThreadgroup:tg];
      [enc endEncoding];
      [cmd commit];
      [cmd waitUntilCompleted];

      if ([cmd error]) {
        fprintf(stderr, "metal_normalize: GPU error pass %u: %s\n",
                pass,
                [[[cmd error] localizedDescription] UTF8String]);
        break;
      }

      // --- Read back next_count ---
      uint32_t next = *nc;

      // --- Swap frontiers ---
      id<MTLBuffer> tmp = cur_frontier;
      cur_frontier  = next_frontier;
      next_frontier = tmp;
      frontier_count = next;
      pass++;

      if (frontier_count > METAL_MAX_FRONTIER) {
        fprintf(stderr,
                "metal_normalize: frontier overflow (%u > %u)\n",
                frontier_count, METAL_MAX_FRONTIER);
        break;
      }
    }

    g_alloc_cursor = *(uint32_t *)[g_alloc_buf contents];
    if (g_alloc_cursor > g_lock_hwm) g_lock_hwm = g_alloc_cursor;
    uint32_t *counters = (uint32_t *)[g_itr_count contents];
    uint64_t total_itrs = counters[0];
    uint32_t total_bailouts = counters[1];
    fprintf(stderr, "metal: normalize done (%u passes, %llu interactions, %u stack bailouts)\n",
            pass, total_itrs, total_bailouts);
    return total_itrs;
  }
}

// ============================================================================
// Heap read-back
// ============================================================================

uint64_t metal_heap_read(uint32_t loc) {
  uint64_t *heap = (uint64_t *)[g_heap contents];
  return heap[loc];
}

uint64_t* metal_heap_ptr(void) {
  return (uint64_t *)[g_heap contents];
}

void metal_set_alloc_cursor(uint32_t cursor) {
  g_alloc_cursor = cursor < 1 ? 1 : cursor;
}

// ============================================================================
// Shutdown
// ============================================================================

void metal_shutdown(void) {
  g_pipeline = nil;
  g_heap = nil;
  g_frontier_a = nil;
  g_frontier_b = nil;
  g_next_count = nil;
  g_params = nil;
  g_locks = nil;
  g_itr_count = nil;
  g_alloc_buf = nil;
  g_queue = nil;
  g_device = nil;
}
