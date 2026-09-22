#include "render/Renderer.h"
#include "assets/Image.h"
#include "core/Log.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/Scene.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

namespace crate {

template <class T> static void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// One constant buffer shared by every draw. Matrices arrive row-major; HLSL's
// column-major reinterpretation + mul(M, v) yields the row-vector product the
// engine math uses.
constexpr int kMaxLights = 8;

struct GpuLight {
    float pos[4];    // xyz world position (point / spot)
    float dir[4];    // xyz forward direction (directional / spot)
    float color[4];  // rgb = colour * intensity, w = type (0 dir, 1 point, 2 spot)
    float params[4]; // x = range, y = cos(inner), z = cos(outer)
};

struct CBData {
    float mvp[16];
    float model[16];
    float lightVP[16]; // world -> shadow-casting light clip space
    float baseColor[4];
    float params[4];    // x=useTexture y=emissive z=unlit w=lightCount
    float ambient[4];   // rgb = ambient light
    float fogColor[4];
    float fogParams[4]; // x=start y=end z=enabled
    float fogHeight[4]; // x=base height y=falloff range (0 = disabled)
    float shadow[4];    // x=enabled y=receive z=1/mapSize w=bias
    float dither[4];    // x=enabled y=levels
    float cameraPos[4];    // xyz = world-space camera position (volume fill only)
    float volumeCenter[4]; // xyz = world-space volume center, w = shape (0 cube, 1 sphere)
    float volumeExtent[4]; // xyz = world-space per-axis half-size
    float depthUnproject[4]; // x=camera near y=camera far (volume fill only)
    GpuLight lights[kMaxLights];
};

static const char* kShaderSrc = R"(
#define MAX_LIGHTS 8
struct Light { float4 pos; float4 dir; float4 color; float4 params; };
cbuffer CB : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float4x4 uLightVP;
    float4   uBaseColor;
    float4   uParams;
    float4   uAmbient;
    float4   uFogColor;
    float4   uFogParams;
    float4   uFogHeight;
    float4   uShadow;
    float4   uDither;
    float4   uCameraPos;    // xyz = world-space camera position
    float4   uVolumeCenter; // xyz = world-space volume center, w = shape (0 cube, 1 sphere)
    float4   uVolumeExtent; // xyz = world-space per-axis half-size
    float4   uDepthUnproject; // x = camera near, y = camera far
    Light    uLights[MAX_LIGHTS];
};
Texture2D             uTex : register(t0);
SamplerState          uSamp : register(s0);
Texture2D             uShadowMap : register(t1);
SamplerComparisonState uShadowSamp : register(s1);
Texture2D             uDepthTex : register(t2); // scene depth; volume fill pass only

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0;
               float3 light : COLOR0; float fog : TEXCOORD1; float4 lpos : TEXCOORD2;
               float3 wpos : TEXCOORD3; float viewZ : TEXCOORD4; };

// Depth-only pass from the shadow light's point of view.
float4 VSShadow(VSIn i) : SV_POSITION
{
    float3 wpos = mul(uModel, float4(i.pos, 1.0)).xyz;
    return mul(uLightVP, float4(wpos, 1.0));
}

float3 shadeVertex(float3 wpos, float3 N)
{
    float3 acc = float3(0, 0, 0);
    int count = (int)uParams.w;
    [loop] for (int k = 0; k < count; ++k)
    {
        Light lt = uLights[k];
        int type = (int)lt.color.w;
        float3 Ldir;
        float atten = 1.0;
        if (type == 0)
        {
            Ldir = -normalize(lt.dir.xyz);
        }
        else
        {
            float3 toL = lt.pos.xyz - wpos;
            float d = length(toL);
            Ldir = toL / max(d, 1e-4);
            float f = saturate(1.0 - d / max(lt.params.x, 1e-4));
            atten = f * f;
            if (type == 2)
            {
                float cs = dot(-Ldir, normalize(lt.dir.xyz));
                atten *= saturate((cs - lt.params.z) / max(lt.params.y - lt.params.z, 1e-4));
            }
        }
        acc += lt.color.rgb * saturate(dot(N, Ldir)) * atten;
    }
    return acc;
}

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(uMVP, float4(i.pos, 1.0));
    o.viewZ = o.pos.w; // linear view-space Z, captured before the perspective
                       // divide -- SV_POSITION.w in the pixel shader is not
                       // this (it's a 1/w used for interpolation), so it has
                       // to travel as its own varying for PSVolume.
    float3 wpos = mul(uModel, float4(i.pos, 1.0)).xyz;
    float3 N = normalize(mul((float3x3)uModel, i.nrm));
    o.uv = i.uv;
    o.wpos = wpos;
    o.light = (uParams.z > 0.5) ? float3(1,1,1) : shadeVertex(wpos, N);
    float distFog = (uFogParams.z > 0.5)
              ? saturate((uFogParams.y - o.pos.w) / max(uFogParams.y - uFogParams.x, 1e-4))
              : 1.0;
    // Height fog: full density at/below uFogHeight.x, fading to none over the
    // next uFogHeight.y world units of altitude. y <= 0 disables the term
    // (heightFog stays 1, i.e. pure distance fog -- unchanged behaviour).
    float heightFog = (uFogParams.z > 0.5 && uFogHeight.y > 1e-4)
              ? saturate((wpos.y - uFogHeight.x) / uFogHeight.y)
              : 1.0;
    o.fog = saturate(distFog * heightFog);
    o.lpos = mul(uLightVP, float4(wpos, 1.0));
    return o;
}

float shadowFactor(float4 lpos)
{
    if (uShadow.x < 0.5 || uShadow.y < 0.5)
        return 1.0;
    float3 p = lpos.xyz / lpos.w;
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > 1.0)
        return 1.0;
    float ref = p.z - uShadow.w;
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            s += uShadowMap.SampleCmpLevelZero(uShadowSamp, uv + float2(x, y) * uShadow.z, ref);
    return s / 9.0;
}

static const float kBayer4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

// Ordered (Bayer) dither: quantizes each channel to uDither.y bands using a
// per-pixel threshold, so lighting bands/dithers instead of shading smoothly.
float3 ditherQuantize(float3 c, float2 screenPos)
{
    int bx = ((int)screenPos.x) & 3;
    int by = ((int)screenPos.y) & 3;
    float threshold = (kBayer4x4[by * 4 + bx] + 0.5) / 16.0;
    float levels = max(uDither.y - 1.0, 1.0);
    float3 scaled = c * levels;
    float3 base = floor(scaled);
    float3 frac = scaled - base;
    float3 stepped = base + step(threshold, frac);
    return stepped / levels;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 tex = lerp(float3(1,1,1), uTex.Sample(uSamp, i.uv).rgb, uParams.x);
    float3 lit = (uParams.z > 0.5) ? float3(1,1,1)
                                   : uAmbient.rgb + i.light * shadowFactor(i.lpos);
    if (uDither.x > 0.5)
        lit = ditherQuantize(saturate(lit), i.pos.xy);
    float3 col = uBaseColor.rgb * tex * lit + uParams.y;
    col = lerp(uFogColor.rgb, col, saturate(i.fog));
    return float4(col, uBaseColor.a);
}

// Fills a convex fog volume (cube or sphere) with a density that reacts to
// distance travelled *through* the shape, not just its surface -- rendered
// back-face-only (see drawVolumeFogs), so i.wpos is always the far/exit
// point of the view ray for this pixel, whether the camera is outside the
// shape (looking through both walls) or inside it (looking at just the far
// wall). The near/entry point isn't known from rasterization alone, so it's
// solved analytically in the shape's local space instead: an axis-aligned
// box slab test or a sphere quadratic, ignoring the volume's rotation (a
// deliberate simplification -- treats the shape as always axis-aligned for
// this fill test even if its mesh is drawn rotated).
float4 PSVolume(VSOut i) : SV_TARGET
{
    float3 toFrag = i.wpos - uCameraPos.xyz;
    float3 ext = max(abs(uVolumeExtent.xyz), 1e-4);
    float3 localCam = (uCameraPos.xyz - uVolumeCenter.xyz) / ext;
    float3 localDir = toFrag / ext;

    float tNear;
    if (uVolumeCenter.w < 0.5)
    {
        // Box, half-extent 1 in local space. Relies on IEEE-754 signed
        // infinity for an axis-aligned ray (localDir component == 0): the
        // 1e-6 epsilon this used to clamp to *lost the sign* of a small but
        // legitimately negative direction, silently flipping that axis'
        // slab bounds and corrupting tNear for any view angle near
        // axis-aligned. 1.0/0.0 already safely produces a correctly-signed
        // +-inf in HLSL, which min/max handle correctly with no epsilon
        // needed at all.
        float3 invD = 1.0 / localDir;
        float3 t0 = (-1.0 - localCam) * invD;
        float3 t1 = (1.0 - localCam) * invD;
        float3 tmn = min(t0, t1);
        tNear = max(max(tmn.x, tmn.y), tmn.z);
    }
    else
    {
        // Sphere, radius 1 in local space.
        float a = max(dot(localDir, localDir), 1e-8);
        float b = 2.0 * dot(localCam, localDir);
        float c = dot(localCam, localCam) - 1.0;
        float disc = b * b - 4.0 * a * c;
        tNear = (disc > 0.0) ? (-b - sqrt(disc)) / (2.0 * a) : 1.0;
    }
    tNear = saturate(tNear); // camera inside the shape -> entry at t = 0

    // Clamp the far end to whatever opaque surface is already at this pixel
    // (e.g. an object sitting inside the volume, or a wall poking through
    // it) instead of always reaching the shape's own back wall -- otherwise
    // fog would only ever show up where nothing solid interrupts it.
    // i.viewZ is this ray's linear view-space depth at t = 1 (the rasterized
    // back-face point); view-space Z is an affine function of t along any
    // ray from the camera, and it's 0 at the camera (t = 0), so viewZ(t) =
    // t * i.viewZ -- letting the scene's own linear depth be inverted
    // straight back to a t value on this same ray without any matrix work.
    float sceneDevice = uDepthTex.Load(int3((int2)i.pos.xy, 0)).r;
    float sceneViewZ = (uDepthUnproject.x * uDepthUnproject.y) /
                       max(uDepthUnproject.y - sceneDevice * (uDepthUnproject.y - uDepthUnproject.x), 1e-4);
    float tFar = (i.viewZ > 1e-4) ? saturate(sceneViewZ / i.viewZ) : 1.0;

    float thickness = length(toFrag) * max(tFar - tNear, 0.0);
    // uBaseColor.a is density (0..1 from the inspector). The old *2.0 factor
    // made even a modest thickness (a handful of world units through a
    // scaled-up volume) saturate alpha to ~0.95+, so the volume read as a
    // flat, nearly opaque blob -- any lighting variation in `lit` below all
    // but disappears once alpha is that close to 1, since the blended
    // result is then almost entirely `col` with barely any of the scene
    // showing through. No multiplier lets density read closer to its literal
    // 0..1 meaning (density 1.0 needs ~4-5 units of thickness to fully
    // saturate instead of ~1.5).
    float alpha = saturate(1.0 - exp(-uBaseColor.a * thickness));

    // No real surface normal inside a volume; sample lighting at a handful of
    // points spread along the visible ray segment (not just its midpoint --
    // task 133) with a fixed up-facing normal, taking the brightest sample.
    // A single midpoint sample badly undersamples a beam that only crosses
    // PART of a long ray (e.g. looking down a corridor with a spotlight off
    // to one side): most rays' one sample point simply missed the cone
    // entirely, which was the main reason a spotlight barely registered in
    // fog even at high range -- increasing range grew the cone, but a
    // single-point sample per ray still had to get lucky to land inside it.
    // Taking the max across several samples means any ray that crosses the
    // beam ANYWHERE along its length shows it.
    const int kFogLightSamples = 5;
    float3 brightestLight = float3(0, 0, 0);
    [unroll]
    for (int s = 0; s < kFogLightSamples; ++s)
    {
        float t = tNear + (tFar - tNear) * (float(s) + 0.5) / float(kFogLightSamples);
        float3 samplePos = uCameraPos.xyz + toFrag * t;
        brightestLight = max(brightestLight, shadeVertex(samplePos, float3(0, 1, 0)));
    }

    // Ambient-only fog colour (task 104): capped to the artist's own
    // uBaseColor brightness, unchanged behaviour for fog that no light is
    // reaching -- denser fog there still only ever obscures/dims, never
    // brightens, exactly like before.
    float3 ambientBase = uBaseColor.rgb * saturate(uAmbient.rgb);
    // Direct light contribution is intentionally NOT multiplied by (or
    // capped to) uBaseColor's dimness (task 133): fog scatters a light's OWN
    // colour toward the viewer, it doesn't just make the artist's dim
    // ambient fog tint slightly less dim -- a bright spotlight punching
    // through dark fog should read as a genuinely bright, visibly-coloured
    // beam, which is what actually makes it useful as a flashlight. Only
    // guarded against runaway values feeding the render target, not capped
    // to look dim.
    float3 col = min(ambientBase + brightestLight, float3(4.0, 4.0, 4.0));
    return float4(col, alpha);
}
)";

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(ID3D11Device* device, ID3D11DeviceContext* context) {
    device_ = device;
    ctx_ = context;
    if (!device_ || !ctx_)
        return false;
    if (!compileShaders()) {
        CR_ERROR("render", "Shader compilation failed");
        return false;
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(CBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cbd, nullptr, &cb_);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // primitives use mixed winding; revisit with real meshes
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &raster_);

    // Volume fill pass: draw only back-facing triangles (see drawVolumeFogs).
    D3D11_RASTERIZER_DESC rdCullFront = rd;
    rdCullFront.CullMode = D3D11_CULL_FRONT;
    device_->CreateRasterizerState(&rdCullFront, &rasterCullFront_);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    device_->CreateDepthStencilState(&dsd, &depthState_);

    // Volumetric fog pass: no depth-stencil view is bound at all during this
    // pass (see drawVolumeFogs), so the fixed-function test is disabled --
    // PSVolume samples scene depth as a texture instead and does the
    // occlusion clamp itself, including full occlusion (thickness clamps to
    // 0), not just the partial case a hardware test alone couldn't express.
    D3D11_DEPTH_STENCIL_DESC dsdNoWrite = dsd;
    dsdNoWrite.DepthEnable = FALSE;
    dsdNoWrite.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device_->CreateDepthStencilState(&dsdNoWrite, &depthNoWrite_);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bd, &blendAlpha_);

    // Shadow map: comparison sampler (clamp, white outside), depth-bias raster.
    D3D11_SAMPLER_DESC ssd = {};
    ssd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    ssd.AddressU = ssd.AddressV = ssd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    ssd.BorderColor[0] = ssd.BorderColor[1] = ssd.BorderColor[2] = ssd.BorderColor[3] = 1.0f;
    ssd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    ssd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&ssd, &shadowSamp_);

    D3D11_RASTERIZER_DESC srd = {};
    srd.FillMode = D3D11_FILL_SOLID;
    srd.CullMode = D3D11_CULL_NONE;
    srd.DepthClipEnable = TRUE;
    srd.DepthBias = 120;
    srd.SlopeScaledDepthBias = 3.5f;
    device_->CreateRasterizerState(&srd, &shadowRaster_);
    ensureShadowMap();

    // 1x1 white + an 8x8 checker for "no texture" and "textured" states.
    auto makeTex = [&](const uint32_t* pixels, int w, int h) -> ID3D11ShaderResourceView* {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd = {};
        srd.pSysMem = pixels;
        srd.SysMemPitch = w * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device_->CreateTexture2D(&td, &srd, &tex)))
            return nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        device_->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
        return srv;
    };
    uint32_t white = 0xFFFFFFFFu;
    whiteSrv_ = makeTex(&white, 1, 1);
    uint32_t checker[64];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            checker[y * 8 + x] = ((x ^ y) & 1) ? 0xFF3A3A3Au : 0xFFAAAAAAu;
    checkerSrv_ = makeTex(checker, 8, 8);

    CR_LOG("render", "Renderer initialised (D3D11)");
    return true;
}

void Renderer::shutdown() {
    releaseTargets();
    for (auto& [k, m] : meshCache_) {
        safeRelease(m.vb);
        safeRelease(m.ib);
    }
    meshCache_.clear();
    for (auto& [k, srv] : textureCache_)
        safeRelease(srv);
    textureCache_.clear();
    safeRelease(whiteSrv_);
    safeRelease(checkerSrv_);
    safeRelease(vs_);
    safeRelease(ps_);
    safeRelease(psVolume_);
    safeRelease(vsShadow_);
    safeRelease(layout_);
    safeRelease(cb_);
    safeRelease(sampler_);
    safeRelease(raster_);
    safeRelease(depthState_);
    safeRelease(blendAlpha_);
    safeRelease(depthNoWrite_);
    safeRelease(rasterCullFront_);
    safeRelease(shadowTex_);
    safeRelease(shadowDsv_);
    safeRelease(shadowSrv_);
    safeRelease(shadowSamp_);
    safeRelease(shadowRaster_);
    device_ = nullptr;
    ctx_ = nullptr;
}

bool Renderer::compileShaders() {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "crate.hlsl", nullptr, nullptr,
                          "VSMain", "vs_4_0", flags, 0, &vsb, &err))) {
        if (err)
            CR_ERROR("render", std::string("VS: ") + static_cast<const char*>(err->GetBufferPointer()));
        safeRelease(err);
        return false;
    }
    if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "crate.hlsl", nullptr, nullptr,
                          "PSMain", "ps_4_0", flags, 0, &psb, &err))) {
        if (err)
            CR_ERROR("render", std::string("PS: ") + static_cast<const char*>(err->GetBufferPointer()));
        safeRelease(err);
        safeRelease(vsb);
        return false;
    }

    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);

    ID3DBlob* pvb = nullptr;
    if (SUCCEEDED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "crate.hlsl", nullptr, nullptr,
                             "PSVolume", "ps_4_0", flags, 0, &pvb, &err))) {
        device_->CreatePixelShader(pvb->GetBufferPointer(), pvb->GetBufferSize(), nullptr,
                                   &psVolume_);
        safeRelease(pvb);
    } else {
        if (err)
            CR_ERROR("render",
                     std::string("PSVolume: ") + static_cast<const char*>(err->GetBufferPointer()));
        safeRelease(err);
    }

    ID3DBlob* svsb = nullptr;
    if (SUCCEEDED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "crate.hlsl", nullptr, nullptr,
                             "VSShadow", "vs_4_0", flags, 0, &svsb, &err))) {
        device_->CreateVertexShader(svsb->GetBufferPointer(), svsb->GetBufferSize(), nullptr,
                                    &vsShadow_);
        safeRelease(svsb);
    } else {
        if (err)
            CR_ERROR("render",
                     std::string("VSShadow: ") + static_cast<const char*>(err->GetBufferPointer()));
        safeRelease(err);
    }

    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(elems, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);
    safeRelease(vsb);
    safeRelease(psb);
    return vs_ && ps_ && layout_;
}

void Renderer::releaseTargets() {
    safeRelease(colorSrv_);
    safeRelease(colorRtv_);
    safeRelease(colorTex_);
    safeRelease(depthDsv_);
    safeRelease(depthSrv_);
    safeRelease(depthTex_);
    rtWidth_ = rtHeight_ = 0;
}

bool Renderer::ensureTargets(int w, int h) {
    if (w == rtWidth_ && h == rtHeight_ && colorRtv_)
        return true;
    releaseTargets();
    if (w <= 0 || h <= 0)
        return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &colorTex_)))
        return false;
    device_->CreateRenderTargetView(colorTex_, nullptr, &colorRtv_);
    device_->CreateShaderResourceView(colorTex_, nullptr, &colorSrv_);

    // Typeless + shader-resource bindable so the volume fog pass can sample
    // it (see drawVolumeFogs) -- no stencil test is actually used anywhere,
    // so a plain 32-bit float depth (matching the shadow map's own format)
    // covers it.
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&dd, nullptr, &depthTex_)))
        return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthTex_, &dvd, &depthDsv_);
    D3D11_SHADER_RESOURCE_VIEW_DESC dsrvd = {};
    dsrvd.Format = DXGI_FORMAT_R32_FLOAT;
    dsrvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    dsrvd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(depthTex_, &dsrvd, &depthSrv_);

    rtWidth_ = w;
    rtHeight_ = h;
    return true;
}

Renderer::GpuMesh Renderer::upload(const MeshData& data) {
    GpuMesh m;
    m.indexCount = static_cast<uint32_t>(data.indices.size());

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = static_cast<UINT>(data.vertices.size() * sizeof(Vertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = {data.vertices.data(), 0, 0};
    device_->CreateBuffer(&vbd, &vsd, &m.vb);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = static_cast<UINT>(data.indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = {data.indices.data(), 0, 0};
    device_->CreateBuffer(&ibd, &isd, &m.ib);
    return m;
}

const Renderer::GpuMesh& Renderer::meshFor(const std::string& key,
                                          const std::string& primitiveFallback) {
    const std::string& cacheKey = key.empty() ? primitiveFallback : key;
    auto it = meshCache_.find(cacheKey);
    if (it != meshCache_.end())
        return it->second;

    const MeshData* imported = (!key.empty() && library_) ? library_->findMesh(key) : nullptr;
    GpuMesh m = upload(imported ? *imported : primitives::byName(primitiveFallback));
    return meshCache_.emplace(cacheKey, m).first->second;
}

void Renderer::invalidateMesh(const std::string& key) {
    auto it = meshCache_.find(key);
    if (it != meshCache_.end()) {
        safeRelease(it->second.vb);
        safeRelease(it->second.ib);
        meshCache_.erase(it);
    }
}

void* Renderer::loadTexture(const std::string& path) {
    if (path.empty())
        return checkerSrv_;
    auto it = textureCache_.find(path);
    if (it != textureCache_.end())
        return it->second;

    Image image = loadImage(path);
    if (!image.valid()) {
        textureCache_[path] = nullptr;
        return checkerSrv_;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(image.width);
    td.Height = static_cast<UINT>(image.height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = {image.rgba.data(), static_cast<UINT>(image.width * 4), 0};
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(device_->CreateTexture2D(&td, &srd, &tex))) {
        device_->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
    }
    textureCache_[path] = srv;
    CR_LOG("assets", "Loaded texture " + path + " (" + std::to_string(image.width) + "x" +
                         std::to_string(image.height) + ")");
    return srv ? static_cast<void*>(srv) : checkerSrv_;
}

void Renderer::invalidateTexture(const std::string& path) {
    auto it = textureCache_.find(path);
    if (it != textureCache_.end()) {
        safeRelease(it->second);
        textureCache_.erase(it);
    }
}

void Renderer::drawActor(Actor& actor, const Mat4& viewProj, const Options& opt, bool shadowPass) {
    for (const auto& child : actor.children())
        drawActor(*child, viewProj, opt, shadowPass);

    auto* mr = actor.getComponent<MeshRenderer>();
    if (!mr || !mr->enabled || !actor.visible())
        return;
    if (shadowPass && !mr->castShadows)
        return;

    Transform w = actor.worldTransform();
    // Primitives are sized from the component's own fields (task 84), not
    // Transform.scale, so a later physics collider can read the same numbers
    // without also inheriting whatever the transform's scale is used for
    // elsewhere. Imported models have no such fields yet and keep using
    // Transform.scale as before.
    Vec3 meshScale = w.scale;
    if (mr->usePrimitive) {
        const std::string& p = mr->primitive;
        if (p == "Sphere")
            meshScale = Vec3{mr->radius * 2.0f, mr->radius * 2.0f, mr->radius * 2.0f};
        else if (p == "Cylinder" || p == "Capsule")
            meshScale = Vec3{mr->radius * 2.0f, mr->height, mr->radius * 2.0f};
        else if (p == "Plane")
            meshScale = Vec3{mr->planeSize[0], 1.0f, mr->planeSize[1]};
        else if (p == "Quad")
            meshScale = Vec3{mr->planeSize[0], mr->planeSize[1], 1.0f};
        else // Cube
            meshScale = Vec3{mr->boxSize[0], mr->boxSize[1], mr->boxSize[2]};
    }
    Mat4 model = Mat4::scale(meshScale) * Mat4::rotationEuler(w.rotationEuler) *
                 Mat4::translation(w.position);
    Mat4 mvp = model * viewProj;

    // Resolve material: a referenced Material asset wins; otherwise fall back to
    // the MeshRenderer's own tint + texture.
    float baseColor[4] = {mr->tint[0] * 0.78f, mr->tint[1] * 0.78f, mr->tint[2] * 0.80f,
                          mr->tint[3]};
    std::string texPath = mr->texturePath;
    float emissive = 0.0f;
    bool unlit = false;
    bool dither = false;
    float ditherLevels = 4.0f;
    if (materials_ && !mr->materialRef.empty()) {
        if (const Material* mat = materials_->find(mr->materialRef)) {
            for (int i = 0; i < 4; ++i)
                baseColor[i] = mat->baseColor[i] * mr->tint[i];
            texPath = mat->texturePath.empty() ? mr->texturePath : mat->texturePath;
            emissive = mat->emissive;
            unlit = mat->unlit;
            dither = mat->dither;
            ditherLevels = mat->ditherLevels;
        }
    }

    CBData cb = {};
    // Upload row-major data as-is: HLSL's column-major reinterpretation plus
    // mul(M, v) then yields the row-vector product v * M that this math uses.
    std::memcpy(cb.mvp, mvp.m, sizeof(cb.mvp));
    std::memcpy(cb.model, model.m, sizeof(cb.model));
    std::memcpy(cb.lightVP, shadowVP_.m, sizeof(cb.lightVP));
    cb.shadow[0] = shadowActive_ ? 1.0f : 0.0f;
    cb.shadow[1] = mr->receiveShadows ? 1.0f : 0.0f;
    cb.shadow[2] = 1.0f / static_cast<float>(kShadowSize);
    cb.shadow[3] = 0.004f;
    cb.dither[0] = dither ? 1.0f : 0.0f;
    cb.dither[1] = ditherLevels;
    bool sel = opt.highlight == &actor;
    cb.baseColor[0] = sel ? baseColor[0] * 0.85f + 0.10f : baseColor[0];
    cb.baseColor[1] = sel ? baseColor[1] * 0.85f + 0.05f : baseColor[1];
    cb.baseColor[2] = sel ? baseColor[2] * 0.85f + 0.25f : baseColor[2];
    cb.baseColor[3] = baseColor[3];

    // Static lights don't reach the real-time list; their effect only shows up
    // via a bake (below). Everything else is uploaded, capped at kMaxLights.
    int lightCount = 0;
    for (const auto& s : lights_) {
        if (s.isStatic)
            continue;
        if (lightCount >= kMaxLights)
            break;
        GpuLight& g = cb.lights[lightCount];
        g.pos[0] = s.pos.x; g.pos[1] = s.pos.y; g.pos[2] = s.pos.z;
        g.dir[0] = s.dir.x; g.dir[1] = s.dir.y; g.dir[2] = s.dir.z;
        g.color[0] = s.color.x; g.color[1] = s.color.y; g.color[2] = s.color.z;
        g.color[3] = static_cast<float>(s.type);
        g.params[0] = s.range; g.params[1] = s.cosInner; g.params[2] = s.cosOuter;
        ++lightCount;
    }
    cb.params[0] = texPath.empty() ? 0.0f : 1.0f;
    cb.params[1] = emissive;
    cb.params[2] = unlit ? 1.0f : 0.0f;
    cb.params[3] = static_cast<float>(lightCount);

    // Baked static-light contribution: the mesh's own bake if it has one,
    // otherwise the nearest baked LightProbe (for actors that move at runtime).
    Vec3 baked{0, 0, 0};
    if (mr->bakedValid) {
        baked = {mr->bakedLight[0], mr->bakedLight[1], mr->bakedLight[2]};
    } else if (!probes_.empty()) {
        const ProbeSample* nearest = &probes_[0];
        float best = length(probes_[0].pos - w.position);
        for (const auto& p : probes_) {
            float d = length(p.pos - w.position);
            if (d < best) { best = d; nearest = &p; }
        }
        baked = nearest->baked;
    }
    cb.ambient[0] = opt.ambient.x + baked.x;
    cb.ambient[1] = opt.ambient.y + baked.y;
    cb.ambient[2] = opt.ambient.z + baked.z;
    cb.fogColor[0] = opt.fogColor.x; cb.fogColor[1] = opt.fogColor.y; cb.fogColor[2] = opt.fogColor.z;
    cb.fogParams[0] = opt.fogStart;
    cb.fogParams[1] = opt.fogEnd;
    cb.fogParams[2] = opt.fogEnabled ? 1.0f : 0.0f;
    cb.fogHeight[0] = opt.fogHeightBase;
    cb.fogHeight[1] = opt.fogHeightRange;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof(cb));
        ctx_->Unmap(cb_, 0);
    }

    if (!shadowPass) {
        auto* srv = static_cast<ID3D11ShaderResourceView*>(texPath.empty() ? whiteSrv_
                                                                           : loadTexture(texPath));
        ctx_->PSSetShaderResources(0, 1, &srv);
    }

    const GpuMesh& gm =
        meshFor(mr->meshKey(), mr->primitive.empty() ? "Cube" : mr->primitive);
    UINT stride = sizeof(Vertex), offset = 0;
    ctx_->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
    ctx_->IASetIndexBuffer(gm.ib, DXGI_FORMAT_R32_UINT, 0);
    ctx_->DrawIndexed(gm.indexCount, 0, 0);
}

bool Renderer::ensureShadowMap() {
    if (shadowTex_)
        return true;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = kShadowSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &shadowTex_)))
        return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(shadowTex_, &dvd, &shadowDsv_);
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R32_FLOAT;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(shadowTex_, &svd, &shadowSrv_);
    return shadowDsv_ && shadowSrv_;
}

// Build the world -> light clip matrix for the first shadow-casting light
// (directional or spot). Returns false when there is none.
bool Renderer::computeShadowVP(Mat4& out) const {
    for (const auto& s : lights_) {
        if (s.type == 0) { // directional: ortho box around the origin
            Vec3 dir = normalize(s.dir);
            Vec3 up = (std::fabs)(dir.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            Vec3 center{0, 0.5f, 0};
            Mat4 view = Mat4::lookAtLH(center - dir * 30.0f, center, up);
            out = view * Mat4::orthoLH(40.0f, 40.0f, 0.1f, 70.0f);
            return true;
        }
        if (s.type == 2) { // spot: perspective from the light
            Vec3 dir = normalize(s.dir);
            Vec3 up = (std::fabs)(dir.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            Mat4 view = Mat4::lookAtLH(s.pos, s.pos + dir, up);
            float fov = 2.0f * std::acos((std::max)(0.05f, s.cosOuter)) * (180.0f / kPi);
            out = view * Mat4::perspectiveLH(fov, 1.0f, 0.1f, (std::max)(2.0f, s.range));
            return true;
        }
    }
    return false;
}

namespace {
// Turn a scene name into a filesystem-safe basename ("My Scene" -> "My_Scene").
std::string sanitizeFileName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out.empty() ? std::string("scene") : out;
}
} // namespace

std::string Renderer::lightmapPath(const Scene& scene, const std::string& assetDir) {
    return assetDir + "/lightmaps/" + sanitizeFileName(scene.name()) + ".lightmap";
}

void Renderer::bakeLighting(Scene& scene, const std::string& assetDir) {
    lights_.clear();
    probes_.clear();
    for (const auto& child : scene.root().children())
        collectLights(*child);

    const Vec3 N{0, 1, 0}; // coarse per-object bake: assumes an up-facing normal
    auto eval = [&](const Vec3& p) -> Vec3 {
        Vec3 acc{0, 0, 0};
        for (const auto& s : lights_) {
            if (!s.isStatic)
                continue;
            Vec3 Ldir;
            float atten = 1.0f;
            if (s.type == 0) {
                Ldir = s.dir * -1.0f;
            } else {
                Vec3 toL = s.pos - p;
                float d = length(toL);
                Ldir = d > 1e-4f ? toL * (1.0f / d) : Vec3{0, 1, 0};
                float f = (std::max)(0.0f, 1.0f - d / (std::max)(s.range, 1e-4f));
                atten = f * f;
                if (s.type == 2) {
                    float cs = dot(Ldir * -1.0f, s.dir);
                    float denom = (std::max)(s.cosInner - s.cosOuter, 1e-4f);
                    atten *= (std::max)(0.0f, (std::min)(1.0f, (cs - s.cosOuter) / denom));
                }
            }
            float ndl = (std::max)(0.0f, dot(N, Ldir));
            acc = acc + s.color * (ndl * atten);
        }
        return acc;
    };

    std::function<void(Actor&)> walk = [&](Actor& a) {
        if (auto* mr = a.getComponent<MeshRenderer>()) {
            Vec3 c = eval(a.worldTransform().position);
            mr->bakedLight[0] = c.x; mr->bakedLight[1] = c.y; mr->bakedLight[2] = c.z;
            mr->bakedValid = true;
        }
        if (auto* lp = a.getComponent<LightProbeComponent>()) {
            Vec3 c = eval(a.worldTransform().position);
            lp->bakedLight[0] = c.x; lp->bakedLight[1] = c.y; lp->bakedLight[2] = c.z;
            lp->bakedValid = true;
        }
        for (const auto& ch : a.children())
            walk(*ch);
    };
    for (const auto& ch : scene.root().children())
        walk(*ch);

    // Persist the bake to disk (task 59: "lightmap baking" / "lightmaps") so it
    // is picked up again next time this scene is opened, without re-baking.
    std::string path = lightmapPath(scene, assetDir);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) {
        std::function<void(Actor&)> write = [&](Actor& a) {
            std::string p = scene.pathOf(&a);
            if (auto* mr = a.getComponent<MeshRenderer>(); mr && mr->bakedValid) {
                out << "MESH\t" << p << '\t' << mr->bakedLight[0] << '\t' << mr->bakedLight[1]
                    << '\t' << mr->bakedLight[2] << '\n';
            }
            if (auto* lp = a.getComponent<LightProbeComponent>(); lp && lp->bakedValid) {
                out << "PROBE\t" << p << '\t' << lp->bakedLight[0] << '\t' << lp->bakedLight[1]
                    << '\t' << lp->bakedLight[2] << '\n';
            }
            for (const auto& ch2 : a.children())
                write(*ch2);
        };
        for (const auto& ch : scene.root().children())
            write(*ch);
        CR_LOG("render", "Baked static lighting -> " + path);
    } else {
        CR_ERROR("render", "Baked static lighting but could not write " + path);
    }
}

bool Renderer::loadLightmap(Scene& scene, const std::string& assetDir) {
    std::string path = lightmapPath(scene, assetDir);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    bool any = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        // Fields are tab-separated: KIND \t ACTOR_PATH \t R \t G \t B.
        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos
                                                                           : tab - start));
            if (tab == std::string::npos)
                break;
            start = tab + 1;
        }
        if (fields.size() != 5)
            continue;
        const std::string& kind = fields[0];
        const std::string& actorPath = fields[1];
        float r = std::strtof(fields[2].c_str(), nullptr);
        float g = std::strtof(fields[3].c_str(), nullptr);
        float b = std::strtof(fields[4].c_str(), nullptr);

        Actor* actor = scene.atPath(actorPath);
        if (!actor)
            continue;
        if (kind == "MESH") {
            if (auto* mr = actor->getComponent<MeshRenderer>()) {
                mr->bakedLight[0] = r; mr->bakedLight[1] = g; mr->bakedLight[2] = b;
                mr->bakedValid = true;
                any = true;
            }
        } else if (kind == "PROBE") {
            if (auto* lp = actor->getComponent<LightProbeComponent>()) {
                lp->bakedLight[0] = r; lp->bakedLight[1] = g; lp->bakedLight[2] = b;
                lp->bakedValid = true;
                any = true;
            }
        }
    }
    if (any)
        CR_LOG("render", "Loaded baked lightmap <- " + path);
    return any;
}

void Renderer::collectLights(Actor& actor) {
    if (auto* lc = actor.getComponent<LightComponent>()) {
        if (lc->enabled && actor.visible() && lights_.size() < kMaxLights) {
            Transform w = actor.worldTransform();
            Mat4 rot = Mat4::rotationEuler(w.rotationEuler);
            LightSample s;
            s.type = static_cast<int>(lc->type);
            s.pos = w.position;
            // The light shines along its local +Z (same as the actor normal):
            // row-vector (0,0,1) * R == row 2 of R.
            s.dir = normalize(Vec3{rot.at(2, 0), rot.at(2, 1), rot.at(2, 2)});
            s.color = Vec3{lc->color[0], lc->color[1], lc->color[2]} * lc->intensity;
            s.range = lc->range;
            s.cosInner = std::cos(radians(lc->spotInnerDeg));
            s.cosOuter = std::cos(radians(lc->spotOuterDeg));
            s.isStatic = lc->isStatic;
            lights_.push_back(s);
        }
    }
    if (auto* lp = actor.getComponent<LightProbeComponent>())
        if (lp->bakedValid)
            probes_.push_back({actor.worldTransform().position,
                               Vec3{lp->bakedLight[0], lp->bakedLight[1], lp->bakedLight[2]}});
    for (const auto& child : actor.children())
        collectLights(*child);
}

// First enabled FogComponent anywhere in the tree wins; siblings/descendants
// past that point are left unvisited once found (there's only one scene fog).
void Renderer::collectFog(Actor& actor) {
    if (fogSample_.found)
        return;
    if (auto* fc = actor.getComponent<FogComponent>()) {
        if (fc->enabled) {
            fogSample_.found = true;
            fogSample_.color = Vec3{fc->color[0], fc->color[1], fc->color[2]};
            fogSample_.start = fc->start;
            fogSample_.end = fc->end;
            fogSample_.heightBase = actor.worldTransform().position.y;
            fogSample_.heightRange = fc->heightRange;
            return;
        }
    }
    for (const auto& child : actor.children())
        collectFog(*child);
}

// Like collectFog(): only the first enabled VolumetricFogComponent anywhere
// in the tree matters, and its actor's transform is irrelevant (task 75,
// redesigned to be global fog rather than a bounded shape).
void Renderer::collectVolumeFog(Actor& actor) {
    if (volumeFogSample_.found)
        return;
    if (auto* vf = actor.getComponent<VolumetricFogComponent>()) {
        if (vf->enabled) {
            volumeFogSample_.found = true;
            volumeFogSample_.color = Vec3{vf->color[0], vf->color[1], vf->color[2]};
            volumeFogSample_.density = vf->density;
            return;
        }
    }
    for (const auto& child : actor.children())
        collectVolumeFog(*child);
}

// Fills the whole world with fog that reacts to light (task 75). There's no
// user-placed shape any more: this draws one enormous cube centered on the
// camera (half-extent well inside the far clip plane, so its own back wall
// never gets clipped) and reuses the existing back-face-only volume fill
// technique -- with the camera always deep inside a cube that large, PSVolume
// always resolves tNear to 0 and lets the scene's own depth buffer (sampled
// via depthSrv_) clamp tFar at whatever opaque surface is actually visible,
// which is exactly "fog everywhere, reacting to light, up to the first solid
// thing in the way".
void Renderer::drawVolumeFogs(const Mat4& viewProj, const Vec3& cameraPos, float nearZ,
                              float farZ, const Options& opt) {
    // density<=0 means "no fog" and must draw nothing: at exactly 0, PSVolume's
    // alpha (saturate(1 - exp(-density*thickness))) is mathematically 0 and
    // should blend as a no-op, but empirically the pass instead renders solid
    // black across the whole screen at that exact value (confirmed via direct
    // screenshot repro; not fully root-caused at the shader-assembly level --
    // skipping the draw entirely sidesteps whatever precision/edge-case
    // produces that result, and is also strictly cheaper than drawing a
    // full-screen pass guaranteed to contribute nothing).
    // opt.fogEnabled is the Scene/Game View "Fog" checkbox (task 143): before
    // this fix it only gated the older plain distance/height Fog component's
    // uFogParams.z term (see the fogParams[2] write in render()) and this
    // volumetric pass ignored it entirely, so toggling the checkbox visibly
    // did nothing whenever a VolumetricFogComponent was present -- which is
    // the common case (the default sample scene uses one). The checkbox is
    // now a genuine master on/off for whichever fog system is actually in
    // the scene.
    if (!opt.fogEnabled || !volumeFogSample_.found || !psVolume_ || !depthSrv_ ||
        volumeFogSample_.density <= 0.0f)
        return;
    // No depth-stencil view bound (a resource can't be a depth target and a
    // shader resource at once): a hardware test here would compare against
    // the *volume's own* back-face depth, so it would reject the fragment
    // outright whenever anything -- including an object meant to be
    // partially fogged -- is nearer than that back wall, before PSVolume's
    // partial-thickness clamp ever got to run. The two aren't a
    // coarse-then-fine pair; they directly contradict each other, and the
    // manual depth-sample clamp below already covers full occlusion too
    // (tFar ends up <= tNear, thickness 0).
    ctx_->OMSetRenderTargets(1, &colorRtv_, nullptr);
    ctx_->RSSetState(rasterCullFront_);
    ctx_->OMSetDepthStencilState(depthNoWrite_, 0);
    ctx_->OMSetBlendState(blendAlpha_, nullptr, 0xFFFFFFFF);
    ctx_->PSSetShader(psVolume_, nullptr, 0);
    ID3D11ShaderResourceView* whiteSrv = whiteSrv_;
    ctx_->PSSetShaderResources(0, 1, &whiteSrv);
    ctx_->PSSetShaderResources(2, 1, &depthSrv_);

    CBData cb = {};
    int lightCount = 0;
    for (const auto& s : lights_) {
        if (s.isStatic || lightCount >= kMaxLights)
            continue;
        GpuLight& g = cb.lights[lightCount];
        g.pos[0] = s.pos.x; g.pos[1] = s.pos.y; g.pos[2] = s.pos.z;
        g.dir[0] = s.dir.x; g.dir[1] = s.dir.y; g.dir[2] = s.dir.z;
        g.color[0] = s.color.x; g.color[1] = s.color.y; g.color[2] = s.color.z;
        g.color[3] = static_cast<float>(s.type);
        g.params[0] = s.range; g.params[1] = s.cosInner; g.params[2] = s.cosOuter;
        ++lightCount;
    }
    cb.params[3] = static_cast<float>(lightCount);
    cb.ambient[0] = opt.ambient.x; cb.ambient[1] = opt.ambient.y; cb.ambient[2] = opt.ambient.z;
    cb.cameraPos[0] = cameraPos.x; cb.cameraPos[1] = cameraPos.y; cb.cameraPos[2] = cameraPos.z;
    cb.depthUnproject[0] = nearZ; cb.depthUnproject[1] = farZ;

    // Half-extent kept well under farZ: a cube's corners are up to sqrt(3)
    // times farther from its center than its faces are, and it's the corner
    // directions (view-space Z, not full 3D distance) that must stay inside
    // the projection's far clip or that part of the back wall is clipped
    // away and leaves an unfogged hole. 0.5 gives a large safety margin.
    float half = farZ * 0.5f;
    Mat4 model = Mat4::scale(Vec3{half * 2.0f, half * 2.0f, half * 2.0f}) *
                Mat4::translation(cameraPos);
    Mat4 mvp = model * viewProj;
    std::memcpy(cb.mvp, mvp.m, sizeof(cb.mvp));
    std::memcpy(cb.model, model.m, sizeof(cb.model));
    cb.baseColor[0] = volumeFogSample_.color.x; cb.baseColor[1] = volumeFogSample_.color.y;
    cb.baseColor[2] = volumeFogSample_.color.z; cb.baseColor[3] = volumeFogSample_.density;
    cb.volumeCenter[0] = cameraPos.x; cb.volumeCenter[1] = cameraPos.y;
    cb.volumeCenter[2] = cameraPos.z; cb.volumeCenter[3] = 0.0f; // always the cube branch
    cb.volumeExtent[0] = half; cb.volumeExtent[1] = half; cb.volumeExtent[2] = half;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof(cb));
        ctx_->Unmap(cb_, 0);
    }

    const GpuMesh& gm = meshFor("", "Cube");
    UINT stride = sizeof(Vertex), offset = 0;
    ctx_->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
    ctx_->IASetIndexBuffer(gm.ib, DXGI_FORMAT_R32_UINT, 0);
    ctx_->DrawIndexed(gm.indexCount, 0, 0);

    ID3D11ShaderResourceView* noSrv = nullptr;
    ctx_->PSSetShaderResources(2, 1, &noSrv); // free depthSrv_'s resource for DSV use again
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->RSSetState(raster_);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(depthState_, 0);
    ctx_->OMSetRenderTargets(1, &colorRtv_, depthDsv_);
}

void* Renderer::render(Scene& scene, const OrbitCamera& cam, int width, int height,
                       const Options& opt) {
    float aspect = height > 0 ? float(width) / float(height) : 1.0f;
    return render(scene, cam.view(), cam.proj(aspect), cam.eye(), cam.nearZ, cam.farZ, width,
                 height, opt);
}

void* Renderer::render(Scene& scene, const Mat4& view, const Mat4& proj, const Vec3& eye,
                       float nearZ, float farZ, int width, int height, const Options& opt) {
    if (!ready() || !ensureTargets(width, height))
        return nullptr;

    lights_.clear();
    probes_.clear();
    fogSample_ = FogSample{};
    volumeFogSample_ = VolumeFogSample{};
    for (const auto& child : scene.root().children()) {
        collectLights(*child);
        collectFog(*child);
        collectVolumeFog(*child);
    }
    // A FogComponent in the scene wins over the editor's manual fog toggle.
    Options resolved = opt;
    if (fogSample_.found) {
        resolved.fogEnabled = true;
        resolved.fogColor = fogSample_.color;
        resolved.fogStart = fogSample_.start;
        resolved.fogEnd = fogSample_.end;
        resolved.fogHeightBase = fogSample_.heightBase;
        resolved.fogHeightRange = fogSample_.heightRange;
    }
    shadowActive_ =
        resolved.shadows && ensureShadowMap() && vsShadow_ && computeShadowVP(shadowVP_);

    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(layout_);
    ctx_->VSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetConstantBuffers(0, 1, &cb_);
    ctx_->OMSetDepthStencilState(depthState_, 0);

    // --- Shadow depth pass (from the shadow light) -------------------------
    if (shadowActive_) {
        ID3D11ShaderResourceView* noSrv[2] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, noSrv); // release t1 from the main pass
        ID3D11RenderTargetView* noRtv = nullptr;
        ctx_->OMSetRenderTargets(1, &noRtv, shadowDsv_);
        ctx_->ClearDepthStencilView(shadowDsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT sv = {0, 0, float(kShadowSize), float(kShadowSize), 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &sv);
        ctx_->RSSetState(shadowRaster_);
        ctx_->VSSetShader(vsShadow_, nullptr, 0);
        ctx_->PSSetShader(nullptr, nullptr, 0);
        for (const auto& child : scene.root().children())
            drawActor(*child, shadowVP_, resolved, /*shadowPass=*/true);
        ID3D11RenderTargetView* nr = nullptr;
        ctx_->OMSetRenderTargets(1, &nr, nullptr);
    }

    // --- Main pass -------------------------------------------------------
    D3D11_VIEWPORT vp = {0, 0, float(width), float(height), 0.0f, 1.0f};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, &colorRtv_, depthDsv_);
    const float clear[4] = {resolved.clear.x, resolved.clear.y, resolved.clear.z, 1.0f};
    ctx_->ClearRenderTargetView(colorRtv_, clear);
    ctx_->ClearDepthStencilView(depthDsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->PSSetSamplers(1, 1, &shadowSamp_);
    ctx_->RSSetState(raster_);
    if (shadowActive_)
        ctx_->PSSetShaderResources(1, 1, &shadowSrv_);

    Mat4 viewProj = view * proj;
    for (const auto& child : scene.root().children())
        drawActor(*child, viewProj, resolved, /*shadowPass=*/false);
    drawVolumeFogs(viewProj, eye, nearZ, farZ, resolved);

    // Unbind so the colour SRV (and shadow SRV) can be re-bound next frame.
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx_->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* noSrv2[2] = {nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 2, noSrv2);
    return colorSrv_;
}

} // namespace crate
