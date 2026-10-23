// ref_test.cpp - cloud-side checks: ethash reference against the KawPow test
// vectors, the shared Keccak-f[800] / DAG item code against the library, and
// prints the DAG sizes used by the simulator.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "../src/kawpow_ref.h"
#include "../gpu/fm_dag.h"
#include "crypto/ethash/progpow_test_vectors.hpp"

static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static void unhex(const char* s, uint8_t* o, int n) { for (int i = 0; i < n; i++) o[i] = (uint8_t)(hexv(s[2 * i]) * 16 + hexv(s[2 * i + 1])); }
static std::string hex(const uint8_t* p, int n) { std::string s; char b[3]; for (int i = 0; i < n; i++) { snprintf(b, 3, "%02x", p[i]); s += b; } return s; }

int main(int argc, char** argv)
{
    int fails = 0;
    if (argc > 1 && !strcmp(argv[1], "items")) {   // items <epoch>: dag item count
        auto e = kref::get_epoch(atoi(argv[2]));
        printf("%u\n", e->dag_items_2048());
        return 0;
    }
    for (auto& t : progpow_hash_test_cases) {
        auto e = kref::get_epoch(kref::epoch_of(t.block_number));
        uint8_t h[32], mix[32], fin[32];
        unhex(t.header_hash_hex, h, 32);
        kref::hash(*e, t.block_number, h, strtoull(t.nonce_hex, nullptr, 16), mix, fin);
        bool ok = hex(mix, 32) == t.mix_hash_hex && hex(fin, 32) == t.final_hash_hex;
        if (!ok) fails++;
        printf("vector %6d: %s\n", t.block_number, ok ? "OK" : "WRONG");
    }
    {
        auto e = kref::get_epoch(4);
        uint8_t h[32], mix[32], fin[32];
        unhex("ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff", h, 32);
        kref::hash(*e, 30000, h, 0x123456789abcdef0ull, mix, fin);
        bool ok = hex(mix, 32) == "177b565752a375501e11b6d9d3679c2df6197b2cab3a1ba2d6b10b8c71a3d459" &&
                  hex(fin, 32) == "c824bee0418e3cfb7fae56e0d5b3b8b14ba895777feea81c70c0ba947146da69";
        if (!ok) fails++;
        printf("vector  30000 (kawpow_tests.cpp): %s\n", ok ? "OK" : "WRONG");
        // DAG items of fm_dag.h vs library
        int bad = 0;
        for (uint32_t i = 0; i < 2000; i++) {
            uint32_t idx = i < 1000 ? i : (uint32_t)((i * 2654435761u) % (e->full_items * 2));
            uint32_t a[16], b[16];
            kref::dag_item512(*e, idx, a);
            fmdag::item512(e->light_cache, e->light_items, idx, b);
            if (memcmp(a, b, 64)) bad++;
        }
        printf("fm_dag.h item512 vs library (2000 items, epoch 4): %s\n", bad ? "WRONG" : "OK");
        fails += bad != 0;
        printf("epoch 4: light items %u, full items %u, DAG %.1f MB\n", e->light_items, e->full_items, e->dag_bytes() / 1048576.0);
    }
    printf("RESULT: %s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
