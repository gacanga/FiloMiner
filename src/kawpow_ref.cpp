// kawpow_ref.cpp - CPU reference of KawPow for FiloMiner.  Apache-2.0.
#include "kawpow_ref.h"

#include <crypto/ethash/include/ethash/ethash.hpp>
#include <crypto/ethash/include/ethash/progpow.hpp>
#include <crypto/ethash/include/ethash/keccak.h>
#include "crypto/ethash/lib/ethash/ethash-internal.hpp"

#include <mutex>
#include <map>
#include <string.h>


namespace kref {

static std::mutex g_mtx;
static std::map<int, std::shared_ptr<const Epoch>> g_cache;

std::shared_ptr<const Epoch> get_epoch(int epoch)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_cache.find(epoch);
    if (it != g_cache.end()) return it->second;
    auto e = std::make_shared<Epoch>();
    ethash::epoch_context_ptr p = ethash::create_epoch_context(epoch);
    if (!p) return nullptr;
    const ethash::epoch_context* c = p.get();
    e->epoch = epoch;
    e->light_cache = reinterpret_cast<const uint32_t*>(c->light_cache);
    e->light_items = (uint32_t)c->light_cache_num_items;
    e->full_items = (uint32_t)c->full_dataset_num_items;
    ethash::epoch_context* raw = p.release();
    e->ctx = std::shared_ptr<void>(raw, [](void* q) { ethash_destroy_epoch_context(static_cast<ethash::epoch_context*>(q)); });
    g_cache[epoch] = e;
    while (g_cache.size() > 2) {
        auto lo = g_cache.begin();
        if (lo->first == epoch) ++lo;
        g_cache.erase(lo);
    }
    return e;
}

void hash(const Epoch& e, int height, const uint8_t header[32], uint64_t nonce, uint8_t mix[32], uint8_t final_hash[32])
{
    const ethash::epoch_context& c = *static_cast<const ethash::epoch_context*>(e.ctx.get());
    ethash::hash256 h;
    memcpy(h.bytes, header, 32);
    const ethash::result r = progpow::hash(c, height, h, nonce);
    memcpy(mix, r.mix_hash.bytes, 32);
    memcpy(final_hash, r.final_hash.bytes, 32);
}

bool le_target(const uint8_t final_hash[32], const uint8_t target_be[32])
{
    for (int i = 0; i < 32; i++) {
        if (final_hash[i] < target_be[i]) return true;
        if (final_hash[i] > target_be[i]) return false;
    }
    return true;
}

void dag_item512(const Epoch& e, uint32_t index, uint32_t out[16])
{
    const ethash::epoch_context& c = *static_cast<const ethash::epoch_context*>(e.ctx.get());
    const ethash::hash512 it = ethash::calculate_dataset_item_512(c, index);
    memcpy(out, it.word32s, 64);
}

void keccak_f800(uint32_t st[25]) { ethash_keccakf800(st); }

}  // namespace kref
