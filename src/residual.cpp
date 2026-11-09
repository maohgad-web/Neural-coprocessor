// MGPU Bridge - R316 residual pass (private test build, off by default). See
// residual.hpp for what it is for and the contract it keeps. Everything here
// runs on the bridge thread, inside the stream's command list.
//
// The two kernels are adapted from xenmods' DLSSNR-Cost-Scaler shaders.hlsl
// (MIT License, Copyright (c) 2026 xen - THIRD_PARTY.md). What changed and why:
//   - the depth taps of the silhouette guard are scaled by depth/native, so a
//     depth buffer of any size can be read (render-size depth, R312). With a
//     native depth the arithmetic is the original's.
//   - the skip-frame branches are gone: this add-on evaluates every frame it
//     takes, there is no "stale delta" case here.
//   - constants are named for what they are here.
// The residual math, the chroma/luma control, the HDR guard and RCAS are the
// original's.

#include "residual.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"
#include "../ext/ngx/nvsdk_ngx.h"

namespace mgpu::residual
{
namespace
{
const char kHLSL[] =
    "cbuffer C : register(b0)\n"
    "{\n"
    "  uint  gNativeWidth;  uint  gNativeHeight;\n"
    "  uint  gDepthWidth;   uint  gDepthHeight;\n"   // downsample: destination w/h
    "  float gGain;         float gSharpness;\n"
    "  uint  gMode;         float gColorStrength;\n"
    "  uint  gHasDepth;     uint  gPad;\n"
    "};\n"
    "Texture2D<float4>   gT0 : register(t0);\n"
    "Texture2D<float4>   gT1 : register(t1);\n"
    "Texture2D<float4>   gT2 : register(t2);\n"
    "Texture2D<float>    gT3 : register(t3);\n"
    "RWTexture2D<float4> gU0 : register(u0);\n"
    "SamplerState gLinear : register(s0);\n"
    "static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);\n"
    "groupshared float3 s_tile[10][10];\n"
    "\n"
    // ---- downsample: anchor (t0, native) -> small_in (u0, work) ----
    "[numthreads(8, 8, 1)]\n"
    "void CS_Down(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  if (id.x >= gDepthWidth || id.y >= gDepthHeight) return;\n"
    "  float2 uv = (float2(id.xy) + 0.5) / float2(gDepthWidth, gDepthHeight);\n"
    "  gU0[id.xy] = gT0.SampleLevel(gLinear, uv, 0);\n"
    "}\n"
    "\n"
    // ---- resolve: anchor (t2) + up(small_out (t1) - small_in (t0)) -> dst (u0) ----
    "[numthreads(8, 8, 1)]\n"
    "void CS_Resolve(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)\n"
    "{\n"
    "  int2 baseCoord = int2(gid.xy * 8) - 1;\n"
    "  int2 maxCoord = int2((int)gNativeWidth - 1, (int)gNativeHeight - 1);\n"
    "  uint lt = tid.y * 8 + tid.x;\n"
    "  int2 c0 = clamp(baseCoord + int2((int)(lt % 10), (int)(lt / 10)), int2(0, 0), maxCoord);\n"
    "  s_tile[lt / 10][lt % 10] = gT2.Load(int3(c0, 0)).rgb;\n"
    "  if (lt < 36)\n"
    "  {\n"
    "    uint i1 = lt + 64;\n"
    "    int2 c1 = clamp(baseCoord + int2((int)(i1 % 10), (int)(i1 / 10)), int2(0, 0), maxCoord);\n"
    "    s_tile[i1 / 10][i1 % 10] = gT2.Load(int3(c1, 0)).rgb;\n"
    "  }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (id.x >= gNativeWidth || id.y >= gNativeHeight) return;\n"
    "  float2 uv = (float2(id.xy) + 0.5) / float2(gNativeWidth, gNativeHeight);\n"
    "  float3 original = s_tile[tid.y + 1][tid.x + 1];\n"
    "  float nativeAlpha = gT2.Load(int3(id.xy, 0)).a;\n"
    "\n"
    "  if (gMode == 0)\n"
    "  {\n"
    "    float3 result = gT1.SampleLevel(gLinear, uv, 0).rgb;\n"
    "    if (gGain < 0.999) result = lerp(original, result, saturate(gGain));\n"
    "    if (gSharpness > 0.001)\n"
    "    {\n"
    "      float2 px = float2(1.0 / (float)gNativeWidth, 1.0 / (float)gNativeHeight);\n"
    "      float3 cE = gT1.SampleLevel(gLinear, uv + float2( px.x, 0), 0).rgb;\n"
    "      float3 cW = gT1.SampleLevel(gLinear, uv + float2(-px.x, 0), 0).rgb;\n"
    "      float3 cS = gT1.SampleLevel(gLinear, uv + float2(0,  px.y), 0).rgb;\n"
    "      float3 cN = gT1.SampleLevel(gLinear, uv + float2(0, -px.y), 0).rgb;\n"
    "      float lE = dot(cE, kLuma), lW = dot(cW, kLuma), lS = dot(cS, kLuma), lN = dot(cN, kLuma);\n"
    "      float lM = dot(result, kLuma);\n"
    "      float minL = min(lM, min(min(lE, lW), min(lS, lN)));\n"
    "      float maxL = max(lM, max(max(lE, lW), max(lS, lN)));\n"
    "      float range = maxL - minL;\n"
    "      if (range > 1e-5)\n"
    "      {\n"
    "        float3 crossAvg = (cE + cW + cS + cN) * 0.25;\n"
    "        float3 highFreq = result - crossAvg;\n"
    "        float adaptiveScale = saturate(1.0 - range / (maxL + 1e-4));\n"
    "        float w = saturate(gSharpness) * (0.2 + 0.8 * adaptiveScale);\n"
    "        result = max(result + highFreq * w, 0.0);\n"
    "      }\n"
    "    }\n"
    "    gU0[id.xy] = float4(max(result, 0.0), nativeAlpha);\n"
    "    return;\n"
    "  }\n"
    "\n"
    "  float3 smallInput  = gT0.SampleLevel(gLinear, uv, 0).rgb;\n"
    "  float3 smallOutput = gT1.SampleLevel(gLinear, uv, 0).rgb;\n"
    "  float3 edit = smallOutput - smallInput;\n"
    "  float editLuma = dot(edit, kLuma);\n"
    "  float origLuma = dot(max(original, 0.0), kLuma);\n"
    "  float3 lumaDir = lerp(float3(1.0, 1.0, 1.0), original / (origLuma + 1e-5), saturate(origLuma * 50.0));\n"
    "  float3 lumaEdit = lumaDir * editLuma;\n"
    "  lumaEdit = clamp(lumaEdit, -original * 0.95, max(original * 2.5, 2.0));\n"
    "  float3 controlledEdit = lerp(lumaEdit, edit, saturate(gColorStrength));\n"
    "\n"
    // silhouette guard, taps scaled to the depth buffer's own extent
    "  if (gHasDepth != 0)\n"
    "  {\n"
    "    int2 dmax = int2((int)gDepthWidth - 1, (int)gDepthHeight - 1);\n"
    "    float2 dsc = float2(gDepthWidth, gDepthHeight) / float2(gNativeWidth, gNativeHeight);\n"
    "    int2 dc = clamp(int2(floor((float2(id.xy) + 0.5) * dsc)), int2(0, 0), dmax);\n"
    "    float d0 = gT3.Load(int3(dc, 0)).r;\n"
    "    float dE = gT3.Load(int3(min(dc.x + 1, dmax.x), dc.y, 0)).r;\n"
    "    float dW = gT3.Load(int3(max(dc.x - 1, 0), dc.y, 0)).r;\n"
    "    float dS = gT3.Load(int3(dc.x, min(dc.y + 1, dmax.y), 0)).r;\n"
    "    float dN = gT3.Load(int3(dc.x, max(dc.y - 1, 0), 0)).r;\n"
    "    float minD = min(d0, min(min(dE, dW), min(dS, dN)));\n"
    "    float maxD = max(d0, max(max(dE, dW), max(dS, dN)));\n"
    "    float depthRange = (maxD - minD) / (maxD + 1e-4);\n"
    "    if (depthRange > 0.02)\n"
    "    {\n"
    "      float edgeWeight = saturate(1.0 - (depthRange - 0.02) * 20.0);\n"
    "      controlledEdit *= lerp(0.25, 1.0, edgeWeight);\n"
    "    }\n"
    "  }\n"
    "\n"
    "  float3 scaledEdit = controlledEdit * gGain;\n"
    "  float3 result = max(original + scaledEdit, 0.0);\n"
    "\n"
    "  {\n"
    "    float inLuma  = dot(max(smallInput, 0.0), kLuma);\n"
    "    float outLuma = dot(max(smallOutput, 0.0), kLuma);\n"
    "    const float kFloor = 1.0 / 512.0;\n"
    "    float lumaRatio = (outLuma + kFloor) / (inLuma + kFloor);\n"
    "    float resLuma = dot(result, kLuma);\n"
    "    if (resLuma > 1e-5 && inLuma > 1e-5)\n"
    "    {\n"
    "      float targetLuma = origLuma * lumaRatio;\n"
    "      float maxAllowedLuma = max(origLuma * 2.5, targetLuma * 1.5 + 0.1);\n"
    "      if (resLuma > maxAllowedLuma) result *= (maxAllowedLuma / resLuma);\n"
    "    }\n"
    "  }\n"
    "\n"
    "  if (gSharpness > 0.001)\n"
    "  {\n"
    "    float3 cE = s_tile[tid.y + 1][tid.x + 2];\n"
    "    float3 cW = s_tile[tid.y + 1][tid.x + 0];\n"
    "    float3 cS = s_tile[tid.y + 2][tid.x + 1];\n"
    "    float3 cN = s_tile[tid.y + 0][tid.x + 1];\n"
    "    float lE = dot(cE, kLuma), lW = dot(cW, kLuma), lS = dot(cS, kLuma), lN = dot(cN, kLuma);\n"
    "    float lM = dot(max(original, 0.0), kLuma);\n"
    "    float minL = min(lM, min(min(lE, lW), min(lS, lN)));\n"
    "    float maxL = max(lM, max(max(lE, lW), max(lS, lN)));\n"
    "    float range = maxL - minL;\n"
    "    if (range > 1e-5)\n"
    "    {\n"
    "      float3 crossAvg = (cE + cW + cS + cN) * 0.25;\n"
    "      float3 highFreq = original - crossAvg;\n"
    "      float adaptiveScale = saturate(1.0 - range / (maxL + 1e-4));\n"
    "      float w = saturate(gSharpness) * (0.2 + 0.8 * adaptiveScale);\n"
    "      result = max(result + highFreq * w, 0.0);\n"
    "    }\n"
    "  }\n"
    "  gU0[id.xy] = float4(result, nativeAlpha);\n"
    "}\n";

typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                         ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob **, ID3DBlob **);

// R332: the module is PER PASS. Each pass that carries the residual owns its own
// scratch pair, work extent, tracked states and counters; the kernels, the root
// signature and the descriptor heap are shared (one heap, one slot range per
// pass per parity). With NRS absent exactly one pass carries it (ResidualPass),
// as R316 did; with NRS=1 pass 1 carries it and pass 2 may too (Pass2=nrs).
const unsigned MAX_SETS = 4u;   // == stream_state::MAX_PASSES, by contract (checked at arm). R350: capacity 4;
                                // a set is allocated only for a pass that carries, so 1 / 2 cost what they did.

struct pass_set
{
    bool on = false;              // this pass carries the residual (from the keys)
    bool active = false;          // on AND built; cleared by three failed evaluates
    unsigned ww = 0, wh = 0;      // work extent
    ID3D12Resource *sin = nullptr;    // small input  (work extent)
    ID3D12Resource *sout = nullptr;   // small output (work extent)
    // tracked states of the scratch pair - never guessed
    D3D12_RESOURCE_STATES sin_state  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES sout_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // per-evaluate memory (set in before, used in after)
    float mvx0 = 1.0f, mvy0 = 1.0f;
    unsigned long long evals = 0ull;
    unsigned fails = 0u;
    bool said_bind = false;
    bool said_depth = false;
    bool from_render = false;
};

std::atomic<bool> g_active{false};   // any pass active (one relaxed load for the callers)
pass_set g_set[MAX_SETS];

// keys (shared by every carrying pass: one work-extent recipe, one look)
unsigned g_nrs = 0u;          // R332: NRS key (0 = R316 behaviour, 1 = pass 1 carries)
unsigned g_pass2 = 0u;        // R332: 0 = Pass2 absent, 1 = full, 2 = nrs
unsigned g_passn[MAX_SETS] = {};   // R350: the same for Pass3 / Pass4 (index = pass - 1; [0] and [1] unused)
float    g_scale = 0.75f;
bool     g_render = false;    // R318: extent = the table's render size
bool     g_depth = false;
float    g_gain = 1.0f;
float    g_sharp = 0.0f;
float    g_color = 0.90f;
unsigned g_mode = 1u;

// geometry
unsigned g_nw = 0, g_nh = 0;  // native

// R364 (Marcelo 2026-11-09 10:16: resolution must switch live too): what the arm was handed, kept so a pass can be
// turned Residual / Native in the SESSION. Nothing here is used by the per-frame path.
ID3D12Device *g_arm_dev = nullptr;      // the neural device at arm (borrowed)
unsigned      g_arm_w = 0, g_arm_h = 0; // native extent at arm
DXGI_FORMAT   g_arm_fmt = DXGI_FORMAT_UNKNOWN;   // the stream's raw format (make_tex takes this one)
bool          g_arm_nrscale = false;    // NRScale carried the residual this run: our module stays out
char          g_arm_refusal[200] = {};  // the R316a shape refusal, if any (empty = none)
bool          g_kernels = false;        // build_kernels succeeded this arm

// D3D12 objects (ours; the device is borrowed, not AddRef'd)
ID3D12Device         *g_dev   = nullptr;
ID3D12RootSignature  *g_rs    = nullptr;
ID3D12PipelineState  *g_down  = nullptr;
ID3D12PipelineState  *g_res   = nullptr;
ID3D12DescriptorHeap *g_heap  = nullptr;
UINT                  g_inc   = 0;
DXGI_FORMAT           g_fmt   = DXGI_FORMAT_UNKNOWN;

// one heap, per pass two parities, disjoint slot ranges per dispatch. The root
// table is 4 SRVs then 1 UAV at offset 4, for both kernels, so each dispatch owns
// five consecutive slots:
//   down    0..4   (t0 anchor at 0, u0 small_in at 4; 1..3 not read)
//   resolve 5..9   (t0 small_in, t1 small_out, t2 anchor, t3 depth, u0 dst)
const UINT SLOTS_PER_PARITY = 10u;
const UINT SLOTS_PER_PASS   = 2u * SLOTS_PER_PARITY;

// Root layout shared by both kernels: 32-bit constants at b0 (10), then one
// table of 4 SRVs + 1 UAV. The downsample uses t0/u0 and 4 constants; the
// rest of the table is simply not read.
struct consts_t
{
    unsigned native_w, native_h;
    unsigned depth_w, depth_h;      // for the downsample: dst w/h
    float    gain, sharp;
    unsigned mode;
    float    color;
    unsigned has_depth;
    unsigned pad;
};

void rel(IUnknown *&p)
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

void release_all()
{
    g_active.store(false, std::memory_order_relaxed);
    IUnknown *u;
    u = g_down; rel(u); g_down = nullptr;
    u = g_res;  rel(u); g_res  = nullptr;
    u = g_rs;   rel(u); g_rs   = nullptr;
    u = g_heap; rel(u); g_heap = nullptr;
    for (unsigned i = 0; i < MAX_SETS; ++i)
    {
        pass_set &ps = g_set[i];
        u = ps.sin;  rel(u); ps.sin  = nullptr;
        u = ps.sout; rel(u); ps.sout = nullptr;
        ps = pass_set{};
    }
    g_dev = nullptr;
    g_kernels = false;
}

// ---- keys, from mgpu.ini beside the module (the nr16 reader, same rules) ----
bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod);
    return true;
}

float key_float(const char *buf, size_t n, const char *key, float dflt, float lo, float hi)
{
    const char *v = mgpu::config::find(buf, n, key);
    if (v == nullptr) return dflt;
    char *end = nullptr;
    const double d = strtod(v, &end);
    if (end == v) return dflt;
    if (d < (double)lo || d > (double)hi) return dflt;
    return (float)d;
}

bool key_flag(const char *buf, size_t n, const char *key)
{
    const char *v = mgpu::config::find(buf, n, key);
    return v != nullptr && atoi(v) == 1;
}

void read_look(const char *buf, size_t got)
{
    g_scale  = key_float(buf, got, "ResidualScale", 0.75f, 0.30f, 1.00f);
    g_render = key_flag(buf, got, "ResidualRender");
    g_depth  = key_flag(buf, got, "ResidualDepth");
    g_gain   = key_float(buf, got, "ResidualGain", 1.0f, 0.0f, 2.0f);
    g_sharp  = key_float(buf, got, "ResidualSharpness", 0.0f, 0.0f, 1.0f);
    g_color  = key_float(buf, got, "ResidualColor", 0.90f, 0.0f, 1.0f);
    g_mode   = key_flag(buf, got, "ResidualMode") ? 1u
             : ((mgpu::config::find(buf, got, "ResidualMode") != nullptr) ? 0u : 1u);
}

// Returns the raw ResidualPass value (-1 absent) and fills g_set[i].on / g_nrs / g_pass2.
//   NRS absent or 0 (R316, unchanged): ResidualPass 1 or 2 turns that one pass on; any
//       other value or no line: the module does not exist.
//   NRS=1 (R332): pass 1 carries it. Pass 2 carries it when Pass2=nrs, or when Pass2 is
//       absent and ResidualPass=2 (the panel's Medium / Light keep their meaning for the
//       2nd pass). Pass2=full, or Pass2 absent with any other ResidualPass: pass 2 is a
//       full pass. ResidualPass absent is fine with NRS=1.
int read_keys()
{
    for (unsigned i = 0; i < MAX_SETS; ++i) g_set[i].on = false;
    g_nrs = 0u; g_pass2 = 0u;
    for (unsigned i = 0; i < MAX_SETS; ++i) g_passn[i] = 0u;   // R350
    g_scale = 0.75f; g_render = false; g_depth = false;
    g_gain = 1.0f; g_sharp = 0.0f; g_color = 0.90f; g_mode = 1u;
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return -1;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return -1;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return -1;   // whole-document rule
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "ResidualPass");
    const int raw = (v != nullptr) ? atoi(v) : -1;
    // R332: the NRS key. Read before ResidualPass is judged, because with NRS=1 the module
    // exists even when ResidualPass is absent.
    g_nrs = key_flag(buf.data(), got, "NRS") ? 1u : 0u;
    if (g_nrs != 0u)
    {
        const char *p2 = mgpu::config::find(buf.data(), got, "Pass2");
        if (p2 != nullptr) g_pass2 = (p2[0] == 'n' || p2[0] == 'N') ? 2u : 1u;
        g_set[0].on = true;
        g_set[1].on = (g_pass2 == 2u) || (g_pass2 == 0u && raw == 2);
        // R350 (Experimental, Passes=3 / 4): Pass3 / Pass4 = full | nrs, the Pass2 grammar. Absent = full.
        // Only read with NRS=1, as Pass2 is; the arm turns a pass off again when it is not built.
        for (unsigned i = 2u; i < MAX_SETS; ++i)
        {
            char key[8]; snprintf(key, sizeof key, "Pass%u", i + 1u);
            const char *pn = mgpu::config::find(buf.data(), got, key);
            g_passn[i] = (pn != nullptr) ? ((pn[0] == 'n' || pn[0] == 'N') ? 2u : 1u) : 0u;
            g_set[i].on = (g_passn[i] == 2u);
        }
        read_look(buf.data(), got);
        return raw;
    }
    // R366: without NRS=1, Pass3 / Pass4 = nrs still turn their pass on (the panel writes them for any pass 1
    // setting now that resolution switches live); the module exists when any residual key names a pass.
    bool any3 = false;
    for (unsigned i = 2u; i < MAX_SETS; ++i)
    {
        char key[8]; snprintf(key, sizeof key, "Pass%u", i + 1u);
        const char *pn = mgpu::config::find(buf.data(), got, key);
        g_passn[i] = (pn != nullptr) ? ((pn[0] == 'n' || pn[0] == 'N') ? 2u : 1u) : 0u;
        g_set[i].on = (g_passn[i] == 2u);
        any3 = any3 || g_set[i].on;
    }
    if (v == nullptr && !any3) return -1;
    if (raw == 1 || raw == 2) g_set[(unsigned)(raw - 1)].on = true;
    else if (!any3) return raw;
    read_look(buf.data(), got);
    return (raw == -1) ? 0 : raw;
}

// ---- GPU objects ----
HRESULT make_tex(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                        IID_PPV_ARGS(out));
}

bool build_kernels(ID3D12Device *dev)
{
    char line[900];
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = (dc != nullptr)
                               ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][RSD] d3dcompiler_47.dll / D3DCompile not available - the "
                          "residual pass stays off for this arm.");
        return false;
    }
    ID3DBlob *cs[2] = { nullptr, nullptr };
    const char *entry[2] = { "CS_Down", "CS_Resolve" };
    for (int i = 0; i < 2; ++i)
    {
        ID3DBlob *err = nullptr;
        // R316c: cs_5_0, not cs_5_1. FC6's d3dcompiler_47 rejects cs_5_1 (X3506) and the module
        // stayed off there; R307 compiles at cs_5_0 on FC6 for the same reason. The kernels use
        // nothing from shader model 5.1.
        const HRESULT hr = compile(kHLSL, strlen(kHLSL), "residual", nullptr, nullptr, entry[i],
                                   "cs_5_0", 0, 0, &cs[i], &err);
        if (FAILED(hr) || cs[i] == nullptr)
        {
            snprintf(line, sizeof line, "[MGPU][RSD] kernel '%s' failed to compile: 0x%08X %s",
                     entry[i], (unsigned)hr,
                     (err != nullptr) ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(line);
            if (err != nullptr) err->Release();
            if (cs[0] != nullptr) cs[0]->Release();
            if (cs[1] != nullptr) cs[1]->Release();
            return false;
        }
        if (err != nullptr) err->Release();
    }

    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rng[0].NumDescriptors = 4;
    rng[0].BaseShaderRegister = 0;
    rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rng[1].NumDescriptors = 1;
    rng[1].BaseShaderRegister = 0;
    rng[1].OffsetInDescriptorsFromTableStart = 4;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 10;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 2;
    rp[1].DescriptorTable.pDescriptorRanges = rng;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &ss;

    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err != nullptr) err->Release();
    if (SUCCEEDED(hr))
    {
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      IID_PPV_ARGS(&g_rs));
        sig->Release();
    }
    ID3D12PipelineState **pso[2] = { &g_down, &g_res };
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs[i]->GetBufferPointer();
        pd.CS.BytecodeLength = cs[i]->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso[i]));
    }
    cs[0]->Release();
    cs[1]->Release();
    if (FAILED(hr))
    {
        snprintf(line, sizeof line, "[MGPU][RSD] root signature / PSO failed: 0x%08X", (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = MAX_SETS * SLOTS_PER_PASS;   // R332: one range per pass
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr)) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

// An _SRGB view would linearise on read; the kernels work on code values.
DXGI_FORMAT view_format(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:   return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:                              return f;
    }
}

// The SRV format for a depth resource, or UNKNOWN when it cannot be read as one
// channel. The 24/8 and 32/8 stencil layouts are not accepted: their SRV formats
// need a planar view this module does not write.
DXGI_FORMAT depth_view_format(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_FLOAT:    return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default:                       return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned pi, unsigned parity, unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += ((SIZE_T)pi * SLOTS_PER_PASS + (SIZE_T)(parity & 1u) * SLOTS_PER_PARITY + slot) * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned pi, unsigned parity, unsigned slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += ((UINT64)pi * SLOTS_PER_PASS + (UINT64)(parity & 1u) * SLOTS_PER_PARITY + slot) * g_inc;
    return g;
}

void write_srv(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *r, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r, &d, h);
}

void write_uav(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *r, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(r, nullptr, &d, h);
}

void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

// Tracked transitions of a pass's scratch pair: the recorded state is the truth.
void sin_to(ID3D12GraphicsCommandList *cl, pass_set &ps, D3D12_RESOURCE_STATES to)
{
    transition(cl, ps.sin, ps.sin_state, to);
    ps.sin_state = to;
}

void sout_to(ID3D12GraphicsCommandList *cl, pass_set &ps, D3D12_RESOURCE_STATES to)
{
    transition(cl, ps.sout, ps.sout_state, to);
    ps.sout_state = to;
}

void uav_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

// The pass set for `pi`, or nullptr when that pass does not carry (or is off).
pass_set *live(unsigned pi)
{
    if (pi >= MAX_SETS) return nullptr;
    pass_set &ps = g_set[pi];
    return ps.active ? &ps : nullptr;
}

void refresh_any()
{
    bool any = false;
    for (unsigned i = 0; i < MAX_SETS; ++i) any = any || g_set[i].active;
    g_active.store(any, std::memory_order_release);
}

// The pass that bind_create (no pass argument) describes: the lowest carrying pass -
// with NRS absent the one ResidualPass names, exactly as R316 did.
unsigned first_pass()
{
    for (unsigned i = 0; i < MAX_SETS; ++i) if (g_set[i].on) return i;
    return 0u;
}

void bind_scratch(NVSDK_NGX_Parameter *p, const pass_set &ps)
{
    p->Set("DLSSNR.Color", ps.sin);
    p->Set("DLSSNR.Output", ps.sout);
    p->Set("DLSSNR.ColorSubrectBaseX", 0u);
    p->Set("DLSSNR.ColorSubrectBaseY", 0u);
    p->Set("DLSSNR.ColorSubrectWidth",  (unsigned int)ps.ww);
    p->Set("DLSSNR.ColorSubrectHeight", (unsigned int)ps.wh);
    p->Set("DLSSNR.OutputSubrectBaseX", 0u);
    p->Set("DLSSNR.OutputSubrectBaseY", 0u);
    p->Set("DLSSNR.OutputSubrectWidth",  (unsigned int)ps.ww);
    p->Set("DLSSNR.OutputSubrectHeight", (unsigned int)ps.wh);
}
} // namespace

bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt,
         unsigned passes, bool nrscale_on, unsigned render_w, unsigned render_h,
         const char *shape_refusal, bool srgb_in)
{
    release_all();
    const int raw = read_keys();
    // R364: keep the arm's inputs for the live Residual / Native switch, whatever the keys say now
    g_arm_dev = dev; g_arm_w = width; g_arm_h = height; g_arm_fmt = fmt; g_arm_nrscale = nrscale_on;
    g_arm_refusal[0] = 0;
    if (shape_refusal != nullptr) snprintf(g_arm_refusal, sizeof g_arm_refusal, "%s", shape_refusal);
    bool any_on = false;
    for (unsigned i = 0; i < MAX_SETS; ++i) any_on = any_on || g_set[i].on;   // R350: every set
    if (raw == -1 && g_nrs == 0u) return false;   // keys absent: the module does not exist
    char l[700];
    if (!any_on)
    {
        snprintf(l, sizeof l, "[MGPU][RSD] residual pass OFF (ResidualPass=%d; 1 or 2 turns it on).", raw);
        mgpu::diag::info(l);
        return false;
    }
    if (nrscale_on)
    {
        mgpu::diag::info("[MGPU][RSD] residual pass OFF: NRScale is set, so the NRScale path (R299) "
                         "carries the residual this run. One residual path at a time.");
        for (unsigned i = 0; i < MAX_SETS; ++i) g_set[i].on = false;
        return false;
    }
    // R319: `passes` is the number of pass features the arm creates (MAX_PASSES), not the
    // live Passes value. The panel moves between x1 and x2 live; the carrying pass's
    // feature has to exist at the work extent whichever one the session starts on.
    if (passes > MAX_SETS) passes = MAX_SETS;
    for (unsigned i = passes; i < MAX_SETS; ++i)
    {
        if (!g_set[i].on) continue;
        snprintf(l, sizeof l, "[MGPU][RSD] residual pass OFF on pass %u: this build creates "
                 "%u pass feature(s).", i + 1u, passes);
        mgpu::diag::info(l);
        g_set[i].on = false;
    }
    any_on = false;
    for (unsigned i = 0; i < MAX_SETS; ++i) any_on = any_on || g_set[i].on;
    if (!any_on) return false;
    // R316a: the carrying pass's feature is built at the work extent, so every frame
    // of the run must be able to take the residual. Settings that change the frame's
    // shape are known here and do not change after arm; refuse them now rather than
    // run the normal path on a feature of the wrong size.
    // R332: with NRS=1 SrgbInput=1 on pass 1 is a PATH, not a refusal (below). With NRS
    // absent the R316 refusal stands, word for word.
    if (shape_refusal == nullptr && srgb_in && g_set[0].on && g_nrs == 0u)
        shape_refusal = "SrgbInput=1 is set (pass 1 would hand NR our scratch, not the sRGB input)";
    if (shape_refusal != nullptr)
    {
        snprintf(l, sizeof l, "[MGPU][RSD] residual pass OFF for this run: %s. It runs with SRUpscale=0, "
                 "Subrect=100, CostCurve=0, no ScaleMode, no Pass1Fovea / Pass2Region / Pass2Fovea"
                 "%s. Every feature is created at its usual size.",
                 shape_refusal, (g_set[0].on && g_nrs == 0u) ? " and SrgbInput=0" : "");
        mgpu::diag::warn(l);
        for (unsigned i = 0; i < MAX_SETS; ++i) g_set[i].on = false;
        return false;
    }
    if (dev == nullptr || width == 0 || height == 0) return false;

    // The work extent: the table's render size (R318), or scale x native. One recipe,
    // applied to every carrying pass (pass 2's anchor is pass 1's native output, so
    // the same extent is the right one there too).
    bool from_render = false;
    unsigned ww = 0, wh = 0;
    // R316b: the render size is used only when it is BELOW native. On DLAA the game renders
    // at native, so following it would run pass 2 at full size plus the downsample and the
    // resolve - more work than Full, no saving. That case takes ResidualScale (Medium), as a
    // title without the NGX table does.
    const bool r_known = (render_w >= 128u && render_h >= 128u && render_w <= width && render_h <= height);
    const bool r_below = r_known && (render_w < width || render_h < height);
    if (g_render && r_below)
    {
        ww = render_w; wh = render_h; from_render = true;
    }
    else
    {
        if (g_render && r_known)
            mgpu::diag::info("[MGPU][RSD] ResidualRender=1 but the game renders at native size (DLAA) - "
                             "ResidualScale is used for this run.");
        else if (g_render)
            mgpu::diag::info("[MGPU][RSD] ResidualRender=1 but the NGX table has no usable render size "
                             "at arm - ResidualScale is used for this run.");
        ww = (unsigned)((double)width  * (double)g_scale + 0.5);
        wh = (unsigned)((double)height * (double)g_scale + 0.5);
    }
    if (ww < 128u) ww = 128u;
    if (wh < 128u) wh = 128u;
    ww &= ~1u; wh &= ~1u;
    if (ww > width)  ww = width  & ~1u;
    if (wh > height) wh = height & ~1u;

    g_dev = dev; g_nw = width; g_nh = height; g_fmt = view_format(fmt);
    HRESULT h = S_OK;
    for (unsigned i = 0; i < MAX_SETS && SUCCEEDED(h); ++i)
    {
        pass_set &ps = g_set[i];
        if (!ps.on) continue;
        ps.ww = ww; ps.wh = wh; ps.from_render = from_render;
        h = make_tex(dev, ww, wh, fmt, &ps.sin);
        if (SUCCEEDED(h)) h = make_tex(dev, ww, wh, fmt, &ps.sout);
    }
    if (FAILED(h) || !build_kernels(dev))
    {
        snprintf(l, sizeof l, "[MGPU][RSD] residual pass could not be built (0x%08X) - OFF for this "
                 "arm; the stream runs its normal path.", (unsigned)h);
        mgpu::diag::error(l);
        release_all();
        return false;
    }
    g_kernels = true;   // R364
    for (unsigned i = 0; i < MAX_SETS; ++i)
    {
        pass_set &ps = g_set[i];
        if (!ps.on) continue;
        ps.active = true;
        snprintf(l, sizeof l,
                 "[MGPU][RSD] RESIDUAL PASS ON (R316): pass %u (of up to %u) carries it. work=%ux%u (%s) native=%ux%u "
                 "fmt=%d gain=%.2f sharp=%.2f color=%.2f mode=%u depth=%s. The pass's feature is created at the "
                 "work extent; each evaluate downsamples its input, runs DLSS-NR there and adds the residual "
                 "onto the pass's input at native size.",
                 i + 1u, passes, ww, wh,
                 from_render ? "the table's render size, ResidualRender=1" : "ResidualScale x native",
                 width, height, (int)fmt, (double)g_gain, (double)g_sharp, (double)g_color, g_mode,
                 g_depth ? "guard on, taps scaled to the depth's size" : "no depth to the resolve");
        mgpu::diag::info(l);
    }
    if (g_nrs != 0u)
    {
        snprintf(l, sizeof l,
                 "[MGPU][RSD][R332] NRS=1: pass 1 runs our residual (the 0.5.0 path); pass 2 at x2 is %s "
                 "(Pass2=%s%s). N01950's NRScale is untouched and still wins when set.",
                 g_set[1].on ? "our residual too, anchored on pass 1's resolved native output" : "a FULL NR pass",
                 (g_pass2 == 2u) ? "nrs" : (g_pass2 == 1u) ? "full" : "absent",
                 (g_pass2 == 0u) ? (raw == 2 ? ", ResidualPass=2 kept its meaning for the 2nd pass"
                                             : ", no ResidualPass=2") : "");
        mgpu::diag::info(l);
        if (srgb_in && g_set[0].on)
            mgpu::diag::warn("[MGPU][RSD][R332] SrgbInput=1 with the residual on pass 1: NR reads OUR work-extent "
                             "scratch (the linear-typed format, code values, as every SrgbInput=0 run does), not "
                             "the sRGB-typed tex_in; the downsample reads tex_in through a UNORM view (same bytes). "
                             "The P7.8b free-decode experiment does not apply to the carrying pass this run; "
                             "compare halves in Present=split as that experiment says.");
        for (unsigned i = 2u; i < MAX_SETS; ++i)   // R350: passes 3 / 4 exist only when built (Passes=3 / 4)
        {
            if (i >= passes) continue;
            snprintf(l, sizeof l, "[MGPU][RSD][R350] pass %u is %s (Pass%u=%s).", i + 1u,
                     g_set[i].on ? "our residual, anchored on the pass before it" : "a FULL NR pass",
                     i + 1u, (g_passn[i] == 2u) ? "nrs" : (g_passn[i] == 1u) ? "full" : "absent");
            mgpu::diag::info(l);
        }
    }
    refresh_any();
    return true;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

// R364: turn pass `pi` Residual (on) or Native (off) in the session. Called on the bridge thread, between
// frames, right before the caller recreates that pass's feature at the new extent. Native: the set stops
// carrying (its scratch stays allocated - the GPU may still read it; it is freed at the next arm). Residual:
// the kernels are built on first need (an ini with no residual key never built them) and the pass's scratch
// pair is allocated at the module's extent (ResidualScale x native; the table's render size is an arm-time
// decision and is not re-read here). Refusals mirror arm(): NRScale carrying, or the R316a shape refusal.
bool set_pass_live(unsigned pi, bool on, char *line, size_t cap)
{
    if (pi >= MAX_SETS) return false;
    pass_set &ps = g_set[pi];
    if (!on)
    {
        ps.on = false; ps.active = false;
        refresh_any();
        snprintf(line, cap, "[MGPU][RSD][R364] pass %u -> NATIVE live (a full NR pass; its scratch stays allocated until the next arm).", pi + 1u);
        return true;
    }
    if (g_arm_dev == nullptr || g_arm_w == 0u || g_arm_h == 0u)
    {
        snprintf(line, cap, "[MGPU][RSD][R364] pass %u cannot turn Residual live: the module saw no arm this run.", pi + 1u);
        return false;
    }
    if (g_arm_nrscale)
    {
        snprintf(line, cap, "[MGPU][RSD][R364] pass %u cannot turn Residual live: NRScale carries the residual this run (one path at a time).", pi + 1u);
        return false;
    }
    if (g_arm_refusal[0] != 0)
    {
        snprintf(line, cap, "[MGPU][RSD][R364] pass %u cannot turn Residual live: %s (the arm's refusal stands).", pi + 1u, g_arm_refusal);
        return false;
    }
    if (!g_kernels)
    {
        g_dev = g_arm_dev; g_nw = g_arm_w; g_nh = g_arm_h; g_fmt = view_format(g_arm_fmt);
        if (!build_kernels(g_arm_dev))
        {
            snprintf(line, cap, "[MGPU][RSD][R364] pass %u cannot turn Residual live: the kernels did not build.", pi + 1u);
            return false;
        }
        g_kernels = true;
    }
    if (ps.sin == nullptr || ps.sout == nullptr)
    {
        unsigned ww = (unsigned)((double)g_arm_w * (double)g_scale + 0.5);
        unsigned wh = (unsigned)((double)g_arm_h * (double)g_scale + 0.5);
        if (ww < 128u) ww = 128u;
        if (wh < 128u) wh = 128u;
        ww &= ~1u; wh &= ~1u;
        if (ww > g_arm_w) ww = g_arm_w & ~1u;
        if (wh > g_arm_h) wh = g_arm_h & ~1u;
        HRESULT h = make_tex(g_arm_dev, ww, wh, g_arm_fmt, &ps.sin);
        if (SUCCEEDED(h)) h = make_tex(g_arm_dev, ww, wh, g_arm_fmt, &ps.sout);
        if (FAILED(h))
        {
            IUnknown *u = ps.sin; rel(u); ps.sin = nullptr;
            u = ps.sout; rel(u); ps.sout = nullptr;
            snprintf(line, cap, "[MGPU][RSD][R364] pass %u cannot turn Residual live: scratch allocation failed (0x%08X).", pi + 1u, (unsigned)h);
            return false;
        }
        ps.ww = ww; ps.wh = wh; ps.from_render = false;
        ps.sin_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ps.sout_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    ps.on = true; ps.active = true; ps.fails = 0u; ps.evals = 0ull; ps.said_bind = false; ps.said_depth = false;
    refresh_any();
    snprintf(line, cap, "[MGPU][RSD][R364] pass %u -> RESIDUAL live: work=%ux%u (ResidualScale x native), native=%ux%u.",
             pi + 1u, ps.ww, ps.wh, g_arm_w, g_arm_h);
    return true;
}

bool carries(unsigned pi)
{
    return live(pi) != nullptr;
}

unsigned pass()
{
    return first_pass();
}

void work_extent(unsigned &w, unsigned &h)
{
    const pass_set &ps = g_set[first_pass()];
    w = ps.ww; h = ps.wh;
}

void work_extent(unsigned pi, unsigned &w, unsigned &h)
{
    const pass_set *ps = live(pi);
    if (ps == nullptr) return;
    w = ps->ww; h = ps->wh;
}

void bind_create(NVSDK_NGX_Parameter *p)
{
    if (!active() || p == nullptr) return;
    bind_scratch(p, g_set[first_pass()]);
}

void bind_create(NVSDK_NGX_Parameter *p, unsigned pi)
{
    const pass_set *ps = live(pi);
    if (ps == nullptr || p == nullptr) return;
    bind_scratch(p, *ps);
}

void before(ID3D12GraphicsCommandList *cl, unsigned pi, unsigned parity, NVSDK_NGX_Parameter *p,
            ID3D12Resource *anchor)
{
    pass_set *psp = live(pi);
    if (psp == nullptr || cl == nullptr || p == nullptr || anchor == nullptr) return;
    pass_set &ps = *psp;

    // downsample: anchor (SRV, already NON_PIXEL_SHADER_RESOURCE) -> small_in (UAV)
    sin_to(cl, ps, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    write_srv(cpu_slot(pi, parity, 0u), anchor, g_fmt);
    for (unsigned k = 1u; k <= 3u; ++k) write_srv(cpu_slot(pi, parity, k), anchor, g_fmt);   // not read; valid anyway
    write_uav(cpu_slot(pi, parity, 4u), ps.sin, g_fmt);
    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_down);
    consts_t c{};
    c.native_w = g_nw; c.native_h = g_nh; c.depth_w = ps.ww; c.depth_h = ps.wh;
    cl->SetComputeRoot32BitConstants(0, 10, &c, 0);
    cl->SetComputeRootDescriptorTable(1, gpu_slot(pi, parity, 0u));
    cl->Dispatch((ps.ww + 7u) / 8u, (ps.wh + 7u) / 8u, 1u);
    // the write is not visible until this says it is (R299's lesson)
    sin_to(cl, ps, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sout_to(cl, ps, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // the snippet re-reads Width/Height on every evaluate (R299's lesson)
    p->Set("DLSSNR.Width",  (unsigned int)ps.ww);
    p->Set("DLSSNR.Height", (unsigned int)ps.wh);
    bind_scratch(p, ps);
    // Vector scale x work/native, as the proxy and R299 do. Left as is on purpose:
    // whether the vectors want this when they are already render-sized is the open
    // scale-convention question, and it is answered by the picture, not here.
    ps.mvx0 = 1.0f; ps.mvy0 = 1.0f;
    (void)p->Get("DLSSNR.MVecScaleX", &ps.mvx0);
    (void)p->Get("DLSSNR.MVecScaleY", &ps.mvy0);
    p->Set("DLSSNR.MVecScaleX", ps.mvx0 * ((float)ps.ww / (float)g_nw));
    p->Set("DLSSNR.MVecScaleY", ps.mvy0 * ((float)ps.wh / (float)g_nh));
}

void after(ID3D12GraphicsCommandList *cl, unsigned pi, unsigned parity, NVSDK_NGX_Parameter *p,
           ID3D12Resource *anchor, ID3D12Resource *dst, bool evaluate_ok,
           ID3D12Resource *nr_color, ID3D12Resource *nr_output,
           unsigned native_w, unsigned native_h,
           unsigned rbx, unsigned rby, unsigned rw, unsigned rh)
{
    pass_set *psp = live(pi);
    if (psp == nullptr || cl == nullptr || p == nullptr) return;
    pass_set &ps = *psp;

    // the snippet wrote small_out as a UAV; make that visible, then read it
    uav_barrier(cl, ps.sout);
    sout_to(cl, ps, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // depth: whatever NGX was handed this evaluate, at any size, if readable
    ID3D12Resource *depth = nullptr;
    DXGI_FORMAT dfmt = DXGI_FORMAT_UNKNOWN;
    unsigned dw = 0, dh = 0;
    if (g_depth)
    {
        (void)p->Get("DLSSNR.Depth", &depth);
        if (depth != nullptr)
        {
            const D3D12_RESOURCE_DESC dd = depth->GetDesc();
            dfmt = depth_view_format(dd.Format);
            const bool srv_ok = (dd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
            if (!srv_ok || dfmt == DXGI_FORMAT_UNKNOWN || dd.Width == 0 || dd.Height == 0)
            {
                if (!ps.said_depth)
                {
                    ps.said_depth = true;
                    char l[300];
                    snprintf(l, sizeof l, "[MGPU][RSD] pass %u: depth not readable by the resolve (srv=%d fmt=%d) - "
                             "the silhouette guard is off this run.", pi + 1u, srv_ok ? 1 : 0, (int)dd.Format);
                    mgpu::diag::warn(l);
                }
                depth = nullptr;
            }
            else { dw = (unsigned)dd.Width; dh = dd.Height; }
        }
    }

    if (evaluate_ok && anchor != nullptr && dst != nullptr)
    {
        write_srv(cpu_slot(pi, parity, 5u), ps.sin, g_fmt);
        write_srv(cpu_slot(pi, parity, 6u), ps.sout, g_fmt);
        write_srv(cpu_slot(pi, parity, 7u), anchor, g_fmt);
        // t3 must hold a valid descriptor even when unused: small_in stands in
        write_srv(cpu_slot(pi, parity, 8u), (depth != nullptr) ? depth : ps.sin,
                  (depth != nullptr) ? dfmt : g_fmt);
        write_uav(cpu_slot(pi, parity, 9u), dst, g_fmt);
        ID3D12DescriptorHeap *heaps[1] = { g_heap };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(g_rs);
        cl->SetPipelineState(g_res);
        consts_t c{};
        c.native_w = g_nw; c.native_h = g_nh;
        c.depth_w = (depth != nullptr) ? dw : g_nw;
        c.depth_h = (depth != nullptr) ? dh : g_nh;
        c.gain = g_gain; c.sharp = g_sharp; c.mode = g_mode; c.color = g_color;
        c.has_depth = (depth != nullptr) ? 1u : 0u;
        cl->SetComputeRoot32BitConstants(0, 10, &c, 0);
        cl->SetComputeRootDescriptorTable(1, gpu_slot(pi, parity, 5u));
        cl->Dispatch((g_nw + 7u) / 8u, (g_nh + 7u) / 8u, 1u);
        uav_barrier(cl, dst);   // the resolve's writes done before anything reads dst
        ps.fails = 0u;
    }
    else if (!evaluate_ok)
    {
        // A failed evaluate leaves small_out stale: no resolve (the proxy's rule),
        // dst keeps what it had. Three in a row turn THIS pass's module off for the arm.
        if (++ps.fails >= 3u && ps.active)
        {
            ps.active = false;
            refresh_any();
            char l[400];
            snprintf(l, sizeof l, "[MGPU][RSD] pass %u: three failed evaluates in a row - residual pass OFF for the "
                     "rest of this arm; the pass's feature stays at the work extent, so the "
                     "normal path runs it at that size until the next arm.", pi + 1u);
            mgpu::diag::error(l);
        }
    }
    sout_to(cl, ps, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ++ps.evals;

    if (!ps.said_bind)
    {
        ps.said_bind = true;
        char l[400];
        snprintf(l, sizeof l,
                 "[MGPU][RSD] pass %u first evaluate: %s. anchor==dst=%d mvScale=%.3f/%.3f depth=%s work=%ux%u",
                 pi + 1u,
                 evaluate_ok ? "SUCCESS" : "FAILED (see the P4.1 EvaluateFeature line)",
                 (anchor == dst) ? 1 : 0,
                 (double)(ps.mvx0 * ((float)ps.ww / (float)g_nw)),
                 (double)(ps.mvy0 * ((float)ps.wh / (float)g_nh)),
                 (depth != nullptr) ? "bound" : (g_depth ? "none this evaluate" : "off"),
                 ps.ww, ps.wh);
        mgpu::diag::info(l);
    }
    else if ((ps.evals % 600ull) == 0ull)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][RSD] pass %u evaluates=%llu at work=%ux%u depth=%s", pi + 1u, ps.evals,
                 ps.ww, ps.wh, (depth != nullptr) ? "bound" : "off");
        mgpu::diag::info(l);
    }

    // Put the caller's binding back: the rest of the frame and the next pass
    // expect the native resources with the native subrects, not our scratch.
    p->Set("DLSSNR.Color", nr_color);
    p->Set("DLSSNR.Output", nr_output);
    p->Set("DLSSNR.Width",  (unsigned int)native_w);
    p->Set("DLSSNR.Height", (unsigned int)native_h);
    p->Set("DLSSNR.ColorSubrectBaseX", (unsigned int)rbx);
    p->Set("DLSSNR.ColorSubrectBaseY", (unsigned int)rby);
    p->Set("DLSSNR.ColorSubrectWidth",  (unsigned int)rw);
    p->Set("DLSSNR.ColorSubrectHeight", (unsigned int)rh);
    p->Set("DLSSNR.OutputSubrectBaseX", (unsigned int)rbx);
    p->Set("DLSSNR.OutputSubrectBaseY", (unsigned int)rby);
    p->Set("DLSSNR.OutputSubrectWidth",  (unsigned int)rw);
    p->Set("DLSSNR.OutputSubrectHeight", (unsigned int)rh);
    p->Set("DLSSNR.MVecScaleX", ps.mvx0);
    p->Set("DLSSNR.MVecScaleY", ps.mvy0);
}

bool nrs_on()
{
    return g_nrs != 0u;
}

void release()
{
    release_all();
}
}
