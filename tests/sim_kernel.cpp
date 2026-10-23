// sim_kernel.cpp - runs gpu/kawpow_kernel.cuh on the CPU (16 threads = one lane
// group, shuffles through a barrier) and compares it with the ethash reference.
// Build: g++ -O2 -std=c++20 -DFM_SIM -DGEN_HEADER='"gen.h"' ... (see tools/sim_test.sh)
// gen.h (tools/gen_program) defines FM_DAG_ITEMS, FM_PROGRAM_OPS, FM_PROGRAM_DAG and
// SIM_HEIGHT / SIM_EPOCH for one test case.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <barrier>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include "../src/kawpow_ref.h"
#include "../gpu/fm_dag.h"

#define FM_SIM 1
struct uint4 { uint32_t x, y, z, w; };
struct Dim { uint32_t x; };
static thread_local Dim threadIdx;
static Dim blockIdx;
static std::barrier<>* g_bar;
static uint32_t g_slot[16];

static inline uint32_t sim_shfl(uint32_t v, uint32_t src)
{
    g_slot[threadIdx.x] = v;
    g_bar->arrive_and_wait();
    uint32_t r = g_slot[src & 15];
    g_bar->arrive_and_wait();
    return r;
}
static inline uint32_t rotl32(uint32_t a, uint32_t b) { b &= 31; return b ? (a << b) | (a >> (32 - b)) : a; }
static inline uint32_t rotr32(uint32_t a, uint32_t b) { b &= 31; return b ? (a >> b) | (a << (32 - b)) : a; }
static inline uint32_t bswap_(uint32_t a) { return __builtin_bswap32(a); }
static inline uint32_t clz_(uint32_t a) { return a ? (uint32_t)__builtin_clz(a) : 32; }
static std::mutex g_atomic;
static uint32_t atomicAdd(uint32_t* p, uint32_t v) { std::lock_guard<std::mutex> lk(g_atomic); uint32_t o = *p; *p += v; return o; }

// DAG computed on demand from the light cache (fm_dag.h, checked separately)
struct LazyDag {
    const kref::Epoch* e;
    mutable std::mutex m;
    mutable std::unordered_map<uint32_t, std::vector<uint32_t>> items;
    uint4 operator[](uint32_t idx) const   // idx in uint4 units
    {
        const uint32_t word = idx * 4, it = word / 16, off = word % 16;
        std::lock_guard<std::mutex> lk(m);
        auto f = items.find(it);
        if (f == items.end()) {
            std::vector<uint32_t> v(16);
            fmdag::item512(e->light_cache, e->light_items, it, v.data());
            f = items.emplace(it, std::move(v)).first;
        }
        const uint32_t* p = f->second.data() + off;
        return uint4{ p[0], p[1], p[2], p[3] };
    }
};

#define __global__
#define __device__
#define __forceinline__ inline
#define __launch_bounds__(x)
#define __shared__ static
#define __syncthreads() g_bar->arrive_and_wait()
#define FM_ROTL(a, b) rotl32((a), (b))
#define FM_ROTR(a, b) rotr32((a), (b))
#define FM_MULHI(a, b) ((uint32_t)(((uint64_t)(a) * (uint64_t)(b)) >> 32))
#define FM_MIN(a, b) std::min<uint32_t>((a), (b))
#define FM_CLZ(a) clz_(a)
#define FM_POPC(a) ((uint32_t)__builtin_popcount(a))
#define FM_BSWAP(a) bswap_(a)
#define FM_DAG_PTR const LazyDag&
#define FM_SHFL(v, src) sim_shfl((v), (src))
#define FM_BLOCK 16
#define FM_MAX_RESULTS 4
#define FM_DEBUG 1
#include GEN_HEADER
#include "../gpu/kawpow_kernel.cuh"

static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static void unhex(const char* s, uint8_t* o, int n) { for (int i = 0; i < n; i++) o[i] = (uint8_t)(hexv(s[2 * i]) * 16 + hexv(s[2 * i + 1])); }
static std::string hex(const uint8_t* p, int n) { std::string s; char b[3]; for (int i = 0; i < n; i++) { snprintf(b, 3, "%02x", p[i]); s += b; } return s; }

int main(int argc, char** argv)
{
    // argv: header_hex nonce_hex [expected_mix expected_final]
    if (argc < 3) { fprintf(stderr, "usage: sim header nonce [mix final]\n"); return 2; }
    auto e = kref::get_epoch(SIM_EPOCH);
    if ((uint32_t)FM_DAG_ITEMS != e->dag_items_2048()) { printf("DAG item count mismatch %u vs %u\n", (uint32_t)FM_DAG_ITEMS, e->dag_items_2048()); return 1; }
    uint8_t hdr[32];
    unhex(argv[1], hdr, 32);
    const uint64_t nonce0 = strtoull(argv[2], nullptr, 16);
    LazyDag dag;
    dag.e = e.get();
    fm_header_t H;
    memcpy(H.w, hdr, 32);
    fm_results_t res;
    memset(&res, 0, sizeof res);
    std::vector<uint32_t> dbg(16 * 16);
    // the test nonce is thread 5 of the group; target: everything (all 16 candidates, only 4 kept)
    const uint64_t start = nonce0 - 5;
    std::barrier<> bar(16);
    g_bar = &bar;
    blockIdx.x = 0;
    std::vector<std::thread> th;
    for (uint32_t t = 0; t < 16; t++)
        th.emplace_back([&, t]() { threadIdx.x = t; fm_search(start, H, ~0ull, dag, &res, dbg.data()); });
    for (auto& x : th) x.join();
    int bad = 0;
    for (uint32_t t = 0; t < 16; t++) {
        uint8_t mix[32], fin[32];
        kref::hash(*e, SIM_HEIGHT, hdr, start + t, mix, fin);
        if (memcmp(mix, &dbg[t * 16], 32) || memcmp(fin, &dbg[t * 16 + 8], 32)) {
            bad++;
            printf("  thread %u nonce %016llx: kernel mix %s final %s\n  reference mix %s final %s\n", t, (unsigned long long)(start + t),
                   hex((uint8_t*)&dbg[t * 16], 32).c_str(), hex((uint8_t*)&dbg[t * 16 + 8], 32).c_str(), hex(mix, 32).c_str(), hex(fin, 32).c_str());
        }
        if (t == 5 && argc >= 5) {
            if (hex(mix, 32) != argv[3] || hex(fin, 32) != argv[4]) { printf("  reference does not match the test vector!\n"); bad++; }
        }
    }
    printf("SIM height %d epoch %d nonce %016llx: %s (%u candidates)\n", SIM_HEIGHT, SIM_EPOCH, (unsigned long long)nonce0, bad ? "FAILED" : "OK", res.count);
    return bad ? 1 : 0;
}
