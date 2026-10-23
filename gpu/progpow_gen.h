// progpow_gen.h - KawPow (ProgPoW 0.9.4, Ravencoin/FiloPow variant) random program
// generator of FiloMiner.  Apache-2.0.
//
// ProgPoW changes its inner loop every PERIOD (3) blocks.  The loop body is fully
// determined by the period number: a KISS99 generator seeded from it draws the
// register permutations, the 11 cache reads, the 18 random math operations and the
// 4 DAG merges, in exactly the order of the reference implementation
// (ethash/lib/ethash/progpow.cpp, functions mix_rng_state and round()).
//
// emit_macros() turns one period's program into two C macros:
//   FM_PROGRAM_OPS   cache reads + random math, operating on uint32_t mix[32]
//                    (reads the 16 KB L1 cache through FM_CACHE(index))
//   FM_PROGRAM_DAG   merges the 4 DAG words dag_w0..dag_w3 into mix
// The same text is compiled by NVRTC for the GPU and by g++ for the CPU simulator.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <cstdio>

namespace fmgen {

static const int PERIOD = 3;
static const int REGS = 32;
static const int LANES = 16;
static const int CACHE_ACCESSES = 11;
static const int MATH_OPERATIONS = 18;
static const int DAG_WORDS = 4;              // 32-bit words per lane per DAG access
static const uint32_t FNV_PRIME = 0x01000193u;
static const uint32_t FNV_BASIS = 0x811c9dc5u;

inline uint32_t fnv1a(uint32_t u, uint32_t v) { return (u ^ v) * FNV_PRIME; }

struct Kiss99 {
    uint32_t z, w, jsr, jcong;
    uint32_t operator()()
    {
        z = 36969 * (z & 0xffff) + (z >> 16);
        w = 18000 * (w & 0xffff) + (w >> 16);
        jcong = 69069 * jcong + 1234567;
        jsr ^= (jsr << 17);
        jsr ^= (jsr >> 13);
        jsr ^= (jsr << 5);
        return (((z << 16) + w) ^ jcong) + jsr;
    }
};

struct Op {
    bool cache;          // true: cache read, false: random math
    int src1, src2, dst; // cache read uses src1 only
    uint32_t sel1;       // math selector (math only)
    uint32_t sel2;       // merge selector
};

struct Program {
    uint64_t period = 0;
    std::vector<Op> ops;          // in reference order
    int dag_dst[DAG_WORDS];
    uint32_t dag_sel[DAG_WORDS];
};

inline Program make_program(uint64_t period)
{
    Program p;
    p.period = period;
    const uint32_t seed_lo = (uint32_t)period, seed_hi = (uint32_t)(period >> 32);
    Kiss99 rng;
    rng.z = fnv1a(FNV_BASIS, seed_lo);
    rng.w = fnv1a(rng.z, seed_hi);
    rng.jsr = fnv1a(rng.w, seed_lo);
    rng.jcong = fnv1a(rng.jsr, seed_hi);
    int dst_seq[REGS], src_seq[REGS];
    for (int i = 0; i < REGS; i++) dst_seq[i] = src_seq[i] = i;
    for (uint32_t i = REGS; i > 1; --i) {
        uint32_t a = rng() % i;
        int t = dst_seq[i - 1]; dst_seq[i - 1] = dst_seq[a]; dst_seq[a] = t;
        uint32_t b = rng() % i;
        t = src_seq[i - 1]; src_seq[i - 1] = src_seq[b]; src_seq[b] = t;
    }
    int dst_cnt = 0, src_cnt = 0;
    auto next_dst = [&]() { return dst_seq[(dst_cnt++) % REGS]; };
    auto next_src = [&]() { return src_seq[(src_cnt++) % REGS]; };
    const int max_ops = CACHE_ACCESSES > MATH_OPERATIONS ? CACHE_ACCESSES : MATH_OPERATIONS;
    for (int i = 0; i < max_ops; i++) {
        if (i < CACHE_ACCESSES) {
            Op o;
            o.cache = true;
            o.src1 = next_src();
            o.dst = next_dst();
            o.sel2 = rng();
            o.src2 = -1;
            o.sel1 = 0;
            p.ops.push_back(o);
        }
        if (i < MATH_OPERATIONS) {
            Op o;
            o.cache = false;
            uint32_t src_rnd = rng() % (REGS * (REGS - 1));
            o.src1 = (int)(src_rnd % REGS);
            o.src2 = (int)(src_rnd / REGS);
            if (o.src2 >= o.src1) ++o.src2;
            o.sel1 = rng();
            o.dst = next_dst();
            o.sel2 = rng();
            p.ops.push_back(o);
        }
    }
    for (int i = 0; i < DAG_WORDS; i++) {
        p.dag_dst[i] = i == 0 ? 0 : next_dst();
        p.dag_sel[i] = rng();
    }
    return p;
}

// "mix[d] = merge(mix[d], b)" for the merge selector sel
inline std::string emit_merge(int d, const std::string& b, uint32_t sel)
{
    char buf[256];
    const uint32_t x = ((sel >> 16) % 31) + 1;
    switch (sel % 4) {
    case 0: snprintf(buf, sizeof buf, "mix[%d] = (mix[%d] * 33u) + (%s);", d, d, b.c_str()); break;
    case 1: snprintf(buf, sizeof buf, "mix[%d] = (mix[%d] ^ (%s)) * 33u;", d, d, b.c_str()); break;
    case 2: snprintf(buf, sizeof buf, "mix[%d] = FM_ROTL(mix[%d], %uu) ^ (%s);", d, d, x, b.c_str()); break;
    default: snprintf(buf, sizeof buf, "mix[%d] = FM_ROTR(mix[%d], %uu) ^ (%s);", d, d, x, b.c_str()); break;
    }
    return buf;
}

inline std::string emit_math(int s1, int s2, uint32_t sel)
{
    char a[16], b[16], buf[128];
    snprintf(a, sizeof a, "mix[%d]", s1);
    snprintf(b, sizeof b, "mix[%d]", s2);
    switch (sel % 11) {
    case 0: snprintf(buf, sizeof buf, "%s + %s", a, b); break;
    case 1: snprintf(buf, sizeof buf, "%s * %s", a, b); break;
    case 2: snprintf(buf, sizeof buf, "FM_MULHI(%s, %s)", a, b); break;
    case 3: snprintf(buf, sizeof buf, "FM_MIN(%s, %s)", a, b); break;
    case 4: snprintf(buf, sizeof buf, "FM_ROTL(%s, %s)", a, b); break;
    case 5: snprintf(buf, sizeof buf, "FM_ROTR(%s, %s)", a, b); break;
    case 6: snprintf(buf, sizeof buf, "%s & %s", a, b); break;
    case 7: snprintf(buf, sizeof buf, "%s | %s", a, b); break;
    case 8: snprintf(buf, sizeof buf, "%s ^ %s", a, b); break;
    case 9: snprintf(buf, sizeof buf, "FM_CLZ(%s) + FM_CLZ(%s)", a, b); break;
    default: snprintf(buf, sizeof buf, "FM_POPC(%s) + FM_POPC(%s)", a, b); break;
    }
    return buf;
}

inline std::string emit_macros(const Program& p)
{
    std::string s;
    char buf[128];
    snprintf(buf, sizeof buf, "// KawPow program of period %llu (blocks %llu..%llu)\n", (unsigned long long)p.period,
             (unsigned long long)(p.period * PERIOD), (unsigned long long)(p.period * PERIOD + PERIOD - 1));
    s += buf;
    s += "#define FM_PROGRAM_OPS \\\n";
    for (const Op& o : p.ops) {
        if (o.cache) {
            snprintf(buf, sizeof buf, "FM_CACHE(mix[%d] & 4095u)", o.src1);
            s += "    { const uint32_t t_ = " + std::string(buf) + "; " + emit_merge(o.dst, "t_", o.sel2) + " } \\\n";
        } else {
            s += "    { const uint32_t t_ = " + emit_math(o.src1, o.src2, o.sel1) + "; " + emit_merge(o.dst, "t_", o.sel2) + " } \\\n";
        }
    }
    s += "\n#define FM_PROGRAM_DAG \\\n";
    for (int i = 0; i < DAG_WORDS; i++) {
        snprintf(buf, sizeof buf, "dag_w%d", i);
        s += "    { " + emit_merge(p.dag_dst[i], buf, p.dag_sel[i]) + " } \\\n";
    }
    s += "\n";
    return s;
}

}  // namespace fmgen
