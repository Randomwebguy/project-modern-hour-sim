// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental modern rendering layer. See README.md for integration status.
#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include "EnvironmentGPU.h"
#include "SceneDepthGPU.h"
#include "ScreenMaterialGPU.h"
#include "GTAOGPU.h"
#include "TemporalAAGPU.h"
#include "BloomGPU.h"

namespace GenZH::Modern {
using Microsoft::WRL::ComPtr;

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

// All matrices are row-major and multiply row vectors in HLSL. The caller
// supplies inverse-transpose world for normals, including non-uniform scale.
struct MeshConstants {
    float world[16];
    float viewProjection[16];
    float normalWorld[16];
    float baseColor[4];              // Linear RGB and alpha, including team tint.
    float surface[4];                // Roughness, metalness, alpha cutoff, normal strength.
    float emissive[4];               // Linear emissive RGB; W reserved.
};
struct FrameConstants {
    float cameraExposure[4];         // Camera world XYZ, positive exposure multiplier.
    float sunDirectionIntensity[4]; // Direction toward the sun, nonnegative intensity.
    float sunColor[4];               // Linear RGB; W reserved.
    float ambient[4];                // Linear diffuse fill; W reserved (IBL pending).
    float shadowSettings[4];         // Depth bias, inverse map size XY, enabled flag.
    float lightViewProjection[16];
};
static_assert(sizeof(Vertex)==32);
static_assert(sizeof(MeshConstants)==240);
static_assert(sizeof(FrameConstants)==144);

struct Mesh {
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11Buffer> indices;
    UINT indexCount=0;
    const ID3D11Device *owner=nullptr; // Identity only; buffers retain the device.
};
struct Texture {
    ComPtr<ID3D11ShaderResourceView> view;
    const ID3D11Device *owner=nullptr;
};
struct MaterialTextures {
    const Texture *albedo=nullptr;   // SRGB input.
    const Texture *normal=nullptr;   // Linear tangent-space input.
    const Texture *orm=nullptr;      // Linear: occlusion, roughness, metalness.
};

class RenderDevice final {
public:
    RenderDevice()=default;
    ~RenderDevice() { Shutdown(); }
    RenderDevice(const RenderDevice&)=delete;
    RenderDevice& operator=(const RenderDevice&)=delete;

    HRESULT Initialize(HWND window, UINT width, UINT height, bool useWarp=false);
    void Shutdown();
    HRESULT Resize(UINT width, UINT height);
    HRESULT CreateMesh(const Vertex *vertices, UINT vertexCount,
                       const std::uint32_t *indices, UINT indexCount, Mesh &result);
    HRESULT CreateTextureRGBA(const void *pixels, UINT width, UINT height,
                              UINT rowPitch, bool srgb, Texture &result);
    HRESULT SetFrame(const FrameConstants &constants);
    HRESULT SetGamma(const std::uint16_t *red,const std::uint16_t *green,const std::uint16_t *blue);
    HRESULT BeginShadowPass();
    HRESULT BeginFrame(const float clearLinearRGBA[4]);
    HRESULT DrawMesh(const Mesh &mesh, const MeshConstants &constants,
                     const MaterialTextures &textures={});
    HRESULT EndFrame(bool vsync);
    HRESULT ToneMapFrame();
    HRESULT PresentFrame(bool vsync);
    ID3D11Device *Device() const { return device_.Get(); }
    ID3D11DeviceContext *Context() const { return context_.Get(); }
    ID3D11RenderTargetView *SceneTarget() const { return hdrTarget_.Get(); }
    ID3D11DepthStencilView *SceneDepth() const { return sceneDepth_.Writable(); }
    ID3D11DepthStencilView *SceneReadOnlyDepth() const { return sceneDepth_.ReadOnly(); }
    ID3D11ShaderResourceView *SceneDepthView() const { return sceneDepth_.DepthView(); }
    const SceneDepthGPU &SceneDepthData()const{return sceneDepth_;}
    HRESULT ApplyScreenAO();
    const GTAOGPU &ScreenAO()const{return screenAO_;}
    HRESULT BuildSceneDepth(){return sceneDepth_.Build(context_.Get());}
    // taa100: resolves the world image with its reprojected history (before the
    // HUD); the edge-AA post pass is skipped on frames resolved here.
    HRESULT ApplyTemporalAA(const TemporalAAFrame &frame);
    void InvalidateTemporalAA(){temporal_.Invalidate();}
    // post106: glow of the world image (after TAA, before the HUD); the tone map adds it.
    // Mode 0 off, 1 the former 3x3 tone-map bloom, 2 the mip-chain bloom.
    HRESULT ApplyBloom();
    void SetBloomMode(int mode){bloomMode_=mode;postSettings_[3]=mode==1?.06f:0.0f;}
    // photo107: colour grade after the tone curve (saturation, highlight warmth, contrast about
    // 18% grey). Off restores the identity grade.
    void SetColourGrade(bool on){postSettings_[8]=on?1.06f:1.0f;postSettings_[9]=on?.35f:0.0f;postSettings_[10]=on?1.03f:1.0f;lutDirty_=true;}
    // photo107: the colour LUT of the tone map (32^3, linear); rebuilt when the grade settings change.
    HRESULT BuildColourLut();
    ComPtr<ID3D11Texture3D> lutTexture_;ComPtr<ID3D11ShaderResourceView> lutView_;bool lutDirty_=true;
    // photo107: screen-space indirect light (GENZH_GI): the bounce is gathered at half resolution
    // from the lit HDR into giTarget_, then added to the HDR target before the temporal resolve.
    struct GlobalIlluminationConstants {float viewProjection[16],inverseViewProjection[16],eye[4],depthRange[4],frame[4];};
    ComPtr<ID3D11PixelShader> giPS_,giComposePS_;
    ComPtr<ID3D11Texture2D> giTexture_;
    ComPtr<ID3D11RenderTargetView> giTarget_;
    ComPtr<ID3D11ShaderResourceView> giView_;
    ComPtr<ID3D11Buffer> giConstants_;
    ComPtr<ID3D11BlendState> giAdditive_;
    UINT giWidth_=0,giHeight_=0;unsigned giFrame_=0;bool giEnabled_=true;float giStrength_=.55f,giRadius_=6.0f;
    HRESULT ApplyGlobalIllumination();
    // photo107: the scene before the water draws (screen-space water reflection, GENZH_WATER_REFLECTION).
    ComPtr<ID3D11Texture2D> hdrTexture_,waterCopy_;ComPtr<ID3D11ShaderResourceView> waterCopyView_;
    UINT waterCopyWidth_=0,waterCopyHeight_=0;bool waterCopyValid_=false;
    HRESULT CopySceneForWater();
    ID3D11ShaderResourceView *WaterSceneView()const{return waterCopyValid_?waterCopyView_.Get():nullptr;}
    const BloomGPU &Bloom()const{return bloom_;}
    const TemporalAAGPU &TemporalAA()const{return temporal_;}
    TemporalAAGPU &TemporalAA(){return temporal_;}
    ScreenMaterialGPU &ScreenMaterials(){return screenMaterials_;}
    HRESULT PrepareScreenMaterials(){
        if(screenMaterials_.Available())return S_OK;
        if(screenMaterialAttempted_)return S_FALSE;
        screenMaterialAttempted_=true;return screenMaterials_.Create(device_.Get(),width_,height_);
    }
    ID3D11RenderTargetView *OutputTarget() const { return backBuffer_.Get(); }
    ID3D11DepthStencilView *ShadowDepth() const { return shadowDepth_.Get(); }
    ID3D11ShaderResourceView *ShadowView() const { return shadowView_.Get(); }
    ID3D11SamplerState *ShadowSampler() const { return shadowSampler_.Get(); }
    void BindEnvironment(bool enabled=true,ScenePipelineBindings *cache=nullptr){environment_.Bind(context_.Get(),enabled,cache);}
    bool EnvironmentReady()const{return environment_.Ready();}
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    HRESULT DeviceStatus() const;
    const std::string &LastError() const { return error_; }

private:
    enum class Pass { None, Shadow, Opaque };
    HRESULT CreatePipeline();
    HRESULT CreateTargets(UINT width, UINT height);
    HRESULT CreateShadowTarget();
    HRESULT Upload(ID3D11Buffer *buffer,const void *data,UINT bytes);
    HRESULT Fail(HRESULT result,const char *operation);
    void BindMeshPipeline();
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_;
    // present101: swap chain flags (tearing, waitable) and the frame-latency wait handle.
    UINT swapFlags_=0;HANDLE frameWait_=nullptr;bool tearing_=false;
public:
    bool Tearing()const{return tearing_;}
    bool FrameWaitable()const{return frameWait_!=nullptr;}
    UINT SwapFlags()const{return swapFlags_;}
private:
    ComPtr<ID3D11RenderTargetView> backBuffer_,hdrTarget_,tonemapTarget_;
    ComPtr<ID3D11ShaderResourceView> hdrView_,shadowView_,gammaView_,tonemapView_;
    ComPtr<ID3D11DepthStencilView> shadowDepth_;
    SceneDepthGPU sceneDepth_;
    ScreenMaterialGPU screenMaterials_;
    GTAOGPU screenAO_;bool screenAOAttempted_=false,screenAOEnabled_=true,screenContactEnabled_=true;
    TemporalAAGPU temporal_;bool temporalAttempted_=false,temporalResolved_=false;
    BloomGPU bloom_;bool bloomAttempted_=false,bloomReady_=false;int bloomMode_=2;
    bool screenMaterialAttempted_=false;
    ComPtr<ID3D11VertexShader> meshVS_,presentVS_;
    ComPtr<ID3D11PixelShader> meshPS_,shadowPS_,tonemapPS_,aaPS_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> meshConstants_,frameConstants_,postConstants_;
    ComPtr<ID3D11SamplerState> materialSampler_,shadowSampler_,presentSampler_;
    ComPtr<ID3D11RasterizerState> raster_,presentRaster_;
    ComPtr<ID3D11DepthStencilState> depthState_,noDepth_;
    ComPtr<ID3D11BlendState> opaqueBlend_;
    Texture white_,flatNormal_;
    EnvironmentGPU environment_;
    FrameConstants frame_{};
    UINT width_=0,height_=0;
    static constexpr UINT ShadowSize=2048;
    Pass pass_=Pass::None;
    bool frameSet_=false,shadowRendered_=false;
    // perf111: index 5 is the exposure trim, default .85 (stock fortress median match, GENZH_EXPOSURE_TRIM=1 restores the former default).
    float postSettings_[12]={0,0,1,0,0,.85f,0,0,1,0,1,0}; // Inverse size, edge AA, former bloom strength; post106 glow scale.
    std::string error_;
};
}
