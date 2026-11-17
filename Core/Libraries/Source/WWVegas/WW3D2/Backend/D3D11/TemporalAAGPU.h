// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>

namespace GenZH::Modern {
// taa100: temporal anti-aliasing (TemporalAA.hlsl). The renderer jitters the
// projection of the primary camera's world draws by a Halton(2,3) sub-pixel
// offset; after the world passes this resolve blends the frame with its
// reprojected history (scene depth, unjittered camera; rejected where the
// previous depth shows another surface) before the HUD.
inline float TemporalHalton(unsigned index,unsigned base){float f=1,result=0;while(index>0){f/=static_cast<float>(base);result+=f*static_cast<float>(index%base);index/=base;}return result;}
// Sub-pixel offset of a frame in pixels (+x right, +y down), 8-sample cycle.
inline std::array<float,2> TemporalJitterPixels(std::uint64_t frame){const unsigned i=static_cast<unsigned>(frame%8)+1;return {TemporalHalton(i,2)-.5f,TemporalHalton(i,3)-.5f};}
// Shifts what a row-major (row-vector) projection draws by (x,y) pixels of a
// viewport: clip.xy += ndcOffset*clip.w, exact for perspective and orthographic.
inline void ApplyTemporalJitter(float *projection,float x,float y,float viewportWidth,float viewportHeight){
    const float nx=2*x/viewportWidth,ny=-2*y/viewportHeight;
    for(unsigned row=0;row<4;++row){projection[row*4+0]+=nx*projection[row*4+3];projection[row*4+1]+=ny*projection[row*4+3];}
}
struct TemporalAAFrame {
    float inverseViewProjection[16]{};  // unjittered, current
    float viewProjection[16]{};         // unjittered, current (becomes the next frame's previous)
    float viewport[4]{},depthRange[2]{0,1},eye[3]{};
    float jitter[2]{};                  // pixels, this frame
};
// Defaults chosen on the offline fixture (TemporalAAGPUChecks): a centre-sample
// reconstruction (sharpness 100: neighbours weigh <1e-6) converges to the box
// filter of supersampling, the sharpest option; wider Gaussians (2.29 as in
// common engines) soften flat areas next to edges. Linear, unweighted
// accumulation: luminance or tonemapped weighting darkens bright edges.
struct TemporalAASettings {
    float blend=.08f,motionBlend=.4f,gamma=1.1f,cutDistance=150;   // base current-frame weight, its boost under motion, variance box, cut (world units)
    float sharpness=100,motionScale=2;                              // reconstruction Gaussian exp(-s*d*d); motion (pixels) of the full boost
    float lumaWeight=0;bool tonemapped=false;                       // luminance weight strength; tonemapped accumulation (A/B only)
    bool disocclusion=true;float disocclusionFootprints=2,disocclusionRelative=.002f; // previous-depth rejection tolerance
    float sharpen=.35f;                                             // post102: CAS of the output (0 = plain copy)
};
struct TemporalAAStats {std::uint64_t resolves=0,resets=0,cuts=0,failures=0,sharpened=0;std::string lastError;};
class TemporalAAGPU final {
public:
    HRESULT Create(ID3D11Device *device,UINT width,UINT height);
    // Resolves `color` (SRV of `colorResource`) with the history and writes the
    // result back into `colorResource`. Render targets are unbound on return.
    // post102: with colorTarget (an RTV of colorResource) the output is sharpened (CAS) on the way back.
    HRESULT Resolve(ID3D11DeviceContext *context,ID3D11Resource *colorResource,ID3D11ShaderResourceView *color,ID3D11ShaderResourceView *depth,const TemporalAAFrame &frame,ID3D11RenderTargetView *colorTarget=nullptr,ID3D11ShaderResourceView *motion=nullptr);
    // The next resolve starts a new history (no frame of the camera this frame).
    void Invalidate(){historyValid_=false;}
    void Reset();
    bool Ready()const{return bool(pixel_);}
    UINT Width()const{return width_;}UINT Height()const{return height_;}
    TemporalAASettings settings;
    const TemporalAAStats &Stats()const{return stats_;}
private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_,sharpenPixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> noDepth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaque_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,2> history_;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>,2> historyTargets_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,2> historyViews_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> previousDepth_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previousDepthView_;
    D3D11_TEXTURE2D_DESC previousDepthSource_{};bool previousDepthReady_=false;
    float previousViewProjection_[16]{},previousInverseViewProjection_[16]{},previousEye_[3]{},previousViewport_[4]{};
    unsigned current_=0;UINT width_=0,height_=0;bool historyValid_=false;
    TemporalAAStats stats_;
};
}
