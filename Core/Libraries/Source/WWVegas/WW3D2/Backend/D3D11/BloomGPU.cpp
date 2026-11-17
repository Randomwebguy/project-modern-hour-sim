// SPDX-License-Identifier: GPL-3.0-or-later
#include "BloomGPU.h"
#include "BloomShaderSource.h"
#include "ShaderBytecodeCache.h"
#include <algorithm>
#include <cmath>

namespace GenZH::Modern {
namespace {
using Microsoft::WRL::ComPtr;
struct BloomConstants {float source[4],curve[4];};
static_assert(sizeof(BloomConstants)==32);
}

HRESULT BloomGPU::Create(ID3D11Device *device,UINT width,UINT height){
    Reset();
    if(!device||width<4||height<4||width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)return E_INVALIDARG;
    ComPtr<ID3DBlob> vs,prefilter,down,up;std::string error;bool hit=false;HRESULT hr=S_OK;
    if(FAILED(hr=CompileCachedShader(BloomShaderSource,"VSBloom","vs_5_0",vs,error,hit))){stats_.lastError="VSBloom: "+error;return hr;}
    if(FAILED(hr=CompileCachedShader(BloomShaderSource,"PSBloomPrefilter","ps_5_0",prefilter,error,hit))){stats_.lastError="PSBloomPrefilter: "+error;return hr;}
    if(FAILED(hr=CompileCachedShader(BloomShaderSource,"PSBloomDown","ps_5_0",down,error,hit))){stats_.lastError="PSBloomDown: "+error;return hr;}
    if(FAILED(hr=CompileCachedShader(BloomShaderSource,"PSBloomUp","ps_5_0",up,error,hit))){stats_.lastError="PSBloomUp: "+error;return hr;}
    if(FAILED(hr=device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex_.GetAddressOf()))||
       FAILED(hr=device->CreatePixelShader(prefilter->GetBufferPointer(),prefilter->GetBufferSize(),nullptr,prefilter_.GetAddressOf()))||
       FAILED(hr=device->CreatePixelShader(down->GetBufferPointer(),down->GetBufferSize(),nullptr,down_.GetAddressOf()))||
       FAILED(hr=device->CreatePixelShader(up->GetBufferPointer(),up->GetBufferSize(),nullptr,up_.GetAddressOf())))return hr;
    {D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
     desc.Format=DXGI_FORMAT_R11G11B10_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
     if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,source_.GetAddressOf()))||FAILED(hr=device->CreateRenderTargetView(source_.Get(),nullptr,sourceTarget_.GetAddressOf()))||
        FAILED(hr=device->CreateShaderResourceView(source_.Get(),nullptr,sourceView_.GetAddressOf())))return hr;}
    // Level 0 is half resolution; each further level halves again, down to 4 texels.
    for(unsigned level=0;level<std::max(settings.maxLevels,1u);++level){
        const UINT w=std::max(width>>(level+1),1u),h=std::max(height>>(level+1),1u);
        if(level>0&&(w<4||h<4))break;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R11G11B10_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11ShaderResourceView> view;
        if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,texture.GetAddressOf()))||FAILED(hr=device->CreateRenderTargetView(texture.Get(),nullptr,target.GetAddressOf()))||
           FAILED(hr=device->CreateShaderResourceView(texture.Get(),nullptr,view.GetAddressOf())))return hr;
        textures_.push_back(texture);targets_.push_back(target);views_.push_back(view);levelWidth_.push_back(w);levelHeight_.push_back(h);
    }
    levels_=static_cast<unsigned>(textures_.size());
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(BloomConstants);buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(hr=device->CreateBuffer(&buffer,nullptr,constants_.GetAddressOf())))return hr;
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=FALSE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC add=blend;auto &a=add.RenderTarget[0];a.BlendEnable=TRUE;a.SrcBlend=a.DestBlend=a.SrcBlendAlpha=a.DestBlendAlpha=D3D11_BLEND_ONE;a.BlendOp=a.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    if(FAILED(hr=device->CreateSamplerState(&sampler,sampler_.GetAddressOf()))||FAILED(hr=device->CreateRasterizerState(&raster,raster_.GetAddressOf()))||
       FAILED(hr=device->CreateDepthStencilState(&depth,noDepth_.GetAddressOf()))||FAILED(hr=device->CreateBlendState(&blend,opaque_.GetAddressOf()))||
       FAILED(hr=device->CreateBlendState(&add,additive_.GetAddressOf())))return hr;
    width_=width;height_=height;stats_.levels=levels_;return S_OK;
}

HRESULT BloomGPU::Apply(ID3D11DeviceContext *context,ID3D11ShaderResourceView *hdr,float exposure){
    if(!down_||!context||!hdr||!std::isfinite(exposure)||exposure<=0){++stats_.failures;return E_INVALIDARG;}
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(),nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->GSSetShader(nullptr,nullptr,0);
    context->RSSetState(raster_.Get());context->OMSetDepthStencilState(noDepth_.Get(),0);
    ID3D11Buffer *buffer=constants_.Get();context->PSSetConstantBuffers(0,1,&buffer);
    ID3D11SamplerState *sampler=sampler_.Get();context->PSSetSamplers(0,1,&sampler);
    ID3D11ShaderResourceView *none[1]{};
    auto pass=[&](ID3D11RenderTargetView *rtv,UINT targetWidth,UINT targetHeight,ID3D11PixelShader *shader,ID3D11ShaderResourceView *source,UINT sourceWidth,UINT sourceHeight,bool upsample){
        BloomConstants k{};k.source[0]=1.0f/static_cast<float>(sourceWidth);k.source[1]=1.0f/static_cast<float>(sourceHeight);k.source[3]=exposure;
        k.curve[0]=settings.threshold;k.curve[1]=std::max(settings.knee,1e-4f);
        context->UpdateSubresource(constants_.Get(),0,nullptr,&k,0,0);
        context->PSSetShaderResources(0,1,none);context->OMSetRenderTargets(1,&rtv,nullptr);
        const D3D11_VIEWPORT viewport{0,0,static_cast<float>(targetWidth),static_cast<float>(targetHeight),0,1};context->RSSetViewports(1,&viewport);
        context->OMSetBlendState(upsample?additive_.Get():opaque_.Get(),nullptr,0xffffffff);context->PSSetShader(shader,nullptr,0);
        context->PSSetShaderResources(0,1,&source);context->Draw(3,0);};
    pass(sourceTarget_.Get(),width_,height_,prefilter_.Get(),hdr,width_,height_,false);
    pass(targets_[0].Get(),levelWidth_[0],levelHeight_[0],down_.Get(),sourceView_.Get(),width_,height_,false);
    for(unsigned level=1;level<levels_;++level)pass(targets_[level].Get(),levelWidth_[level],levelHeight_[level],down_.Get(),views_[level-1].Get(),levelWidth_[level-1],levelHeight_[level-1],false);
    // Each level gains the tent-filtered level below it: level 0 ends as the sum of all levels.
    for(unsigned level=levels_-1;level-->0;)pass(targets_[level].Get(),levelWidth_[level],levelHeight_[level],up_.Get(),views_[level+1].Get(),levelWidth_[level+1],levelHeight_[level+1],true);
    context->PSSetShaderResources(0,1,none);context->OMSetRenderTargets(0,nullptr,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);
    ++stats_.applied;return S_OK;
}

void BloomGPU::Reset(){
    vertex_.Reset();prefilter_.Reset();down_.Reset();up_.Reset();source_.Reset();sourceTarget_.Reset();sourceView_.Reset();constants_.Reset();sampler_.Reset();raster_.Reset();noDepth_.Reset();opaque_.Reset();additive_.Reset();
    textures_.clear();targets_.clear();views_.clear();levelWidth_.clear();levelHeight_.clear();levels_=0;width_=height_=0;
}
}
