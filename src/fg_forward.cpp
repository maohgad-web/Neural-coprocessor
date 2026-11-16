// MGPU Bridge - R396 FrameGen=5: the forward warp. See fg_forward.hpp for what it is for and the
// contract it keeps. The compute-pass plumbing follows residual.cpp (R316).

#include "fg_forward.hpp"

#include <cstdio>
#include <cstring>

#include "diag.hpp"
#include "gpu1_context.hpp"   // present_queue_addref

namespace mgpu::fgfwd
{
namespace
{
const char kHLSL[] =
    "cbuffer C : register(b0)\n"
    "{\n"
    "  uint  gW;    uint  gH;     float gSx;  float gSy;\n"
    "  float gPush; uint  gFlags; float gTol; float gRad;\n"
    "};\n"
    "Texture2D<float4>   gNeural : register(t0);\n"
    "Texture2D<float4>   gMv     : register(t1);\n"
    "Texture2D<float4>   gDepthB : register(t2);\n"
    "Texture2D<float4>   gDepthA : register(t3);\n"
    "Texture2D<float4>   gGame   : register(t4);\n"
    "RWTexture2D<float4> gOut    : register(u0);\n"
    "SamplerState gLin : register(s0);\n"
    "SamplerState gPnt : register(s1);\n"
    "static const float2 kRing[8] = { float2(1.0, 0.0), float2(0.7071, 0.7071), float2(0.0, 1.0), float2(-0.7071, 0.7071),\n"
    "                                 float2(-1.0, 0.0), float2(-0.7071, -0.7071), float2(0.0, -1.0), float2(0.7071, -0.7071) };\n"
    // grows as the surface comes nearer, for either depth convention
    "float eye(float d) { return ((gFlags & 1u) != 0u) ? d : (1.0 - d); }\n"
    "bool inside(float2 p) { return p.x >= 0.0 && p.y >= 0.0 && p.x <= 1.0 && p.y <= 1.0; }\n"
    // where the surface at uv (frame B) was in the neural frame, pushed gPush of the way
    "float2 src_of(float2 uv)\n"
    "{\n"
    "  float2 mv = gMv.SampleLevel(gLin, uv, 0.0).xy;\n"
    "  return uv + mv * float2(gSx, gSy) * gPush;\n"
    "}\n"
    // true: the neural frame shows this surface at s
    "bool covered(float2 uv, float2 s)\n"
    "{\n"
    "  if (!inside(s)) return false;\n"
    "  if ((gFlags & 2u) == 0u) return true;\n"
    "  float eb = eye(gDepthB.SampleLevel(gPnt, uv, 0.0).x);\n"
    "  float ea = eye(gDepthA.SampleLevel(gPnt, s, 0.0).x);\n"
    "  return ea <= eb * (1.0 + gTol) + 0.000001;\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void CS_Warp(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  if (id.x >= gW || id.y >= gH) return;\n"
    "  float2 px = float2(1.0 / (float)gW, 1.0 / (float)gH);\n"
    "  float2 uv = (float2((float)id.x, (float)id.y) + 0.5) * px;\n"
    "  float2 s = src_of(uv);\n"
    "  float2 sc = clamp(s, 0.0, 1.0);\n"
    "  float4 c = gNeural.SampleLevel(gLin, sc, 0.0);\n"
    "  if (!covered(uv, s))\n"
    "  {\n"
    "    if ((gFlags & 8u) != 0u)\n"
    "    {\n"
    "      c = float4(1.0, 0.0, 1.0, c.a);\n"
    "    }\n"
    "    else if ((gFlags & 4u) != 0u)\n"
    "    {\n"
    // the game's own pixel, scaled by how the neural pixels around it differ from the game's
    "      float3 an = float3(0.0, 0.0, 0.0);\n"
    "      float3 ag = float3(0.0, 0.0, 0.0);\n"
    "      float n = 0.0;\n"
    "      for (uint k = 0u; k < 8u; ++k)\n"
    "      {\n"
    "        float2 q = uv + kRing[k] * gRad * px;\n"
    "        if (inside(q))\n"
    "        {\n"
    "          float2 sq = src_of(q);\n"
    "          if (covered(q, sq))\n"
    "          {\n"
    "            an += gNeural.SampleLevel(gLin, sq, 0.0).rgb;\n"
    "            ag += gGame.SampleLevel(gLin, q, 0.0).rgb;\n"
    "            n += 1.0;\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "      float3 g = gGame.SampleLevel(gLin, uv, 0.0).rgb;\n"
    "      float3 r = float3(1.0, 1.0, 1.0);\n"
    "      if (n > 0.0) r = clamp(an / max(ag, float3(0.0001, 0.0001, 0.0001)), 0.25, 4.0);\n"
    "      c = float4(g * r, c.a);\n"
    "    }\n"
    "    else if ((gFlags & 2u) != 0u)\n"
    "    {\n"
    // the neural pixel of the neighbour of the source that is as far away as this surface
    "      float eb = eye(gDepthB.SampleLevel(gPnt, uv, 0.0).x);\n"
    "      float best = 1.0e30;\n"
    "      float2 bs = sc;\n"
    "      for (uint k = 0u; k < 16u; ++k)\n"
    "      {\n"
    "        float rad = (k < 8u) ? gRad : gRad * 2.5;\n"
    "        float2 t = sc + kRing[k & 7u] * rad * px;\n"
    "        if (inside(t))\n"
    "        {\n"
    "          float d = abs(eye(gDepthA.SampleLevel(gPnt, t, 0.0).x) - eb) + ((k < 8u) ? 0.0 : 0.0000001);\n"
    "          if (d < best) { best = d; bs = t; }\n"
    "        }\n"
    "      }\n"
    "      c = gNeural.SampleLevel(gLin, bs, 0.0);\n"
    "    }\n"
    "  }\n"
    "  gOut[id.xy] = c;\n"
    "}\n";

typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                         LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

const unsigned MAX_OUT = 4u;          // frames per pair
const unsigned SLOTS = 6u;            // t0..t4, u0
const unsigned PARITIES = 2u;         // a pair's descriptors are never rewritten while its list may still run

ID3D12Device *g_dev = nullptr;        // borrowed (the arm's)
ID3D12CommandQueue *g_queue = nullptr;   // the present queue, AddRef'd
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_pso = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
UINT g_inc = 0;
ID3D12Resource *g_out[MAX_OUT] = {};
ID3D12CommandAllocator *g_alloc[PARITIES] = {};
ID3D12GraphicsCommandList *g_list = nullptr;
ID3D12Fence *g_fence = nullptr;
UINT64 g_fence_value = 0ull, g_alloc_value[PARITIES] = {};
unsigned g_w = 0u, g_h = 0u, g_n_max = 2u, g_parity = 0u;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_inverted = true, g_up = false;
// pacing thread
unsigned long long g_pairs = 0ull, g_frames = 0ull, g_busy = 0ull, g_bad_input = 0ull, g_no_depth = 0ull, g_no_game = 0ull;

template <class T> void relt(T *&p) { if (p != nullptr) { p->Release(); p = nullptr; } }

// The SRV format of a colour or vector resource; UNKNOWN when it cannot be read as one.
DXGI_FORMAT colour_view(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:      return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:      return DXGI_FORMAT_R32G32_FLOAT;
    default:                               return f;
    }
}

// The SRV format of a depth resource read as one channel; UNKNOWN for the stencil layouts.
DXGI_FORMAT depth_view(DXGI_FORMAT f)
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

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(unsigned parity, unsigned k, unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += ((SIZE_T)parity * MAX_OUT * SLOTS + (SIZE_T)k * SLOTS + slot) * g_inc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(unsigned parity, unsigned k)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += ((UINT64)parity * MAX_OUT * SLOTS + (UINT64)k * SLOTS) * g_inc;
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

bool build(char *why, size_t n)
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = (dc != nullptr) ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr) { snprintf(why, n, "d3dcompiler_47.dll / D3DCompile is not available"); return false; }
    ID3DBlob *cs = nullptr, *err = nullptr;
    // cs_5_0 for the reason residual.cpp gives (R316c): some games ship a d3dcompiler_47 that rejects cs_5_1.
    HRESULT hr = compile(kHLSL, strlen(kHLSL), "fg_forward", nullptr, nullptr, "CS_Warp", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        snprintf(why, n, "the warp kernel did not compile: 0x%08X %.300s", (unsigned)hr,
                 (err != nullptr) ? (const char *)err->GetBufferPointer() : "");
        if (err != nullptr) err->Release();
        if (cs != nullptr) cs->Release();
        return false;
    }
    if (err != nullptr) { err->Release(); err = nullptr; }

    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rng[0].NumDescriptors = 5;
    rng[0].BaseShaderRegister = 0;
    rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rng[1].NumDescriptors = 1;
    rng[1].BaseShaderRegister = 0;
    rng[1].OffsetInDescriptorsFromTableStart = 5;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 8;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 2;
    rp[1].DescriptorTable.pDescriptorRanges = rng;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        ss[i].Filter = (i == 0) ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].MaxLOD = D3D12_FLOAT32_MAX;
        ss[i].ShaderRegister = (UINT)i;
        ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = ss;
    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (SUCCEEDED(hr))
    {
        hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rs));
        sig->Release();
    }
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    }
    cs->Release();
    if (SUCCEEDED(hr))
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = PARITIES * MAX_OUT * SLOTS;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    }
    if (SUCCEEDED(hr))
    {
        g_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = g_w;
        d.Height = g_h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = g_fmt;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (unsigned k = 0; k < g_n_max && SUCCEEDED(hr); ++k)
            hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&g_out[k]));
    }
    for (unsigned p = 0; p < PARITIES && SUCCEEDED(hr); ++p)
        hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[p]));
    if (SUCCEEDED(hr))
        hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list));
    if (SUCCEEDED(hr)) hr = g_list->Close();
    if (SUCCEEDED(hr)) hr = g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(hr)) { snprintf(why, n, "the warp pass could not be built (0x%08X)", (unsigned)hr); return false; }
    return true;
}
}   // namespace

bool create(const info &i, char *why, size_t n)
{
    release();
    if (why != nullptr && n != 0) why[0] = 0;
    if (i.dev == nullptr || i.w == 0u || i.h == 0u) { snprintf(why, n, "no device or extent"); return false; }
    g_queue = mgpu::gpu1::present_queue_addref();
    if (g_queue == nullptr) { snprintf(why, n, "the present chain has no queue yet"); return false; }
    {   // the frames are made on the present queue and read by the neural device's textures: one device object
        ID3D12Device *qd = nullptr;
        const HRESULT qh = g_queue->GetDevice(IID_PPV_ARGS(&qd));
        const bool same = SUCCEEDED(qh) && qd == i.dev;
        if (qd != nullptr) qd->Release();
        if (!same) { snprintf(why, n, "the present queue is not on the neural device object"); release(); return false; }
    }
    g_dev = i.dev;
    g_w = i.w; g_h = i.h; g_fmt = i.fmt; g_inverted = i.depth_inverted;
    g_n_max = (i.n_max < 2u) ? 2u : ((i.n_max > MAX_OUT) ? MAX_OUT : i.n_max);
    g_parity = 0u; g_fence_value = 0ull; g_alloc_value[0] = g_alloc_value[1] = 0ull;
    g_pairs = g_frames = g_busy = g_bad_input = g_no_depth = g_no_game = 0ull;
    if (!build(why, n)) { release(); return false; }
    g_up = true;
    char l[360];
    snprintf(l, sizeof l, "[MGPU][R396] FORWARD WARP UP: %u frame(s) per pair at most, %ux%u fmt=%u (%.0f MB), made on the present "
             "queue. Each shown frame is the newest neural frame fetched through the vectors of the game frame in work.",
             g_n_max, g_w, g_h, (unsigned)g_fmt,
             (double)((unsigned long long)g_n_max * g_w * g_h * (g_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8ull : 4ull)) / (1024.0 * 1024.0));
    mgpu::diag::info(l);
    return true;
}

bool warp(const pair &p)
{
    if (!g_up || p.neural == nullptr || p.mvec == nullptr) { ++g_bad_input; return false; }
    const unsigned n = (p.n < 1u) ? 1u : ((p.n > g_n_max) ? g_n_max : p.n);
    const unsigned par = g_parity & 1u;
    // this parity's allocator and descriptors were last used two pairs ago: not reused before that list has run
    if (g_fence->GetCompletedValue() < g_alloc_value[par]) { ++g_busy; return false; }

    const DXGI_FORMAT f_neural = colour_view(p.neural->GetDesc().Format);
    const DXGI_FORMAT f_mv = colour_view(p.mvec->GetDesc().Format);
    DXGI_FORMAT f_db = (p.depth_b != nullptr) ? depth_view(p.depth_b->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT f_da = (p.depth_a != nullptr) ? depth_view(p.depth_a->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
    // R398 (FGWarpFill=2, the default): no uncovered test and no fill - every pixel is the fetched neural pixel.
    const bool depth_ok = (p.fill != 2 && f_db != DXGI_FORMAT_UNKNOWN && f_da != DXGI_FORMAT_UNKNOWN);
    const bool game_ok = (p.game != nullptr);
    if (!depth_ok && p.fill != 2) ++g_no_depth;
    if (!game_ok && p.fill == 1) ++g_no_game;
    // a slot that has nothing of its own is given the neural frame: bound, never read (the flags say so)
    ID3D12Resource *const r_db = depth_ok ? p.depth_b : p.neural;
    ID3D12Resource *const r_da = depth_ok ? p.depth_a : p.neural;
    ID3D12Resource *const r_game = game_ok ? p.game : p.neural;
    if (!depth_ok) { f_db = f_neural; f_da = f_neural; }
    const DXGI_FORMAT f_game = game_ok ? colour_view(p.game->GetDesc().Format) : f_neural;
    for (unsigned k = 0; k < n; ++k)
    {
        write_srv(cpu_slot(par, k, 0u), p.neural, f_neural);
        write_srv(cpu_slot(par, k, 1u), p.mvec, f_mv);
        write_srv(cpu_slot(par, k, 2u), r_db, f_db);
        write_srv(cpu_slot(par, k, 3u), r_da, f_da);
        write_srv(cpu_slot(par, k, 4u), r_game, f_game);
        write_uav(cpu_slot(par, k, 5u), g_out[k], g_fmt);
    }

    if (FAILED(g_alloc[par]->Reset()) || FAILED(g_list->Reset(g_alloc[par], g_pso))) { ++g_busy; return false; }
    ID3D12DescriptorHeap *const heaps[1] = { g_heap };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRootSignature(g_rs);
    struct { UINT w, h; float sx, sy; float push; UINT flags; float tol; float rad; } c{};
    c.w = g_w; c.h = g_h; c.sx = p.uv_x; c.sy = p.uv_y;
    c.flags = (g_inverted ? 1u : 0u) | (depth_ok ? 2u : 0u) | ((p.fill == 1 && game_ok) ? 4u : 0u) | ((p.show == 1) ? 8u : 0u);
    c.tol = 0.10f;                                   // a surface one tenth nearer counts as another surface
    c.rad = (float)g_h * (12.0f / 1440.0f);          // the neighbour ring, in output pixels: 12 at 1440 rows
    if (c.rad < 4.0f) c.rad = 4.0f;
    for (unsigned k = 0; k < n; ++k)
    {
        c.push = p.el * (float)(k + 1u) / (float)n;
        g_list->SetComputeRoot32BitConstants(0, 8, &c, 0);
        g_list->SetComputeRootDescriptorTable(1, gpu_slot(par, k));
        g_list->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);
    }
    if (FAILED(g_list->Close())) { ++g_busy; return false; }
    // Frame B's textures are written by the copy queue: this queue does not start the pass before that copy is done.
    if (p.unpack != nullptr && p.unpack->GetCompletedValue() < (UINT64)p.unpack_value)
        (void)g_queue->Wait(p.unpack, (UINT64)p.unpack_value);
    ID3D12CommandList *const ls[1] = { g_list };
    g_queue->ExecuteCommandLists(1, ls);
    ++g_fence_value;
    (void)g_queue->Signal(g_fence, g_fence_value);
    g_alloc_value[par] = g_fence_value;
    ++g_parity;
    ++g_pairs; g_frames += n;
    return true;
}

ID3D12Resource *out(unsigned k) { return (k < MAX_OUT) ? g_out[k] : nullptr; }

void summary(char *line, size_t n)
{
    snprintf(line, n, "pairs warped %llu (%llu frame(s)) | not made: the pass before had not run %llu, no neural frame or vectors %llu | "
             "pairs without the depth test %llu, without the game's pixel for the fill %llu",
             g_pairs, g_frames, g_busy, g_bad_input, g_no_depth, g_no_game);
}

void release()
{
    g_up = false;
    if (g_queue != nullptr && g_fence != nullptr && g_fence_value != 0ull && g_fence->GetCompletedValue() < g_fence_value)
    {   // the last pass may still be running on the present queue
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev != nullptr)
        {
            if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fence_value, ev))) (void)WaitForSingleObject(ev, 500);
            CloseHandle(ev);
        }
    }
    relt(g_list);
    for (unsigned p = 0; p < PARITIES; ++p) relt(g_alloc[p]);
    relt(g_fence);
    for (unsigned k = 0; k < MAX_OUT; ++k) relt(g_out[k]);
    relt(g_heap);
    relt(g_pso);
    relt(g_rs);
    relt(g_queue);
    g_dev = nullptr;
}
}
