// kawpow_ref.h - CPU reference of KawPow for FiloMiner (FiloPow / FPOW), on top of
// the ethash library (third_party/crypto/ethash, Apache-2.0, from the FiloPow
// sources).  Apache-2.0.
#pragma once
#include <stdint.h>
#include <memory>

namespace kref {

static const int EPOCH_LENGTH = 7500;
static const int PERIOD_LENGTH = 3;

inline int epoch_of(int height) { return height / EPOCH_LENGTH; }
inline uint64_t period_of(int height) { return (uint64_t)height / PERIOD_LENGTH; }

// Light epoch context (light cache, ~16 MB + 128 KB per epoch).  Thread-safe to read.
struct Epoch {
    int epoch = -1;
    const uint32_t* light_cache = nullptr;    // light_items x 16 words
    uint32_t light_items = 0;                  // 512-bit light cache items
    uint32_t full_items = 0;                   // 1024-bit full dataset items
    uint64_t dag_bytes() const { return (uint64_t)full_items * 128; }
    uint32_t dag_items_2048() const { return full_items / 2; }
    std::shared_ptr<void> ctx;                 // ethash::epoch_context
};

// Builds (or returns the cached) context of an epoch.  Keeps the last two epochs.
std::shared_ptr<const Epoch> get_epoch(int epoch);

// KawPow of (height, header hash bytes, nonce): mix hash and final hash, both as
// the 32 bytes in the order the node and the pool print them.
void hash(const Epoch& e, int height, const uint8_t header[32], uint64_t nonce, uint8_t mix[32], uint8_t final_hash[32]);

// final_hash (big-endian 256-bit number) <= target (32 bytes, big-endian)
bool le_target(const uint8_t final_hash[32], const uint8_t target_be[32]);

// one 512-bit DAG item computed by the library (tests)
void dag_item512(const Epoch& e, uint32_t index, uint32_t out[16]);

// library Keccak-f[800] (tests)
void keccak_f800(uint32_t st[25]);

}  // namespace kref
