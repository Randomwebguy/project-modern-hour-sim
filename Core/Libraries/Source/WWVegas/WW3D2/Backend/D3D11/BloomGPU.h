// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace GenZH::Modern {
// post106: mip-chain bloom of the world image (Bloom.hlsl): per-pixel threshold at full
// resolution, linear 13-tap downsample chain, tent upsample. Apply() runs after the world
// passes (TAA, before the HUD); the tone map adds Result() * CompositeScale().
struct BloomSettings {
    float intensity=.12f;          // share of the thresholded energy spread into the glow
    float threshold=1,knee=.5f;    // exposed units: glow starts fading in at threshold-knee
    unsigned maxLevels=6;          // half resolution down to 1/64
};
struct BloomStats {std::uint64_t applied=0,failures=0;unsigned levels=0;std::string lastError;};
class BloomGPU final {
public:
    HRESULT Create(ID3D11Device *device,UINT width,UINT height);
    // Builds the glow of `hdr` (scene radiance, multiplied by `exposure` before the threshold).
    // Leaves no render target or shader resource bound.
    HRESULT Apply(ID3D11DeviceContext *context,ID3D11ShaderResourceView *hdr,float exposure);
    ID3D11ShaderResourceView *Result()const{return views_.empty()?nullptr:views_[0].Get();}
    float CompositeScale()const{return levels_?settings.intensity/static_cast<float>(levels_):0.0f;}
    void Reset();
    bool Ready()const{return bool(down_);}
    UINT Width()const{return width_;}UINT Height()const{return height_;}
    BloomSettings settings;
    const BloomStats &Stats()const{return stats_;}
private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> prefilter_,down_,up_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;Microsoft::WRL::ComPtr<ID3D11RenderTargetView> sourceTarget_;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView_; // thresholded, full resolution
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> noDepth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaque_,additive_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> targets_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> views_;
    std::vector<UINT> levelWidth_,levelHeight_;
    unsigned levels_=0;UINT width_=0,height_=0;
    BloomStats stats_;
};
}
