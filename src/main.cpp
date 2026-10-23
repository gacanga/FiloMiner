// FiloMiner - GPU miner for FiloPow (FPOW) and other KawPow coins (Ravencoin
// constants), NOMP-style KawPow stratum (plain TCP or TLS).  No developer fee.
// Apache-2.0.
//
//   filominer.exe                     mine with filominer.conf
//   filominer.exe --test              CPU reference + GPU correctness tests
//   filominer.exe --bench [seconds]   hashrate benchmark on the selected GPUs
//
// Options (also key=value lines in filominer.conf, without "--"):
//   -o / --pool URL   (repeatable, failover order)   -u / --user ADDRESS.RIG   -p / --pass X
//   --gpus 0,1|all    --tpb N (threads per block)    --grid N (blocks per launch, 0 = auto)
//   --target-ms N     duration of one GPU launch     --stats N (seconds)
//   --log FILE        --args-file FILE               --cafile FILE
//   --bench-height N  block height used by --bench (epoch / period)
//
// Stratum (KawPow pools, NOMP / kawpow-stratum):
//   -> mining.subscribe [agent]                 <- [session, extranonce1]
//   -> mining.authorize [user, pass]            <- true
//   <- mining.set_target [target_hex_be]
//   <- mining.notify [job, header_hash, seed_hash, target, clean, height, nbits]
//   -> mining.submit [user, job, "0x"+nonce(16 hex), "0x"+header_hash, "0x"+mix_hash]
// The 64-bit nonce starts with extranonce1 (most significant bytes).
#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "kawpow_ref.h"
#include "json.h"
#include "tls_conn.h"
#include "../gpu/fm_gpu.h"
#include "crypto/ethash/progpow_test_vectors.hpp"

#define FILOMINER_VERSION "0.1.0"

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static std::mutex g_log_mtx;
static FILE* g_logf = nullptr;

static void logf_(const char* fmt, ...)
{
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    std::lock_guard<std::mutex> lk(g_log_mtx);
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);
    if (g_logf) { fprintf(g_logf, "[%s] %s\n", ts, msg); fflush(g_logf); }
}
#define LOG(...) logf_(__VA_ARGS__)

// ---------------------------------------------------------------------------
// hex helpers
// ---------------------------------------------------------------------------
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static std::string strip0x(const std::string& s)
{
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
}
static bool hex_to_bytes(const std::string& h0, uint8_t* out, size_t n)
{
    std::string h = strip0x(h0);
    if (h.size() != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}
static std::string to_hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}
static uint64_t be64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}
// expected hashes per share for a big-endian target
static double target_work(const uint8_t t[32])
{
    double v = 0;
    for (int i = 0; i < 32; i++) v = v * 256.0 + t[i];
    return v > 0 ? 115792089237316195423570985008687907853269984665640564039457584007913129639936.0 / v : 0;
}
// KawPow pools: difficulty 1 = 2^32 hashes
static double work_to_diff(double w) { return w / 4294967296.0; }

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------
struct Config {
    std::vector<std::string> pools;
    std::string user, pass = "x";
    std::vector<int> gpus;          // empty = all
    int tpb = 256;
    int grid = 0;                   // 0 = automatic (target-ms)
    int target_ms = 150;
    std::string cafile;
    int stats = 30;
    std::string logfile;
    bool test = false;
    bool bench = false;
    int bench_seconds = 30;
    int bench_height = 150000;
    std::string exe_dir = ".";
};

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool apply_option(Config& c, const std::string& key, const std::string& val)
{
    std::string k = key;
    while (!k.empty() && k[0] == '-') k.erase(0, 1);
    if (k == "o" || k == "pool" || k == "url") c.pools.push_back(val);
    else if (k == "u" || k == "user") c.user = val;
    else if (k == "p" || k == "pass") c.pass = val;
    else if (k == "gpus" || k == "devices") {
        c.gpus.clear();
        if (val != "all" && !val.empty()) {
            size_t p = 0;
            while (p <= val.size()) {
                size_t q = val.find(',', p);
                std::string one = trim(val.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (!one.empty()) c.gpus.push_back(atoi(one.c_str()));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
    }
    else if (k == "tpb") c.tpb = atoi(val.c_str());
    else if (k == "grid") c.grid = atoi(val.c_str());
    else if (k == "target-ms" || k == "target_ms") c.target_ms = atoi(val.c_str());
    else if (k == "cafile") c.cafile = val;
    else if (k == "stats") c.stats = atoi(val.c_str());
    else if (k == "log") c.logfile = val;
    else if (k == "bench-height" || k == "bench_height") c.bench_height = atoi(val.c_str());
    else return false;
    return true;
}

static bool load_config_file(Config& c, const std::string& path)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (!apply_option(c, k, v)) fprintf(stderr, "config: unknown key '%s'\n", k.c_str());
    }
    fclose(f);
    return true;
}

static std::string exe_directory(const char* argv0)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p = n ? std::string(buf, n) : std::string(argv0);
#else
    std::string p = argv0;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) p.assign(buf, (size_t)n);
#endif
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "." : p.substr(0, s);
}

static bool file_exists(const std::string& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// shared mining state
// ---------------------------------------------------------------------------
struct PoolJob {
    uint64_t seq = 0;
    std::string job_id, header_hex;
    uint8_t header[32];
    int height = 0;
    uint8_t target[32];              // share target, big-endian
    uint64_t target64 = 0;           // its top 64 bits
    uint8_t net_target[32];          // from nbits (all 0xff if unknown)
    double work = 0;
    uint8_t xn[8];
    size_t xn_len = 0;
};

struct Candidate {
    std::shared_ptr<const PoolJob> job;
    uint64_t nonce;
    int gpu;
};

struct Share {
    uint64_t seq;
    double work;
    std::string job_id, nonce_hex, header_hex, mix_hex, hash_hex;
    int gpu;
    bool block;
};

static std::atomic<bool> g_running(true);
static std::atomic<int> g_gpu_failed(0);
static std::mutex g_mtx;
static std::shared_ptr<const PoolJob> g_job;
static bool g_paused = true;
static uint64_t g_job_seq = 0;
static std::deque<Share> g_shares;

static std::mutex g_cand_mtx;
static std::condition_variable g_cand_cv;
static std::deque<Candidate> g_cands;

struct GpuStat {
    std::atomic<uint64_t> hashes{ 0 };
    std::atomic<uint64_t> found{ 0 };       // verified shares
    std::atomic<uint64_t> bad{ 0 };         // GPU candidates that failed the CPU check
    std::atomic<int> grid{ 0 };
    std::atomic<int> launch_ms{ 0 };
    std::atomic<bool> alive{ false };
    std::atomic<bool> busy{ false };        // DAG build / compile in progress (watchdog off)
    std::atomic<long long> beat_ms{ 0 };
    std::string name;
    int dev = -1;
};
static std::vector<std::unique_ptr<GpuStat>> g_stats;

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

#define FM_STUCK_MS 20000
static void deadline_watchdog(int seconds, const char* what)
{
    std::thread([seconds, what]() {
        for (int i = 0; i < seconds * 10; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LOG("%s TIMEOUT after %d s: a GPU is stuck (unstable clocks?), exiting with code 5", what, seconds);
        fflush(nullptr);
        std::_Exit(5);
    }).detach();
}

static std::atomic<uint64_t> g_accepted(0), g_rejected(0), g_stale_rej(0), g_blocks(0);
static double g_accepted_work = 0;
static std::atomic<uint64_t> g_precompiled(0);

static void on_signal(int) { g_running = false; }

// compiles the next period's program in the background (once for all GPUs)
static void precompile_next(uint64_t period, uint32_t items, int cc, int tpb)
{
    uint64_t want = period + 1, cur = g_precompiled.load();
    while (cur < want) {
        if (g_precompiled.compare_exchange_weak(cur, want)) {
            std::thread([want, items, cc, tpb]() {
                if (fm_gpu_precompile(want, items, cc, tpb) != 0) LOG("precompile of period %llu failed: %s", (unsigned long long)want, fm_gpu_last_error(nullptr));
            }).detach();
            return;
        }
    }
}

// epoch DAG + period program for a height; returns false on a CUDA error
static bool prepare_gpu(void* g, GpuStat& st, int height, int& cur_epoch, uint64_t& cur_period, int tpb, bool quiet = false)
{
    const int epoch = kref::epoch_of(height);
    const uint64_t period = kref::period_of(height);
    if (epoch != cur_epoch) {
        st.busy = true;
        auto t0 = Clock::now();
        auto e = kref::get_epoch(epoch);
        double light_s = std::chrono::duration<double>(Clock::now() - t0).count();
        if (!e) { LOG("GPU %d: cannot build the light cache of epoch %d", st.dev, epoch); st.busy = false; return false; }
        double secs = 0;
        if (fm_gpu_build_dag(g, epoch, e->light_cache, e->light_items, e->full_items, &secs) != 0) {
            LOG("GPU %d: DAG build failed: %s", st.dev, fm_gpu_last_error(g));
            st.busy = false;
            return false;
        }
        LOG("GPU %d: DAG epoch %d (%.0f MB) built in %.1f s (light cache %.1f s)", st.dev, epoch, e->dag_bytes() / 1048576.0, secs, light_s);
        cur_epoch = epoch;
        cur_period = ~0ull;
        st.busy = false;
    }
    if (period != cur_period) {
        st.busy = true;
        int compiled = 0;
        double secs = 0;
        if (fm_gpu_set_period(g, period, &compiled, &secs) != 0) {
            LOG("GPU %d: program of period %llu: %s", st.dev, (unsigned long long)period, fm_gpu_last_error(g));
            st.busy = false;
            return false;
        }
        if (!quiet) {
            int regs = 0, lmem = 0;
            fm_gpu_kernel_info(g, &regs, &lmem);
            LOG("GPU %d: program of period %llu (blocks %llu-%llu) %s%s, %d registers%s", st.dev, (unsigned long long)period,
                (unsigned long long)(period * 3), (unsigned long long)(period * 3 + 2), compiled ? "compiled in " : "ready",
                compiled ? (std::to_string((int)(secs * 1000)) + " ms").c_str() : "", regs, lmem ? " (LOCAL MEMORY USED)" : "");
        }
        cur_period = period;
        st.busy = false;
        precompile_next(period, kref::get_epoch(epoch)->dag_items_2048(), fm_gpu_compute_capability(g), tpb);
    }
    return true;
}

// ---------------------------------------------------------------------------
// GPU worker
// ---------------------------------------------------------------------------
static const int GPU_BITS = 4;   // up to 16 GPUs share the nonce space after extranonce1

static void gpu_worker(const Config& cfg, int idx, int dev)
{
    GpuStat& st = *g_stats[idx];
    void* g = fm_gpu_create(dev, cfg.tpb, cfg.grid);
    if (!g) { LOG("GPU %d: init failed: %s", dev, fm_gpu_last_error(nullptr)); return; }
    st.name = fm_gpu_name(g);
    st.beat_ms = now_ms();
    st.alive = true;
    LOG("GPU %d: %s, sm_%d, %d SMs, %.0f MB", dev, st.name.c_str(), fm_gpu_compute_capability(g), fm_gpu_sm_count(g),
        fm_gpu_memory(g) / 1048576.0);

    std::mt19937_64 rng((uint64_t)Clock::now().time_since_epoch().count() ^ ((uint64_t)dev << 40));
    uint64_t cur_seq = 0, base = 0, counter = 0, limit = 0;
    int cur_epoch = -1;
    uint64_t cur_period = ~0ull;
    bool exhausted_logged = false;
    const int target_ms = cfg.target_ms > 20 ? cfg.target_ms : 20;
    int grid = cfg.grid > 0 ? cfg.grid : fm_gpu_sm_count(g) * 32;
    fm_gpu_set_grid(g, grid);
    uint64_t found[FM_GPU_MAX_RESULTS];
    uint32_t hdr[8];

    while (g_running) {
        st.beat_ms = now_ms();
        std::shared_ptr<const PoolJob> job;
        bool paused;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            job = g_job;
            paused = g_paused;
        }
        if (paused || !job) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        if (!prepare_gpu(g, st, job->height, cur_epoch, cur_period, cfg.tpb)) {
            g_gpu_failed = dev + 1;
            g_running = false;
            break;
        }
        st.beat_ms = now_ms();
        if (job->seq != cur_seq) {
            cur_seq = job->seq;
            const int free_bits = 64 - 8 * (int)job->xn_len;
            const int cbits = free_bits - GPU_BITS;
            uint64_t prefix = 0;
            for (size_t i = 0; i < job->xn_len; i++) prefix = (prefix << 8) | job->xn[i];
            base = (job->xn_len ? prefix << free_bits : 0) | ((uint64_t)(idx & 15) << cbits);
            limit = cbits >= 63 ? ~0ull : (1ull << cbits);
            // random start in the lower half of the space (no overlap after reconnects)
            counter = cbits >= 40 ? (rng() & ((1ull << (cbits - 1)) - 1)) & ~0xffffffull : 0;
            memcpy(hdr, job->header, 32);
            exhausted_logged = false;
        }
        const uint64_t batch = fm_gpu_batch(g);
        if (counter + batch > limit) {
            if (!exhausted_logged) { LOG("GPU %d: nonce space of job %s used up, waiting for a new job", dev, job->job_id.c_str()); exhausted_logged = true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        auto t0 = Clock::now();
        int nf = 0;
        if (fm_gpu_search(g, base + counter, hdr, job->target64, found, FM_GPU_MAX_RESULTS, &nf) != 0) {
            LOG("GPU %d: CUDA error: %s", dev, fm_gpu_last_error(g));
            g_gpu_failed = dev + 1;
            g_running = false;
            break;
        }
        counter += batch;
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        st.hashes += batch;
        st.beat_ms = now_ms();
        st.launch_ms = (int)ms;
        if (cfg.grid <= 0 && ms > 1.0) {
            double want = (double)grid * target_ms / ms;
            if (want > grid * 2.0) want = grid * 2.0;
            if (want < grid / 2.0) want = grid / 2.0;
            if (want < 16) want = 16;
            if (want > 65536) want = 65536;
            grid = (int)want;
            fm_gpu_set_grid(g, grid);
        }
        st.grid = grid;
        int m = nf < FM_GPU_MAX_RESULTS ? nf : FM_GPU_MAX_RESULTS;
        if (nf > m) LOG("GPU %d: %d candidates in one launch, extra ones dropped", dev, nf);
        if (m > 0) {
            std::lock_guard<std::mutex> lk(g_cand_mtx);
            for (int i = 0; i < m; i++) g_cands.push_back(Candidate{ job, found[i], idx });
        }
        if (m > 0) g_cand_cv.notify_all();
    }
    st.alive = false;
    fm_gpu_destroy(g);
}

// ---------------------------------------------------------------------------
// verifier: recomputes every GPU candidate on the CPU (also gives the mix hash)
// ---------------------------------------------------------------------------
static void verify_worker()
{
    while (true) {
        Candidate c;
        {
            std::unique_lock<std::mutex> lk(g_cand_mtx);
            g_cand_cv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_cands.empty() || !g_running; });
            if (!g_running) return;
            if (g_cands.empty()) continue;
            c = g_cands.front();
            g_cands.pop_front();
        }
        GpuStat& st = *g_stats[(size_t)c.gpu];
        auto e = kref::get_epoch(kref::epoch_of(c.job->height));
        if (!e) continue;
        uint8_t mix[32], fin[32];
        kref::hash(*e, c.job->height, c.job->header, c.nonce, mix, fin);
        if (be64(fin) > c.job->target64) {
            st.bad++;
            LOG("GPU %d: WRONG result for nonce %016llx (CPU check failed: unstable clocks?)", st.dev, (unsigned long long)c.nonce);
            continue;
        }
        if (!kref::le_target(fin, c.job->target)) continue;
        st.found++;
        Share s;
        s.seq = c.job->seq;
        s.work = c.job->work;
        s.job_id = c.job->job_id;
        char nb[24];
        snprintf(nb, sizeof nb, "%016llx", (unsigned long long)c.nonce);
        s.nonce_hex = nb;
        s.header_hex = c.job->header_hex;
        s.mix_hex = to_hex(mix, 32);
        s.hash_hex = to_hex(fin, 32);
        s.gpu = st.dev;
        s.block = kref::le_target(fin, c.job->net_target);
        if (s.block) LOG("GPU %d: BLOCK candidate at height %d (hash %s)", st.dev, c.job->height, s.hash_hex.c_str());
        std::lock_guard<std::mutex> lk(g_mtx);
        g_shares.push_back(s);
    }
}

// ---------------------------------------------------------------------------
// pool client (KawPow NOMP stratum)
// ---------------------------------------------------------------------------
struct Pending { int gpu; std::string job_id, hash; uint64_t seq; double work; bool block; };

static long long error_code(const json::Value* e)
{
    if (!e || e->is_null()) return 0;
    if (e->is_arr() && !e->arr.empty() && e->arr[0].is_num()) return strtoll(e->arr[0].s.c_str(), nullptr, 10);
    if (e->is_obj()) return e->num("code", -1);
    return -1;
}

static const json::Value* arr_at(const json::Value* a, size_t i)
{
    if (!a || !a->is_arr() || a->arr.size() <= i) return nullptr;
    return &a->arr[i];
}
static std::string arr_str(const json::Value* a, size_t i)
{
    const json::Value* v = arr_at(a, i);
    return (v && (v->is_str() || v->is_num())) ? v->s : std::string();
}

// compact nbits -> 256-bit big-endian target
static bool nbits_target(const std::string& hex, uint8_t out[32])
{
    uint8_t b[4];
    memset(out, 0xff, 32);
    if (!hex_to_bytes(hex, b, 4)) return false;
    const int exp = b[0];
    if (exp < 3 || exp > 32) return false;
    memset(out, 0, 32);
    const int pos = 32 - exp;   // index of the most significant mantissa byte
    for (int i = 0; i < 3; i++)
        if (pos + i >= 0 && pos + i < 32) out[pos + i] = b[1 + i];
    return true;
}

// target for a pool difficulty (diff 1 = 0x00000000ffff0000...)
static void diff_target(double diff, uint8_t out[32])
{
    memset(out, 0, 32);
    if (diff <= 0) { memset(out, 0xff, 32); return; }
    double v = 65535.0 / diff;    // value of bytes 4..5 as a 16-bit number scaled
    // place v * 2^(256-48) as a big-endian number
    int e = 0;
    double m = frexp(v, &e);      // v = m * 2^e, 0.5 <= m < 1
    int bitpos = 208 + e;         // highest bit position (0 = lsb)
    for (int i = 0; i < 64 && bitpos - i >= 0; i++) {
        m *= 2;
        int bit = m >= 1.0;
        if (bit) m -= 1.0;
        int p = bitpos - 1 - i;
        if (bit && p < 256 && p >= 0) out[31 - p / 8] |= (uint8_t)(1u << (p % 8));
    }
}

static bool parse_notify(const json::Value* p, const uint8_t* xn, size_t xn_len, const uint8_t target[32], bool have_target,
                         PoolJob& j, std::string& why)
{
    j.job_id = arr_str(p, 0);
    std::string hh = strip0x(arr_str(p, 1)), tgt = arr_str(p, 3), height = arr_str(p, 5), nbits = arr_str(p, 6);
    if (j.job_id.empty()) { why = "missing job id"; return false; }
    if (!hex_to_bytes(hh, j.header, 32)) { why = "bad header hash"; return false; }
    j.header_hex = hh;
    if (height.empty()) { why = "no block height in the job"; return false; }
    j.height = atoi(height.c_str());
    if (j.height <= 0) { why = "bad block height"; return false; }
    uint8_t t[32];
    if (hex_to_bytes(tgt, t, 32)) memcpy(j.target, t, 32);
    else if (have_target) memcpy(j.target, target, 32);
    else { why = "no share target"; return false; }
    j.target64 = be64(j.target);
    if (nbits.empty() || !nbits_target(nbits, j.net_target)) memset(j.net_target, 0, 32);
    memcpy(j.xn, xn, 8);
    j.xn_len = xn_len;
    j.work = target_work(j.target);
    return true;
}

static void pool_worker(const Config& cfg)
{
    TlsConn conn;
    size_t pool_idx = 0;
    int backoff = 2;
    while (g_running) {
        std::string url = cfg.pools[pool_idx % cfg.pools.size()];
        LOG("pool: connecting to %s", url.c_str());
        if (!conn.connect(url, cfg.cafile, 10000)) {
            LOG("pool: %s", conn.error().c_str());
            pool_idx++;
            for (int i = 0; i < backoff * 10 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff = backoff * 2 > 60 ? 60 : backoff * 2;
            continue;
        }
        LOG("pool: connected to %s", conn.host().c_str());
        int next_id = 3;
        std::map<int, Pending> pending;
        bool subscribed = false, authorized = false;
        uint8_t xn[8] = { 0 };
        size_t xn_len = 0;
        uint8_t target[32];
        memset(target, 0xff, 32);
        bool have_target = false;
        json::Value last_notify;
        bool have_notify = false;
        int raw_lines = 0;                // first lines of a session are logged raw
        auto t_conn = Clock::now();
        auto t_rx = Clock::now();
        conn.send_line(std::string("{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[") +
                       json::quote(std::string("filominer/") + FILOMINER_VERSION) + "]}");
        bool lost = false;

        auto take_job = [&](const json::Value* params) {
            if (!subscribed) return;
            auto j = std::make_shared<PoolJob>();
            std::string why;
            if (!parse_notify(params, xn, xn_len, target, have_target, *j, why)) {
                LOG("pool: ignoring job: %s: %.300s", why.c_str(), json::dump(*params).c_str());
                return;
            }
            int prev_height = 0;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                prev_height = g_job ? g_job->height : 0;
                j->seq = ++g_job_seq;
                g_job = j;
                g_paused = false;
            }
            LOG("pool: new job %s height %d%s (share difficulty %.4g)", j->job_id.c_str(), j->height,
                j->height != prev_height ? "" : " (same block)", work_to_diff(j->work));
        };

        while (g_running && !lost) {
            std::vector<std::string> lines;
            if (!conn.poll_lines(lines, 100)) { LOG("pool: %s", conn.error().c_str()); lost = true; break; }
            if (!lines.empty()) t_rx = Clock::now();
            for (auto& line : lines) {
                if (raw_lines < 6) { raw_lines++; LOG("pool <- %.400s", line.c_str()); }
                json::Value v;
                if (!json::parse(line, v) || !v.is_obj()) { LOG("pool: unparsable line: %.200s", line.c_str()); continue; }
                const json::Value* method = v.get("method");
                const json::Value* params = v.get("params");
                if (method && method->is_str()) {
                    const std::string& m = method->s;
                    if (m == "mining.notify") {
                        last_notify = params ? *params : json::Value();
                        have_notify = true;
                        take_job(params);
                    } else if (m == "mining.set_target") {
                        std::string t = arr_str(params, 0);
                        uint8_t be[32];
                        if (hex_to_bytes(t, be, 32)) {
                            memcpy(target, be, 32);
                            have_target = true;
                            LOG("pool: share target %s (difficulty %.4g)", strip0x(t).c_str(), work_to_diff(target_work(target)));
                        } else LOG("pool: bad set_target: %.200s", line.c_str());
                    } else if (m == "mining.set_difficulty") {
                        double d = atof(arr_str(params, 0).c_str());
                        diff_target(d, target);
                        have_target = true;
                        LOG("pool: share difficulty %.4g", d);
                    } else if (m == "mining.set_extranonce") {
                        std::string x = strip0x(arr_str(params, 0));
                        if (!x.empty() && x.size() <= 12 && x.size() % 2 == 0) {
                            xn_len = x.size() / 2;
                            memset(xn, 0, 8);
                            hex_to_bytes(x, xn, xn_len);
                            LOG("pool: new extranonce1 %s", x.c_str());
                            if (have_notify) take_job(&last_notify);
                        }
                    } else if (m == "client.reconnect") {
                        LOG("pool: reconnect requested");
                        lost = true;
                    } else if (m == "client.show_message") {
                        LOG("pool message: %s", arr_str(params, 0).c_str());
                    } else {
                        LOG("pool: unhandled method %s: %.300s", m.c_str(), line.c_str());
                    }
                    continue;
                }
                long long id = v.num("id", -1);
                const json::Value* result = v.get("result");
                const json::Value* error = v.get("error");
                bool has_err = error && !error->is_null();
                if (id == 1) {
                    std::string x = strip0x(arr_str(result, 1));
                    if (has_err || x.size() > 12 || x.size() % 2) {
                        LOG("pool: subscribe refused: %s", line.c_str());
                        lost = true;
                        break;
                    }
                    xn_len = x.size() / 2;
                    memset(xn, 0, 8);
                    if (xn_len) hex_to_bytes(x, xn, xn_len);
                    subscribed = true;
                    LOG("pool: subscribed, extranonce1 '%s' (%d bits of nonce left)", x.c_str(), 64 - 8 * (int)xn_len);
                    conn.send_line(std::string("{\"id\":2,\"method\":\"mining.authorize\",\"params\":[") + json::quote(cfg.user) + "," +
                                   json::quote(cfg.pass) + "]}");
                    continue;
                }
                if (id == 2) {
                    bool ok = !has_err && result && ((result->is_bool() && result->b) || (!result->is_null() && !result->is_bool()));
                    if (!ok) {
                        LOG("pool: authorization refused: %s", line.c_str());
                        lost = true;
                        backoff = 60;
                        break;
                    }
                    authorized = true;
                    backoff = 2;
                    LOG("pool: authorized as %s", cfg.user.c_str());
                    if (have_notify && !g_job) take_job(&last_notify);
                    continue;
                }
                auto it = pending.find((int)id);
                if (it == pending.end()) { LOG("pool: unexpected reply: %.300s", line.c_str()); continue; }
                Pending pd = it->second;
                pending.erase(it);
                bool acc = !has_err && result && !result->is_null() && !(result->is_bool() && !result->b);
                if (acc) {
                    g_accepted++;
                    if (pd.block) g_blocks++;
                    { std::lock_guard<std::mutex> lk(g_mtx); g_accepted_work += pd.work; }
                    LOG("share accepted (GPU %d)%s %s  [%llu/%llu]", pd.gpu, pd.block ? " BLOCK" : "", pd.hash.substr(0, 16).c_str(),
                        (unsigned long long)g_accepted.load(), (unsigned long long)(g_accepted.load() + g_rejected.load()));
                } else {
                    g_rejected++;
                    long long code = error_code(error);
                    if (code == 21) g_stale_rej++;
                    LOG("share REJECTED (GPU %d) job %s: %s  [%llu/%llu]", pd.gpu, pd.job_id.c_str(),
                        has_err ? json::dump(*error).c_str() : line.c_str(),
                        (unsigned long long)g_accepted.load(), (unsigned long long)(g_accepted.load() + g_rejected.load()));
                }
            }
            if (lost) break;
            if (authorized) {
                std::deque<Share> q;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    q.swap(g_shares);
                }
                for (auto& s : q) {
                    int id = next_id++;
                    pending[id] = Pending{ s.gpu, s.job_id, s.hash_hex, s.seq, s.work, s.block };
                    std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"mining.submit\",\"params\":[" + json::quote(cfg.user) + "," +
                                      json::quote(s.job_id) + "," + json::quote("0x" + s.nonce_hex) + "," + json::quote("0x" + s.header_hex) + "," +
                                      json::quote("0x" + s.mix_hex) + "]}";
                    if (g_accepted.load() + g_rejected.load() < 2) LOG("pool -> %s", msg.c_str());
                    if (!conn.send_line(msg)) { LOG("pool: send failed: %s", conn.error().c_str()); lost = true; break; }
                }
            } else if (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t_conn).count() > 60) {
                LOG("pool: no authorization within 60 s");
                lost = true;
            }
            if (lost) break;
            if (Clock::now() - t_rx > std::chrono::seconds(300)) { LOG("pool: nothing received for 300 s, reconnecting"); lost = true; }
        }
        conn.close();
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_paused = true;
            g_job.reset();
            g_shares.clear();
        }
        if (!g_running) break;
        pool_idx++;
        LOG("pool: disconnected, retrying in %d s", backoff);
        for (int i = 0; i < backoff * 10 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backoff = backoff * 2 > 60 ? 60 : backoff * 2;
    }
}

// ---------------------------------------------------------------------------
// tests and benchmark
// ---------------------------------------------------------------------------
static std::vector<int> selected_devices(const Config& cfg)
{
    int n = fm_gpu_device_count();
    std::vector<int> d;
    if (cfg.gpus.empty()) { for (int i = 0; i < n; i++) d.push_back(i); }
    else { for (int g : cfg.gpus) if (g >= 0 && g < n) d.push_back(g); }
    return d;
}

static int run_tests(const Config& cfg)
{
    // 1) CPU reference against the KawPow test vectors (Ravencoin / FiloPow sources)
    int fails = 0, n = 0;
    for (auto& t : progpow_hash_test_cases) {
        auto e = kref::get_epoch(kref::epoch_of(t.block_number));
        uint8_t h[32], mix[32], fin[32];
        hex_to_bytes(t.header_hash_hex, h, 32);
        kref::hash(*e, t.block_number, h, strtoull(t.nonce_hex, nullptr, 16), mix, fin);
        if (to_hex(mix, 32) != t.mix_hash_hex || to_hex(fin, 32) != t.final_hash_hex) { fails++; LOG("TEST CPU reference: vector %d WRONG", t.block_number); }
        n++;
    }
    if (fails) { LOG("TEST CPU reference: FAILED (%d/%d)", fails, n); LOG("TEST RESULT: FAILED"); return 1; }
    LOG("TEST CPU reference: %d/%d KawPow test vectors OK", n, n);

    std::vector<int> devs = selected_devices(cfg);
    if (devs.empty()) { LOG("TEST: no CUDA device"); LOG("TEST RESULT: FAILED"); return 1; }
    const int height = cfg.bench_height;
    auto e = kref::get_epoch(kref::epoch_of(height));
    int total_fail = 0;
    std::mt19937_64 rng(1234);
    for (int dev : devs) {
        GpuStat st;
        st.dev = dev;
        void* g = fm_gpu_create(dev, cfg.tpb, cfg.grid);
        if (!g) { LOG("TEST GPU %d: create failed: %s", dev, fm_gpu_last_error(nullptr)); total_fail++; continue; }
        int fail = 0;
        int cur_epoch = -1;
        uint64_t cur_period = ~0ull;
        if (!prepare_gpu(g, st, height, cur_epoch, cur_period, cfg.tpb)) { fail++; }
        // a) DAG items against the CPU
        if (!fail) {
            int bad = 0, cnt = 0;
            const uint32_t items512 = e->full_items * 2;
            for (int i = 0; i < 3000 && bad < 5; i++) {
                uint32_t idx = i < 64 ? (uint32_t)i : (i < 128 ? items512 - 1 - (uint32_t)(i - 64) : (uint32_t)(rng() % items512));
                uint32_t gw[16], cw[16];
                if (fm_gpu_read_dag(g, (uint64_t)idx * 16, 16, gw) != 0) { LOG("TEST GPU %d: %s", dev, fm_gpu_last_error(g)); bad++; break; }
                kref::dag_item512(*e, idx, cw);
                if (memcmp(gw, cw, 64)) { bad++; LOG("TEST GPU %d: DAG item %u WRONG", dev, idx); }
                cnt++;
            }
            if (bad) fail++;
            LOG("TEST GPU %d DAG epoch %d: %d items checked against the CPU: %s", dev, e->epoch, cnt, bad ? "FAILED" : "OK");
        }
        // b) full hashes (mix + final) of consecutive nonces, two periods
        for (int k = 0; k < 2 && !fail; k++) {
            const int h = height + 3 * k;
            if (!prepare_gpu(g, st, h, cur_epoch, cur_period, cfg.tpb)) { fail++; break; }
            uint8_t hdr[32];
            for (int i = 0; i < 32; i++) hdr[i] = (uint8_t)rng();
            uint32_t hw[8];
            memcpy(hw, hdr, 32);
            const uint64_t start = rng();
            const uint32_t count = k == 0 ? 2048 : 512;
            std::vector<uint32_t> out((size_t)count * 16);
            auto t1 = Clock::now();
            if (fm_gpu_debug_hashes(g, start, hw, count, out.data()) != 0) { LOG("TEST GPU %d: %s", dev, fm_gpu_last_error(g)); fail++; break; }
            double gs = std::chrono::duration<double>(Clock::now() - t1).count();
            int bad = 0;
            for (uint32_t i = 0; i < count; i++) {
                uint8_t mix[32], fin[32];
                kref::hash(*e, h, hdr, start + i, mix, fin);
                if (memcmp(mix, &out[(size_t)i * 16], 32) || memcmp(fin, &out[(size_t)i * 16 + 8], 32)) {
                    if (bad < 3) LOG("TEST GPU %d: nonce %016llx WRONG (gpu final %s, cpu %s)", dev, (unsigned long long)(start + i),
                                     to_hex((const uint8_t*)&out[(size_t)i * 16 + 8], 32).c_str(), to_hex(fin, 32).c_str());
                    bad++;
                }
            }
            if (bad) fail++;
            LOG("TEST GPU %d height %d (period %d): %u hashes checked against the CPU: %s (debug kernel incl. compile %.2f s)", dev, h, h / 3, count,
                bad ? "FAILED" : "OK", gs);
        }
        // c) candidate filter of the search kernel: ~4 candidates per launch, all checked
        if (!fail) {
            uint8_t hdr[32];
            for (int i = 0; i < 32; i++) hdr[i] = (uint8_t)rng();
            uint32_t hw[8];
            memcpy(hw, hdr, 32);
            fm_gpu_set_grid(g, fm_gpu_sm_count(g) * 32);
            const uint64_t batch = fm_gpu_batch(g);
            const uint64_t t64 = (~0ull / batch) * 4;
            uint64_t start = rng(), hashes = 0;
            int got = 0, bad = 0, lost = 0;
            auto t1 = Clock::now();
            for (int l = 0; l < 25; l++) {
                uint64_t f[FM_GPU_MAX_RESULTS];
                int nf = 0;
                if (fm_gpu_search(g, start, hw, t64, f, FM_GPU_MAX_RESULTS, &nf) != 0) { LOG("TEST GPU %d: %s", dev, fm_gpu_last_error(g)); bad++; break; }
                int m = nf < FM_GPU_MAX_RESULTS ? nf : FM_GPU_MAX_RESULTS;
                lost += nf - m;
                for (int i = 0; i < m; i++) {
                    uint8_t mix[32], fin[32];
                    kref::hash(*e, height + 3, hdr, f[i], mix, fin);
                    if (be64(fin) > t64 || f[i] < start || f[i] >= start + batch) bad++;
                }
                got += nf;
                start += batch;
                hashes += batch;
            }
            double secs = std::chrono::duration<double>(Clock::now() - t1).count();
            double expect = hashes * (double)t64 / 18446744073709551616.0;
            bool ok = bad == 0 && got > expect * 0.6 - 5 && got < expect * 1.4 + 5;
            LOG("TEST GPU %d candidates: %d found for %llu hashes (expected ~%.0f), %d wrong, %d over the list: %s (%.2f MH/s incl. CPU checks)", dev, got,
                (unsigned long long)hashes, expect, bad, lost, ok ? "OK" : "FAILED", hashes / secs / 1e6);
            if (!ok) fail++;
        }
        int regs = 0, lmem = 0;
        fm_gpu_kernel_info(g, &regs, &lmem);
        LOG("TEST GPU %d %s: %s (search kernel %d registers, %d bytes local memory)", dev, fm_gpu_name(g), fail ? "FAILED" : "OK", regs, lmem);
        total_fail += fail;
        fm_gpu_destroy(g);
    }
    LOG("TEST RESULT: %s", total_fail ? "FAILED" : "ALL OK");
    return total_fail ? 1 : 0;
}

static int run_bench(const Config& cfg)
{
    std::vector<int> devs = selected_devices(cfg);
    if (devs.empty()) { LOG("BENCH: no CUDA device"); return 1; }
    auto j = std::make_shared<PoolJob>();
    std::mt19937_64 rng(42);
    for (int i = 0; i < 32; i++) j->header[i] = (uint8_t)rng();
    j->header_hex = to_hex(j->header, 32);
    j->height = cfg.bench_height;
    memset(j->target, 0, 32);                  // impossible target: pure hashing
    j->target64 = 0;
    memset(j->net_target, 0, 32);
    j->job_id = "bench";
    j->seq = 1;
    j->xn_len = 0;
    { std::lock_guard<std::mutex> lk(g_mtx); g_job = j; g_paused = false; g_job_seq = 1; }
    for (size_t i = 0; i < devs.size(); i++) { g_stats.emplace_back(new GpuStat()); g_stats.back()->dev = devs[i]; }
    std::vector<std::thread> th;
    for (size_t i = 0; i < devs.size(); i++) th.emplace_back(gpu_worker, std::cref(cfg), (int)i, devs[i]);
    // wait for the DAGs and the first program
    for (int i = 0; i < 1200 && g_running; i++) {
        bool all = true;
        for (auto& s : g_stats) if (s->hashes == 0) all = false;
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::vector<uint64_t> s0;
    for (auto& s : g_stats) s0.push_back(s->hashes);
    auto t0 = Clock::now();
    for (int i = 0; i < cfg.bench_seconds && g_running; i++) std::this_thread::sleep_for(std::chrono::seconds(1));
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    double total = 0;
    for (size_t i = 0; i < g_stats.size(); i++) {
        double r = (double)(g_stats[i]->hashes - s0[i]) / secs;
        total += r;
        LOG("BENCH GPU %d: %.3f MH/s  (grid %d x %d threads, %d ms/launch) %s", g_stats[i]->dev, r / 1e6, g_stats[i]->grid.load(), cfg.tpb,
            g_stats[i]->launch_ms.load(), g_stats[i]->name.c_str());
    }
    LOG("BENCH TOTAL: %.3f MH/s over %zu GPU(s), height %d (epoch %d), %d threads/block", total / 1e6, g_stats.size(), cfg.bench_height,
        kref::epoch_of(cfg.bench_height), cfg.tpb);
    g_running = false;
    for (auto& t : th) t.join();
    return 0;
}

static std::vector<std::string> expand_args(int argc, char** argv)
{
    std::vector<std::string> out;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--args-file" && i + 1 < argc) {
            FILE* f = fopen(argv[++i], "r");
            if (!f) { fprintf(stderr, "cannot open args file %s\n", argv[i]); continue; }
            char tok[1024];
            while (fscanf(f, "%1023s", tok) == 1) out.push_back(tok);
            fclose(f);
            continue;
        }
        out.push_back(a);
    }
    return out;
}

int main(int argc, char** argv)
{
    Config cfg;
    cfg.exe_dir = exe_directory(argv[0]);
    std::vector<std::string> args = expand_args(argc, argv);
    std::string conf = cfg.exe_dir + "/filominer.conf";
    if (!file_exists(conf)) conf = cfg.exe_dir + "/../filominer.conf";
    for (size_t i = 0; i < args.size(); i++)
        if ((args[i] == "--config" || args[i] == "-c") && i + 1 < args.size()) conf = args[++i];
    bool have_conf = load_config_file(cfg, conf);
    std::vector<std::string> cli_pools;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--config" || a == "-c") { i++; continue; }
        if (a == "--test") { cfg.test = true; continue; }
        if (a == "--nvrtc-check") {   // compiles one period's kernel for sm_61 (no GPU needed)
            auto t0 = Clock::now();
            auto e = kref::get_epoch(kref::epoch_of(cfg.bench_height));
            int r = fm_gpu_precompile(kref::period_of(cfg.bench_height), e->dag_items_2048(), 61, cfg.tpb);
            printf("NVRTC check (sm_61, period %llu): %s %s (%.2f s)\n", (unsigned long long)kref::period_of(cfg.bench_height),
                   r == 0 ? "OK" : "FAILED", r == 0 ? "" : fm_gpu_last_error(nullptr), std::chrono::duration<double>(Clock::now() - t0).count());
            return r == 0 ? 0 : 1;
        }
        if (a == "--bench") {
            cfg.bench = true;
            if (i + 1 < args.size() && args[i + 1][0] != '-') cfg.bench_seconds = atoi(args[++i].c_str());
            continue;
        }
        if (a == "--help" || a == "-h") {
            printf("FiloMiner %s - FiloPow (FPOW) KawPow GPU miner, no dev fee\n"
                   "  filominer [--config file] [--args-file file] [-o url] [-u address.rig] [-p x] [--gpus 0,1|all]\n"
                   "            [--tpb N] [--grid N] [--target-ms N] [--cafile f] [--stats s] [--log file]\n"
                   "            [--test] [--bench [s]] [--bench-height N] [--nvrtc-check]\n",
                   FILOMINER_VERSION);
            return 0;
        }
        if (i + 1 < args.size()) {
            if (a == "-o" || a == "--pool" || a == "--url") { cli_pools.push_back(args[++i]); continue; }
            if (apply_option(cfg, a, args[i + 1])) { i++; continue; }
        }
        fprintf(stderr, "unknown argument %s (see --help)\n", a.c_str());
        return 2;
    }
    if (!cli_pools.empty()) cfg.pools = cli_pools;
    if (cfg.cafile.empty()) {
        const char* cands[] = { "C:\\msys64\\mingw64\\etc\\ssl\\certs\\ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt" };
        std::string local = cfg.exe_dir + "/ca-bundle.crt";
        if (file_exists(local)) cfg.cafile = local;
        else for (auto c : cands) if (file_exists(c)) { cfg.cafile = c; break; }
    }
    if (!cfg.logfile.empty()) g_logf = fopen(cfg.logfile.c_str(), "a");
    if (cfg.stats < 5) cfg.stats = 5;
    if (cfg.tpb < 32 || cfg.tpb > 1024 || cfg.tpb % 32) cfg.tpb = 256;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    LOG("FiloMiner %s (FiloPow / FPOW, KawPow) - no dev fee%s", FILOMINER_VERSION, have_conf ? "" : " - no config file");

    if (cfg.test) { deadline_watchdog(600, "TEST"); return run_tests(cfg); }
    if (cfg.bench) { deadline_watchdog(cfg.bench_seconds + 240, "BENCH"); return run_bench(cfg); }

    if (cfg.pools.empty() || cfg.user.empty()) { LOG("missing pool url or user (FPOW address.rig), see filominer.conf"); return 2; }
    std::vector<int> devs = selected_devices(cfg);
    if (devs.empty()) { LOG("no CUDA device selected"); return 1; }

    for (size_t i = 0; i < devs.size(); i++) { g_stats.emplace_back(new GpuStat()); g_stats.back()->dev = devs[i]; }
    std::vector<std::thread> th;
    for (size_t i = 0; i < devs.size(); i++) th.emplace_back(gpu_worker, std::cref(cfg), (int)i, devs[i]);
    std::thread ver1(verify_worker), ver2(verify_worker);
    std::thread net(pool_worker, std::cref(cfg));

    std::vector<uint64_t> last(g_stats.size(), 0);
    auto t_last = Clock::now();
    auto t_start = Clock::now();
    while (g_running) {
        for (int i = 0; i < cfg.stats * 10 && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (i % 10 != 9) continue;
            long long t = now_ms();
            for (size_t k = 0; k < g_stats.size(); k++) {
                if (!g_stats[k]->alive || g_stats[k]->busy) continue;
                long long idle = t - g_stats[k]->beat_ms.load();
                if (idle > FM_STUCK_MS) {
                    LOG("GPU %d stuck for %lld s (launch never returned, unstable clocks?): exiting with code 4 for an automatic restart",
                        g_stats[k]->dev, idle / 1000);
                    fflush(nullptr);
                    std::_Exit(4);
                }
            }
        }
        if (!g_running) break;
        double secs = std::chrono::duration<double>(Clock::now() - t_last).count();
        t_last = Clock::now();
        double total = 0;
        std::string per;
        for (size_t i = 0; i < g_stats.size(); i++) {
            uint64_t s = g_stats[i]->hashes;
            double r = (double)(s - last[i]) / secs;
            last[i] = s;
            total += r;
            char b[64];
            snprintf(b, sizeof b, "%sGPU%d %.2f", i ? " | " : "", g_stats[i]->dev, r / 1e6);
            per += b;
        }
        bool paused;
        double work;
        { std::lock_guard<std::mutex> lk(g_mtx); paused = g_paused; work = g_accepted_work; }
        double up = std::chrono::duration<double>(Clock::now() - t_start).count();
        std::string bad;
        for (size_t i = 0; i < g_stats.size(); i++) {
            uint64_t b = g_stats[i]->bad;
            if (b) { char x[48]; snprintf(x, sizeof x, "%sGPU%d %llu", bad.empty() ? "" : ", ", g_stats[i]->dev, (unsigned long long)b); bad += x; }
        }
        LOG("%.2f MH/s total (%s) | shares %llu/%llu, stale %llu | pool-side %.2f MH/s | blocks %llu%s%s%s", total / 1e6, per.c_str(),
            (unsigned long long)g_accepted.load(), (unsigned long long)(g_accepted.load() + g_rejected.load()),
            (unsigned long long)g_stale_rej.load(), up > 0 ? work / up / 1e6 : 0.0, (unsigned long long)g_blocks.load(),
            paused ? " | waiting for work" : "", bad.empty() ? "" : " | GPU ERRORS: ", bad.c_str());
    }
    if (g_gpu_failed) {
        LOG("GPU %d failed (CUDA error): exiting with code 3 for an automatic restart", g_gpu_failed.load() - 1);
        fflush(nullptr);
        std::_Exit(3);
    }
    LOG("stopping...");
    g_cand_cv.notify_all();
    for (auto& t : th) t.join();
    ver1.join();
    ver2.join();
    net.join();
    return 0;
}
