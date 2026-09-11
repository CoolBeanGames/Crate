#pragma once
#include "assets/MaterialLibrary.h"
#include "assets/MeshLibrary.h"
#include "core/Math.h"
#include "render/Camera.h"
#include "render/Mesh.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11SamplerState;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilState;
struct ID3D11BlendState;

namespace crate {

class Scene;
class Actor;

// Forward D3D11 renderer that draws a Scene's meshes into an offscreen texture,
// which the Viewport panel then displays through ImGui::Image. Shares the
// application's D3D11 device (created in main.cpp).
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    bool init(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();
    bool ready() const { return device_ != nullptr; }

    // Imported geometry is looked up here by MeshRenderer::meshPath.
    void setMeshLibrary(const MeshLibrary* lib) { library_ = lib; }
    // Materials are looked up here by MeshRenderer::materialRef.
    void setMaterialLibrary(const MaterialLibrary* lib) { materials_ = lib; }

    // Drop a cached GPU mesh so a re-import is picked up.
    void invalidateMesh(const std::string& key);

    struct Options {
        Vec3 clear{0.043f, 0.051f, 0.070f};
        Actor* highlight = nullptr; // selected actor, drawn with a tinted base colour
        Vec3 ambient{0.14f, 0.14f, 0.17f}; // base light when no light reaches a face
        bool shadows = true; // real-time shadow map for the first dir/spot light
        bool fogEnabled = false;
        Vec3 fogColor{0.043f, 0.051f, 0.070f};
        float fogStart = 6.0f;
        float fogEnd = 40.0f;
    };

    // Render the scene; returns an SRV (as void* for ImGui::Image) valid until
    // the next call. Returns nullptr on failure or zero size.
    void* render(Scene& scene, const OrbitCamera& cam, int width, int height, const Options& opt);

    // Explicit texture cache control (used by the asset importer).
    void* loadTexture(const std::string& path);
    void invalidateTexture(const std::string& path);

private:
    struct GpuMesh {
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        uint32_t indexCount = 0;
    };

    // One scene light resolved to world space for the shader.
    struct LightSample {
        int type = 1; // 0 dir, 1 point, 2 spot
        Vec3 pos{};
        Vec3 dir{0, 0, -1};
        Vec3 color{1, 1, 1}; // already scaled by intensity
        float range = 8.0f;
        float cosInner = 1.0f;
        float cosOuter = 0.9f;
    };
    std::vector<LightSample> lights_;

    bool ensureTargets(int w, int h);
    bool ensureShadowMap();
    bool compileShaders();
    const GpuMesh& meshFor(const std::string& key, const std::string& primitiveFallback);
    GpuMesh upload(const MeshData& data);
    void drawActor(Actor& actor, const Mat4& viewProj, const Options& opt, bool shadowPass);
    void collectLights(Actor& actor); // fills lights_ (world space), capped
    bool computeShadowVP(Mat4& out) const; // false if no shadow-casting light
    void releaseTargets();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;

    int rtWidth_ = 0, rtHeight_ = 0;
    ID3D11Texture2D* colorTex_ = nullptr;
    ID3D11RenderTargetView* colorRtv_ = nullptr;
    ID3D11ShaderResourceView* colorSrv_ = nullptr;
    ID3D11Texture2D* depthTex_ = nullptr;
    ID3D11DepthStencilView* depthDsv_ = nullptr;

    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11DepthStencilState* depthState_ = nullptr;

    ID3D11ShaderResourceView* whiteSrv_ = nullptr;
    ID3D11ShaderResourceView* checkerSrv_ = nullptr;

    // Shadow mapping (single directional / spot light).
    static constexpr int kShadowSize = 2048;
    ID3D11VertexShader* vsShadow_ = nullptr;
    ID3D11Texture2D* shadowTex_ = nullptr;
    ID3D11DepthStencilView* shadowDsv_ = nullptr;
    ID3D11ShaderResourceView* shadowSrv_ = nullptr;
    ID3D11SamplerState* shadowSamp_ = nullptr;
    ID3D11RasterizerState* shadowRaster_ = nullptr;
    Mat4 shadowVP_;
    bool shadowActive_ = false;

    const MeshLibrary* library_ = nullptr;
    const MaterialLibrary* materials_ = nullptr;
    std::unordered_map<std::string, GpuMesh> meshCache_;
    std::unordered_map<std::string, ID3D11ShaderResourceView*> textureCache_;
};

} // namespace crate
