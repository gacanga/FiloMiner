// fm_gpu.h - C API of fmgpu.dll (CUDA KawPow engine of FiloMiner).  Apache-2.0.
// Plain C types only: the DLL is built with nvcc + MSVC, the miner with MinGW g++.
//
// Usage per GPU: fm_gpu_create -> fm_gpu_build_dag (once per epoch) ->
// fm_gpu_set_period (once per 3 blocks; NVRTC-compiled, cached) -> fm_gpu_search.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_GPU_MAX_RESULTS 8

int         fm_gpu_device_count(void);
// returns NULL on failure (see fm_gpu_last_error(NULL)).  block = threads per block
// (multiple of 32, default 256), grid = blocks per launch (0 = automatic).
void*       fm_gpu_create(int device, int block, int grid);
void        fm_gpu_destroy(void* ctx);
const char* fm_gpu_name(void* ctx);
const char* fm_gpu_last_error(void* ctx);
int         fm_gpu_compute_capability(void* ctx);     // e.g. 61
int         fm_gpu_sm_count(void* ctx);
uint64_t    fm_gpu_memory(void* ctx);                 // total bytes
uint32_t    fm_gpu_batch(void* ctx);                  // hashes per launch (grid * block)
int         fm_gpu_set_grid(void* ctx, int grid);

// DAG of an epoch from its light cache (light_items x 64 bytes); full_items =
// number of 1024-bit dataset items.  Does nothing if that epoch is already built.
int         fm_gpu_build_dag(void* ctx, int epoch, const uint32_t* light_cache, uint32_t light_items,
                             uint32_t full_items, double* seconds);
// copies `count` 32-bit words of the DAG starting at word `offset` (tests)
int         fm_gpu_read_dag(void* ctx, uint64_t offset, uint32_t count, uint32_t* out);

// Program of a ProgPoW period (height / 3), for the current DAG.  *compiled = 1 when
// NVRTC ran (0 = taken from the cache), *seconds = compile time.
int         fm_gpu_set_period(void* ctx, uint64_t period, int* compiled, double* seconds);
// Compiles a period ahead of time (any thread; no GPU needed).  0 = ok.
int         fm_gpu_precompile(uint64_t period, uint32_t dag_items_2048, int cc, int block);
// Registers and spills of the current search kernel
int         fm_gpu_kernel_info(void* ctx, int* regs, int* local_bytes);

// One launch of fm_gpu_batch() hashes on nonces start_nonce + i.  header = 32
// bytes (8 LE words).  Candidates (top 64 bits of the final hash <= target64) are
// returned in found[]; *n_found may exceed max_found (extras dropped).
int         fm_gpu_search(void* ctx, uint64_t start_nonce, const uint32_t* header, uint64_t target64,
                          uint64_t* found, int max_found, int* n_found);
// Test helper: debug kernel, `count` nonces from start_nonce; out = count x 16 words
// (8 words mix hash, 8 words final hash).
int         fm_gpu_debug_hashes(void* ctx, uint64_t start_nonce, const uint32_t* header, uint32_t count, uint32_t* out);

#ifdef __cplusplus
}
#endif
