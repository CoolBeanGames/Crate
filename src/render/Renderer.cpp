#include "render/Renderer.h"
#include "assets/Image.h"
#include "core/Log.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/Scene.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
    float shadow[4];    // x=enabled y=receive z=1/mapSize w=bias
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
    float4   uShadow;
    Light    uLights[MAX_LIGHTS];
};
Texture2D             uTex : register(t0);
SamplerState          uSamp : register(s0);
Texture2D             uShadowMap : register(t1);
SamplerComparisonState uShadowSamp : register(s1);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0;
               float3 light : COLOR0; float fog : TEXCOORD1; float4 lpos : TEXCOORD2; };

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
    float3 wpos = mul(uModel, float4(i.pos, 1.0)).xyz;
    float3 N = normalize(mul((float3x3)uModel, i.nrm));
    o.uv = i.uv;
    o.light = (uParams.z > 0.5) ? float3(1,1,1) : shadeVertex(wpos, N);
    o.fog = (uFogParams.z > 0.5)
              ? saturate((uFogParams.y - o.pos.w) / max(uFogParams.y - uFogParams.x, 1e-4))
              : 1.0;
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

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 tex = lerp(float3(1,1,1), uTex.Sample(uSamp, i.uv).rgb, uParams.x);
    float3 lit = (uParams.z > 0.5) ? float3(1,1,1)
                                   : uAmbient.rgb + i.light * shadowFactor(i.lpos);
    float3 col = uBaseColor.rgb * tex * lit + uParams.y;
    col = lerp(uFogColor.rgb, col, saturate(i.fog));
    return float4(col, uBaseColor.a);
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

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    device_->CreateDepthStencilState(&dsd, &depthState_);

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
    safeRelease(vsShadow_);
    safeRelease(layout_);
    safeRelease(cb_);
    safeRelease(sampler_);
    safeRelease(raster_);
    safeRelease(depthState_);
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

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&dd, nullptr, &depthTex_)))
        return false;
    device_->CreateDepthStencilView(depthTex_, nullptr, &depthDsv_);

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
    Mat4 model = Mat4::scale(w.scale) * Mat4::rotationEuler(w.rotationEuler) *
                 Mat4::translation(w.position);
    Mat4 mvp = model * viewProj;

    // Resolve material: a referenced Material asset wins; otherwise fall back to
    // the MeshRenderer's own tint + texture.
    float baseColor[4] = {mr->tint[0] * 0.78f, mr->tint[1] * 0.78f, mr->tint[2] * 0.80f,
                          mr->tint[3]};
    std::string texPath = mr->texturePath;
    float emissive = 0.0f;
    bool unlit = false;
    if (materials_ && !mr->materialRef.empty()) {
        if (const Material* mat = materials_->find(mr->materialRef)) {
            for (int i = 0; i < 4; ++i)
                baseColor[i] = mat->baseColor[i] * mr->tint[i];
            texPath = mat->texturePath.empty() ? mr->texturePath : mat->texturePath;
            emissive = mat->emissive;
            unlit = mat->unlit;
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
    bool sel = opt.highlight == &actor;
    cb.baseColor[0] = sel ? baseColor[0] * 0.85f + 0.10f : baseColor[0];
    cb.baseColor[1] = sel ? baseColor[1] * 0.85f + 0.05f : baseColor[1];
    cb.baseColor[2] = sel ? baseColor[2] * 0.85f + 0.25f : baseColor[2];
    cb.baseColor[3] = baseColor[3];

    int lightCount = static_cast<int>(std::min<size_t>(lights_.size(), kMaxLights));
    cb.params[0] = texPath.empty() ? 0.0f : 1.0f;
    cb.params[1] = emissive;
    cb.params[2] = unlit ? 1.0f : 0.0f;
    cb.params[3] = static_cast<float>(lightCount);
    cb.ambient[0] = opt.ambient.x; cb.ambient[1] = opt.ambient.y; cb.ambient[2] = opt.ambient.z;
    cb.fogColor[0] = opt.fogColor.x; cb.fogColor[1] = opt.fogColor.y; cb.fogColor[2] = opt.fogColor.z;
    cb.fogParams[0] = opt.fogStart;
    cb.fogParams[1] = opt.fogEnd;
    cb.fogParams[2] = opt.fogEnabled ? 1.0f : 0.0f;
    for (int k = 0; k < lightCount; ++k) {
        const LightSample& s = lights_[k];
        GpuLight& g = cb.lights[k];
        g.pos[0] = s.pos.x; g.pos[1] = s.pos.y; g.pos[2] = s.pos.z;
        g.dir[0] = s.dir.x; g.dir[1] = s.dir.y; g.dir[2] = s.dir.z;
        g.color[0] = s.color.x; g.color[1] = s.color.y; g.color[2] = s.color.z;
        g.color[3] = static_cast<float>(s.type);
        g.params[0] = s.range; g.params[1] = s.cosInner; g.params[2] = s.cosOuter;
    }

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
            lights_.push_back(s);
        }
    }
    for (const auto& child : actor.children())
        collectLights(*child);
}

void* Renderer::render(Scene& scene, const OrbitCamera& cam, int width, int height,
                       const Options& opt) {
    if (!ready() || !ensureTargets(width, height))
        return nullptr;

    lights_.clear();
    for (const auto& child : scene.root().children())
        collectLights(*child);
    shadowActive_ =
        opt.shadows && ensureShadowMap() && vsShadow_ && computeShadowVP(shadowVP_);

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
            drawActor(*child, shadowVP_, opt, /*shadowPass=*/true);
        ID3D11RenderTargetView* nr = nullptr;
        ctx_->OMSetRenderTargets(1, &nr, nullptr);
    }

    // --- Main pass -------------------------------------------------------
    D3D11_VIEWPORT vp = {0, 0, float(width), float(height), 0.0f, 1.0f};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, &colorRtv_, depthDsv_);
    const float clear[4] = {opt.clear.x, opt.clear.y, opt.clear.z, 1.0f};
    ctx_->ClearRenderTargetView(colorRtv_, clear);
    ctx_->ClearDepthStencilView(depthDsv_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->PSSetSamplers(1, 1, &shadowSamp_);
    ctx_->RSSetState(raster_);
    if (shadowActive_)
        ctx_->PSSetShaderResources(1, 1, &shadowSrv_);

    float aspect = height > 0 ? float(width) / float(height) : 1.0f;
    Mat4 viewProj = cam.view() * cam.proj(aspect);
    for (const auto& child : scene.root().children())
        drawActor(*child, viewProj, opt, /*shadowPass=*/false);

    // Unbind so the colour SRV (and shadow SRV) can be re-bound next frame.
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx_->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* noSrv2[2] = {nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 2, noSrv2);
    return colorSrv_;
}

} // namespace crate
