// fm_dag.h - ethash DAG item computation (Keccak-f[1600] + 512 FNV parents, KawPow) shared
// by the CUDA DAG builder (nvcc) and the CPU tests (g++).  Apache-2.0.
// Follows ethash/lib/ethash/ethash.cpp (item_state, calculate_dataset_item_512).
#pragma once
#include <stdint.h>

#ifdef __CUDACC__
#define FM_HD __host__ __device__ __forceinline__
#else
#define FM_HD inline
#endif

namespace fmdag {

static const uint32_t FNV_PRIME = 0x01000193u;
static const uint32_t PARENTS = 512;   // KawPow: 512 (ethash: 256)

FM_HD uint64_t rotl64(uint64_t x, int s) { return (x << s) | (x >> (64 - s)); }

FM_HD void keccak_f1600(uint64_t* st)
{
    const uint64_t rc[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808Aull, 0x8000000080008000ull,
        0x000000000000808Bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
        0x000000000000008Aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000Aull,
        0x000000008000808Bull, 0x800000000000008Bull, 0x8000000000008089ull, 0x8000000000008003ull,
        0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800Aull, 0x800000008000000Aull,
        0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull };
    const int rotc[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
    const int piln[24] = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };
#ifdef __CUDACC__
#pragma unroll
#endif
    for (int r = 0; r < 24; r++) {
        uint64_t bc[5];
        for (int i = 0; i < 5; i++) bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; i++) {
            const uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) st[j + i] ^= t;
        }
        uint64_t t = st[1];
        for (int i = 0; i < 24; i++) {
            const int j = piln[i];
            const uint64_t b = st[j];
            st[j] = rotl64(t, rotc[i]);
            t = b;
        }
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; i++) bc[i] = st[j + i];
            for (int i = 0; i < 5; i++) st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }
        st[0] ^= rc[r];
    }
}

// keccak-512 of exactly 64 bytes (8 little-endian words), in place
FM_HD void keccak512_64(uint32_t* w16)
{
    uint64_t st[25];
    for (int i = 0; i < 8; i++) st[i] = (uint64_t)w16[2 * i] | ((uint64_t)w16[2 * i + 1] << 32);
    for (int i = 8; i < 25; i++) st[i] = 0;
    st[8] = 0x8000000000000001ull;   // padding 0x01 ... 0x80 (rate 72 bytes)
    keccak_f1600(st);
    for (int i = 0; i < 8; i++) { w16[2 * i] = (uint32_t)st[i]; w16[2 * i + 1] = (uint32_t)(st[i] >> 32); }
}

// One 512-bit DAG item (16 words) from the light cache (light_items x 16 words)
FM_HD void item512(const uint32_t* cache, uint32_t light_items, uint32_t index, uint32_t* out)
{
    uint32_t mix[16];
    const uint32_t* c = cache + (uint64_t)(index % light_items) * 16;
    for (int i = 0; i < 16; i++) mix[i] = c[i];
    mix[0] ^= index;
    keccak512_64(mix);
    for (uint32_t j = 0; j < PARENTS; j++) {
        const uint32_t t = ((index ^ j) * FNV_PRIME) ^ mix[j % 16];
        const uint32_t* p = cache + (uint64_t)(t % light_items) * 16;
        for (int k = 0; k < 16; k++) mix[k] = (mix[k] * FNV_PRIME) ^ p[k];
    }
    keccak512_64(mix);
    for (int i = 0; i < 16; i++) out[i] = mix[i];
}

}  // namespace fmdag
