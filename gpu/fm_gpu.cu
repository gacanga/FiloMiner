// fm_gpu.cu - CUDA KawPow engine (fmgpu.dll) of FiloMiner.  Apache-2.0.
// Target: NVIDIA Pascal (P104-100, sm_61); works on later GPUs too.
//
// - DAG: built on the GPU from the light cache (fm_dag.h, 512 parents per item).
// - Search kernel: gpu/kawpow_kernel.cuh + the period's random program
//   (progpow_gen.h), compiled by NVRTC to a cubin for the device's architecture,
//   cached per (period, DAG size, architecture, block size, options), loaded with
//   the driver API.
#include <cuda.h>
#include <cuda_runtime.h>
#include <nvrtc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>

#include "fm_dag.h"
#include "progpow_gen.h"
#include "kawpow_kernel_src.h"
#include "fm_gpu.h"

struct FmResults {
    uint32_t count;
    uint32_t pad;
    uint64_t nonce[FM_GPU_MAX_RESULTS];
};
struct FmHeader { uint32_t w[8]; };

struct FmGpu {
    int dev = -1;
    std::string name, err;
    int cc = 0, sms = 0;
    uint64_t mem = 0;
    int block = 256, grid = 0;
    cudaStream_t stream = nullptr;
    CUcontext cu = nullptr;
    uint4* d_dag = nullptr;
    uint64_t dag_bytes = 0;
    int epoch = -1;
    uint32_t dag_items = 0;           // 2048-bit items
    FmResults* d_res = nullptr;
    FmResults* h_res = nullptr;       // pinned
    uint32_t* d_dbg = nullptr;
    size_t dbg_words = 0;
    uint64_t period = ~0ull;
    CUmodule mod = nullptr;
    CUfunction fn = nullptr;
    uint64_t dbg_period = ~0ull;
    CUmodule mod_dbg = nullptr;
    CUfunction fn_dbg = nullptr;
};

static thread_local std::string t_err;
static std::mutex g_cmtx;
static std::map<std::string, std::vector<char>> g_cubins;
static std::string g_extra_opts;   // FM_NVRTC_OPTIONS environment variable

static bool cuda_ok(FmGpu* g, cudaError_t e, const char* what)
{
    if (e == cudaSuccess) return true;
    char b[512];
    snprintf(b, sizeof b, "%s: %s (%d)", what, cudaGetErrorString(e), (int)e);
    if (g) g->err = b; else t_err = b;
    return false;
}
static bool cu_ok(FmGpu* g, CUresult r, const char* what)
{
    if (r == CUDA_SUCCESS) return true;
    const char* s = nullptr;
    cuGetErrorString(r, &s);
    char b[512];
    snprintf(b, sizeof b, "%s: %s (%d)", what, s ? s : "?", (int)r);
    if (g) g->err = b; else t_err = b;
    return false;
}

// ---------------------------------------------------------------------------
// DAG generation
// ---------------------------------------------------------------------------
__global__ void fm_dag_kernel(uint4* __restrict__ dag, const uint4* __restrict__ light, uint32_t light_items, uint32_t start, uint32_t end)
{
    const uint32_t idx = start + blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= end) return;
    uint32_t mix[16];
    {
        const uint4* c = light + (uint64_t)(idx % light_items) * 4;
#pragma unroll
        for (int k = 0; k < 4; k++) {
            const uint4 v = __ldg(c + k);
            mix[4 * k] = v.x; mix[4 * k + 1] = v.y; mix[4 * k + 2] = v.z; mix[4 * k + 3] = v.w;
        }
    }
    mix[0] ^= idx;
    fmdag::keccak512_64(mix);
#pragma unroll 1
    for (uint32_t j0 = 0; j0 < fmdag::PARENTS; j0 += 16) {
#pragma unroll
        for (uint32_t jj = 0; jj < 16; jj++) {
            const uint32_t j = j0 + jj;
            const uint32_t t = ((idx ^ j) * fmdag::FNV_PRIME) ^ mix[jj];
            const uint4* p = light + (uint64_t)(t % light_items) * 4;
#pragma unroll
            for (int k = 0; k < 4; k++) {
                const uint4 v = __ldg(p + k);
                mix[4 * k] = (mix[4 * k] * fmdag::FNV_PRIME) ^ v.x;
                mix[4 * k + 1] = (mix[4 * k + 1] * fmdag::FNV_PRIME) ^ v.y;
                mix[4 * k + 2] = (mix[4 * k + 2] * fmdag::FNV_PRIME) ^ v.z;
                mix[4 * k + 3] = (mix[4 * k + 3] * fmdag::FNV_PRIME) ^ v.w;
            }
        }
    }
    fmdag::keccak512_64(mix);
    uint4* o = dag + (uint64_t)idx * 4;
#pragma unroll
    for (int k = 0; k < 4; k++) o[k] = make_uint4(mix[4 * k], mix[4 * k + 1], mix[4 * k + 2], mix[4 * k + 3]);
}

// ---------------------------------------------------------------------------
// NVRTC compilation (cached)
// ---------------------------------------------------------------------------
static std::string cache_key(uint64_t period, uint32_t items, int cc, int block, bool dbg)
{
    char b[128];
    snprintf(b, sizeof b, "%llu:%u:%d:%d:%d:", (unsigned long long)period, items, cc, block, dbg ? 1 : 0);
    return std::string(b) + g_extra_opts;
}

static std::string kernel_source(uint64_t period, uint32_t items, int block, bool dbg)
{
    std::string s;
    s += "#define FM_DAG_ITEMS " + std::to_string(items) + "u\n";
    s += "#define FM_BLOCK " + std::to_string(block) + "\n";
    s += "#define FM_MAX_RESULTS " + std::to_string(FM_GPU_MAX_RESULTS) + "\n";
    if (dbg) s += "#define FM_DEBUG 1\n";
    s += fmgen::emit_macros(fmgen::make_program(period));
    for (int i = 0; fm_kernel_parts[i]; i++) s += fm_kernel_parts[i];
    return s;
}

// returns the cubin (from the cache or freshly compiled); empty + err on failure
static std::vector<char> get_cubin(uint64_t period, uint32_t items, int cc, int block, bool dbg, std::string& err, bool* compiled, double* secs)
{
    std::lock_guard<std::mutex> lk(g_cmtx);
    if (compiled) *compiled = false;
    if (secs) *secs = 0;
    const std::string key = cache_key(period, items, cc, block, dbg);
    auto it = g_cubins.find(key);
    if (it != g_cubins.end()) return it->second;
    auto t0 = std::chrono::steady_clock::now();
    const std::string src = kernel_source(period, items, block, dbg);
    nvrtcProgram prog;
    nvrtcResult r = nvrtcCreateProgram(&prog, src.c_str(), "kawpow_search.cu", 0, nullptr, nullptr);
    if (r != NVRTC_SUCCESS) { err = std::string("nvrtcCreateProgram: ") + nvrtcGetErrorString(r); return {}; }
    char arch[64];
    snprintf(arch, sizeof arch, "--gpu-architecture=sm_%d", cc);
    std::vector<std::string> optv = { arch, "-std=c++17" };
    {   // extra options separated by spaces (experiments: --maxrregcount=72 ...)
        size_t p = 0;
        while (p < g_extra_opts.size()) {
            size_t q = g_extra_opts.find(' ', p);
            if (q == std::string::npos) q = g_extra_opts.size();
            if (q > p) optv.push_back(g_extra_opts.substr(p, q - p));
            p = q + 1;
        }
    }
    std::vector<const char*> opts;
    for (auto& o : optv) opts.push_back(o.c_str());
    r = nvrtcCompileProgram(prog, (int)opts.size(), opts.data());
    if (r != NVRTC_SUCCESS) {
        size_t n = 0;
        nvrtcGetProgramLogSize(prog, &n);
        std::string log(n, '\0');
        if (n) nvrtcGetProgramLog(prog, &log[0]);
        err = std::string("NVRTC compile failed (") + nvrtcGetErrorString(r) + "): " + log.substr(0, 1500);
        nvrtcDestroyProgram(&prog);
        return {};
    }
    size_t n = 0;
    r = nvrtcGetCUBINSize(prog, &n);
    std::vector<char> bin(n);
    if (r == NVRTC_SUCCESS && n) r = nvrtcGetCUBIN(prog, bin.data());
    nvrtcDestroyProgram(&prog);
    if (r != NVRTC_SUCCESS || !n) { err = std::string("nvrtcGetCUBIN: ") + nvrtcGetErrorString(r); return {}; }
    // keep the cache small: this period, the next one and the debug kernels
    if (g_cubins.size() > 16) g_cubins.clear();
    g_cubins[key] = bin;
    if (const char* dd = getenv("FM_DUMP_DIR")) {   // FM_DUMP_DIR=dir: keep source + cubin for inspection
        char fn[1024];
        snprintf(fn, sizeof fn, "%s/kawpow_p%llu_sm%d%s.cu", dd, (unsigned long long)period, cc, dbg ? "_dbg" : "");
        if (FILE* f = fopen(fn, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
        snprintf(fn, sizeof fn, "%s/kawpow_p%llu_sm%d%s.cubin", dd, (unsigned long long)period, cc, dbg ? "_dbg" : "");
        if (FILE* f = fopen(fn, "wb")) { fwrite(bin.data(), 1, bin.size(), f); fclose(f); }
    }
    if (compiled) *compiled = true;
    if (secs) *secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return bin;
}

static bool load_module(FmGpu* g, uint64_t period, bool dbg, int* compiled, double* secs)
{
    std::string err;
    bool comp = false;
    double s = 0;
    std::vector<char> bin = get_cubin(period, g->dag_items, g->cc, g->block, dbg, err, &comp, &s);
    if (compiled) *compiled = comp ? 1 : 0;
    if (secs) *secs = s;
    if (bin.empty()) { g->err = err; return false; }
    if (!cu_ok(g, cuCtxSetCurrent(g->cu), "cuCtxSetCurrent")) return false;
    CUmodule& m = dbg ? g->mod_dbg : g->mod;
    CUfunction& f = dbg ? g->fn_dbg : g->fn;
    if (m) { cuModuleUnload(m); m = nullptr; f = nullptr; }
    if (!cu_ok(g, cuModuleLoadData(&m, bin.data()), "cuModuleLoadData")) { m = nullptr; return false; }
    if (!cu_ok(g, cuModuleGetFunction(&f, m, "fm_search"), "cuModuleGetFunction")) return false;
    if (dbg) g->dbg_period = period; else g->period = period;
    return true;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
extern "C" {

int fm_gpu_device_count(void)
{
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) return 0;
    return n;
}

void* fm_gpu_create(int device, int block, int grid)
{
    const char* eo = getenv("FM_NVRTC_OPTIONS");
    if (eo) { std::lock_guard<std::mutex> lk(g_cmtx); g_extra_opts = eo; }
    FmGpu* g = new FmGpu();
    g->dev = device;
    g->block = (block >= 32 && block <= 1024 && block % 32 == 0) ? block : 256;
    cudaDeviceProp p;
    if (!cuda_ok(nullptr, cudaSetDevice(device), "cudaSetDevice") || !cuda_ok(nullptr, cudaGetDeviceProperties(&p, device), "cudaGetDeviceProperties")) {
        delete g;
        return nullptr;
    }
    cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);   // no busy-waiting CPU thread
    if (!cuda_ok(nullptr, cudaFree(0), "context init") || !cu_ok(nullptr, cuCtxGetCurrent(&g->cu), "cuCtxGetCurrent") ||
        !cuda_ok(nullptr, cudaStreamCreateWithFlags(&g->stream, cudaStreamNonBlocking), "cudaStreamCreate") ||
        !cuda_ok(nullptr, cudaMalloc(&g->d_res, sizeof(FmResults)), "cudaMalloc results") ||
        !cuda_ok(nullptr, cudaMallocHost(&g->h_res, sizeof(FmResults)), "cudaMallocHost")) {
        delete g;
        return nullptr;
    }
    g->name = p.name;
    g->cc = p.major * 10 + p.minor;
    g->sms = p.multiProcessorCount;
    g->mem = p.totalGlobalMem;
    g->grid = grid > 0 ? grid : g->sms * 128;
    return g;
}

void fm_gpu_destroy(void* ctx)
{
    FmGpu* g = (FmGpu*)ctx;
    if (!g) return;
    cudaSetDevice(g->dev);
    if (g->mod) cuModuleUnload(g->mod);
    if (g->mod_dbg) cuModuleUnload(g->mod_dbg);
    if (g->d_dag) cudaFree(g->d_dag);
    if (g->d_dbg) cudaFree(g->d_dbg);
    if (g->d_res) cudaFree(g->d_res);
    if (g->h_res) cudaFreeHost(g->h_res);
    if (g->stream) cudaStreamDestroy(g->stream);
    delete g;
}

const char* fm_gpu_name(void* ctx) { return ctx ? ((FmGpu*)ctx)->name.c_str() : ""; }
const char* fm_gpu_last_error(void* ctx) { return ctx ? ((FmGpu*)ctx)->err.c_str() : t_err.c_str(); }
int fm_gpu_compute_capability(void* ctx) { return ctx ? ((FmGpu*)ctx)->cc : 0; }
int fm_gpu_sm_count(void* ctx) { return ctx ? ((FmGpu*)ctx)->sms : 0; }
uint64_t fm_gpu_memory(void* ctx) { return ctx ? ((FmGpu*)ctx)->mem : 0; }
uint32_t fm_gpu_batch(void* ctx) { return ctx ? (uint32_t)((FmGpu*)ctx)->grid * (uint32_t)((FmGpu*)ctx)->block : 0; }

int fm_gpu_set_grid(void* ctx, int grid)
{
    FmGpu* g = (FmGpu*)ctx;
    if (grid < 1) grid = 1;
    if (grid > 1 << 20) grid = 1 << 20;
    g->grid = grid;
    return 0;
}

int fm_gpu_build_dag(void* ctx, int epoch, const uint32_t* light_cache, uint32_t light_items, uint32_t full_items, double* seconds)
{
    FmGpu* g = (FmGpu*)ctx;
    if (seconds) *seconds = 0;
    if (g->epoch == epoch && g->d_dag) return 0;
    auto t0 = std::chrono::steady_clock::now();
    if (!cuda_ok(g, cudaSetDevice(g->dev), "cudaSetDevice")) return -1;
    if (g->mod) { cuModuleUnload(g->mod); g->mod = nullptr; g->fn = nullptr; g->period = ~0ull; }
    if (g->mod_dbg) { cuModuleUnload(g->mod_dbg); g->mod_dbg = nullptr; g->fn_dbg = nullptr; g->dbg_period = ~0ull; }
    if (g->d_dag) { cudaFree(g->d_dag); g->d_dag = nullptr; }
    g->epoch = -1;
    const uint64_t bytes = (uint64_t)full_items * 128;
    if (!cuda_ok(g, cudaMalloc(&g->d_dag, bytes), "cudaMalloc DAG")) { g->d_dag = nullptr; return -1; }
    uint4* d_light = nullptr;
    const uint64_t lbytes = (uint64_t)light_items * 64;
    if (!cuda_ok(g, cudaMalloc(&d_light, lbytes), "cudaMalloc light cache")) return -1;
    if (!cuda_ok(g, cudaMemcpy(d_light, light_cache, lbytes, cudaMemcpyHostToDevice), "copy light cache")) { cudaFree(d_light); return -1; }
    const uint32_t items512 = full_items * 2;
    const uint32_t chunk = 1u << 18, tpb = 128;
    for (uint32_t s = 0; s < items512; s += chunk) {
        const uint32_t e = s + chunk < items512 ? s + chunk : items512;
        fm_dag_kernel<<<(e - s + tpb - 1) / tpb, tpb, 0, g->stream>>>(g->d_dag, d_light, light_items, s, e);
        if (!cuda_ok(g, cudaGetLastError(), "DAG kernel launch") || !cuda_ok(g, cudaStreamSynchronize(g->stream), "DAG kernel")) {
            cudaFree(d_light);
            return -1;
        }
    }
    cudaFree(d_light);
    g->epoch = epoch;
    g->dag_bytes = bytes;
    g->dag_items = full_items / 2;
    if (seconds) *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return 0;
}

int fm_gpu_read_dag(void* ctx, uint64_t offset, uint32_t count, uint32_t* out)
{
    FmGpu* g = (FmGpu*)ctx;
    if (!g->d_dag || (offset + count) * 4 > g->dag_bytes) { g->err = "read_dag: out of range"; return -1; }
    if (!cuda_ok(g, cudaSetDevice(g->dev), "cudaSetDevice")) return -1;
    return cuda_ok(g, cudaMemcpy(out, (const uint32_t*)g->d_dag + offset, (size_t)count * 4, cudaMemcpyDeviceToHost), "read DAG") ? 0 : -1;
}

int fm_gpu_set_period(void* ctx, uint64_t period, int* compiled, double* seconds)
{
    FmGpu* g = (FmGpu*)ctx;
    if (compiled) *compiled = 0;
    if (seconds) *seconds = 0;
    if (!g->d_dag) { g->err = "set_period: no DAG"; return -1; }
    if (g->fn && g->period == period) return 0;
    return load_module(g, period, false, compiled, seconds) ? 0 : -1;
}

int fm_gpu_precompile(uint64_t period, uint32_t dag_items_2048, int cc, int block)
{
    std::string err;
    std::vector<char> b = get_cubin(period, dag_items_2048, cc, block, false, err, nullptr, nullptr);
    if (b.empty()) { t_err = err; return -1; }
    return 0;
}

int fm_gpu_kernel_info(void* ctx, int* regs, int* local_bytes)
{
    FmGpu* g = (FmGpu*)ctx;
    if (!g->fn) return -1;
    cuCtxSetCurrent(g->cu);
    int r = 0, l = 0;
    cuFuncGetAttribute(&r, CU_FUNC_ATTRIBUTE_NUM_REGS, g->fn);
    cuFuncGetAttribute(&l, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES, g->fn);
    if (regs) *regs = r;
    if (local_bytes) *local_bytes = l;
    return 0;
}

static int launch(FmGpu* g, CUfunction f, uint64_t start, const uint32_t* header, uint64_t target, uint32_t grid, uint32_t* d_dbg)
{
    FmHeader h;
    memcpy(h.w, header, 32);
    CUdeviceptr dag = (CUdeviceptr)g->d_dag, res = (CUdeviceptr)g->d_res, dbg = (CUdeviceptr)d_dbg;
    void* args[] = { &start, &h, &target, &dag, &res, &dbg };
    if (!cuda_ok(g, cudaMemsetAsync(g->d_res, 0, 8, g->stream), "reset results")) return -1;
    if (!cu_ok(g, cuLaunchKernel(f, grid, 1, 1, (unsigned)g->block, 1, 1, 0, (CUstream)g->stream, args, nullptr), "cuLaunchKernel")) return -1;
    if (!cuda_ok(g, cudaMemcpyAsync(g->h_res, g->d_res, sizeof(FmResults), cudaMemcpyDeviceToHost, g->stream), "copy results")) return -1;
    if (!cuda_ok(g, cudaStreamSynchronize(g->stream), "search kernel")) return -1;
    return 0;
}

int fm_gpu_search(void* ctx, uint64_t start_nonce, const uint32_t* header, uint64_t target64, uint64_t* found, int max_found, int* n_found)
{
    FmGpu* g = (FmGpu*)ctx;
    *n_found = 0;
    if (!g->fn) { g->err = "search: no program (fm_gpu_set_period)"; return -1; }
    if (!cuda_ok(g, cudaSetDevice(g->dev), "cudaSetDevice")) return -1;
    if (launch(g, g->fn, start_nonce, header, target64, (uint32_t)g->grid, nullptr)) return -1;
    const uint32_t n = g->h_res->count;
    *n_found = (int)n;
    const uint32_t m = n < FM_GPU_MAX_RESULTS ? n : FM_GPU_MAX_RESULTS;
    for (uint32_t i = 0; i < m && (int)i < max_found; i++) found[i] = g->h_res->nonce[i];
    return 0;
}

int fm_gpu_debug_hashes(void* ctx, uint64_t start_nonce, const uint32_t* header, uint32_t count, uint32_t* out)
{
    FmGpu* g = (FmGpu*)ctx;
    if (!cuda_ok(g, cudaSetDevice(g->dev), "cudaSetDevice")) return -1;
    if (g->period == ~0ull) { g->err = "debug: set a period first"; return -1; }
    if (!g->fn_dbg || g->dbg_period != g->period) {
        if (!load_module(g, g->period, true, nullptr, nullptr)) return -1;
    }
    const uint32_t grid = (count + (uint32_t)g->block - 1) / (uint32_t)g->block;
    const size_t words = (size_t)grid * (size_t)g->block * 16;
    if (words > g->dbg_words) {
        if (g->d_dbg) cudaFree(g->d_dbg);
        g->d_dbg = nullptr;
        g->dbg_words = 0;
        if (!cuda_ok(g, cudaMalloc(&g->d_dbg, words * 4), "cudaMalloc debug")) return -1;
        g->dbg_words = words;
    }
    if (launch(g, g->fn_dbg, start_nonce, header, 0, grid, g->d_dbg)) return -1;
    return cuda_ok(g, cudaMemcpy(out, g->d_dbg, (size_t)count * 64, cudaMemcpyDeviceToHost), "copy debug") ? 0 : -1;
}

}  // extern "C"
