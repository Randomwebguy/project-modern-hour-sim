// SPDX-License-Identifier: GPL-3.0-or-later
#include "TemporalAAGPU.h"
#include "TemporalAAShaderSource.h"
#include "ShaderBytecodeCache.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace GenZH::Modern {
namespace {
using Microsoft::WRL::ComPtr;
struct TemporalConstants {float currentInverseVP[16],previousVP[16],viewport[4],jitter[4],size[4],depth[4],filter[4],previousInverseVP[16],disocclusion[4],currentVP[16],sharpen[4];};
static_assert(sizeof(TemporalConstants)==368);
}

HRESULT TemporalAAGPU::Create(ID3D11Device *device,UINT width,UINT height){
    Reset();
    if(!device||!width||!height||width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)return E_INVALIDARG;
    ComPtr<ID3DBlob> vs,ps,cas;std::string error;bool hit=false;HRESULT hr=S_OK;
    if(FAILED(hr=CompileCachedShader(TemporalAAShaderSource,"VSTemporal","vs_5_0",vs,error,hit))){stats_.lastError="VSTemporal: "+error;return hr;}
    if(FAILED(hr=CompileCachedShader(TemporalAAShaderSource,"PSTemporalResolve","ps_5_0",ps,error,hit))){stats_.lastError="PSTemporalResolve: "+error;return hr;}
    if(FAILED(hr=CompileCachedShader(TemporalAAShaderSource,"PSTemporalSharpen","ps_5_0",cas,error,hit))){stats_.lastError="PSTemporalSharpen: "+error;return hr;}
    if(FAILED(hr=device->CreatePixelShader(cas->GetBufferPointer(),cas->GetBufferSize(),nullptr,sharpenPixel_.GetAddressOf())))return hr;
    if(FAILED(hr=device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex_.GetAddressOf()))||
       FAILED(hr=device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel_.GetAddressOf())))return hr;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    for(unsigned i=0;i<2;++i)
        if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,history_[i].GetAddressOf()))||FAILED(hr=device->CreateRenderTargetView(history_[i].Get(),nullptr,historyTargets_[i].GetAddressOf()))||
           FAILED(hr=device->CreateShaderResourceView(history_[i].Get(),nullptr,historyViews_[i].GetAddressOf())))return hr;
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(TemporalConstants);buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(hr=device->CreateBuffer(&buffer,nullptr,constants_.GetAddressOf())))return hr;
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=FALSE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    if(FAILED(hr=device->CreateSamplerState(&sampler,sampler_.GetAddressOf()))||FAILED(hr=device->CreateRasterizerState(&raster,raster_.GetAddressOf()))||
       FAILED(hr=device->CreateDepthStencilState(&depth,noDepth_.GetAddressOf()))||FAILED(hr=device->CreateBlendState(&blend,opaque_.GetAddressOf())))return hr;
    width_=width;height_=height;return S_OK;
}

HRESULT TemporalAAGPU::Resolve(ID3D11DeviceContext *context,ID3D11Resource *colorResource,ID3D11ShaderResourceView *color,ID3D11ShaderResourceView *depth,const TemporalAAFrame &frame,ID3D11RenderTargetView *colorTarget,ID3D11ShaderResourceView *motion){
    if(!pixel_||!context||!colorResource||!color||!depth)return E_INVALIDARG;
    for(float value:frame.inverseViewProjection)if(!std::isfinite(value))return E_INVALIDARG;
    for(float value:frame.viewProjection)if(!std::isfinite(value))return E_INVALIDARG;
    // A camera cut (map change, jump to a minimap location, a new viewport)
    // starts a new history instead of smearing the previous view into it.
    bool valid=historyValid_;
    if(valid){
        const float dx=frame.eye[0]-previousEye_[0],dy=frame.eye[1]-previousEye_[1],dz=frame.eye[2]-previousEye_[2];
        if(std::sqrt(dx*dx+dy*dy+dz*dz)>settings.cutDistance||std::memcmp(previousViewport_,frame.viewport,sizeof(previousViewport_))){valid=false;++stats_.cuts;}
    }
    if(!valid)++stats_.resets;
    // The previous frame's depth (disocclusion test) is a copy of the scene depth
    // resource in its own format; it is (re)created when that resource changes.
    ComPtr<ID3D11Resource> depthResource;depth->GetResource(depthResource.GetAddressOf());ComPtr<ID3D11Texture2D> depthTexture;
    if(FAILED(depthResource.As(&depthTexture)))return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC depthDesc{};depthTexture->GetDesc(&depthDesc);
    if(!previousDepth_||std::memcmp(&depthDesc,&previousDepthSource_,sizeof(depthDesc))){
        previousDepth_.Reset();previousDepthView_.Reset();previousDepthReady_=false;
        D3D11_TEXTURE2D_DESC copy=depthDesc;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;copy.Usage=D3D11_USAGE_DEFAULT;copy.CPUAccessFlags=0;copy.MiscFlags=0;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};depth->GetDesc(&view);ComPtr<ID3D11Device> device;context->GetDevice(device.GetAddressOf());
        HRESULT hr=device->CreateTexture2D(&copy,nullptr,previousDepth_.GetAddressOf());
        if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(previousDepth_.Get(),&view,previousDepthView_.GetAddressOf());
        if(FAILED(hr)){previousDepth_.Reset();previousDepthView_.Reset();++stats_.failures;stats_.lastError="previous depth copy";return hr;}
        previousDepthSource_=depthDesc;
    }
    const bool disocclusion=valid&&previousDepthReady_&&settings.disocclusion;
    TemporalConstants k{};std::memcpy(k.currentInverseVP,frame.inverseViewProjection,64);std::memcpy(k.previousVP,valid?previousViewProjection_:frame.viewProjection,64);
    std::memcpy(k.viewport,frame.viewport,16);k.jitter[0]=frame.jitter[0];k.jitter[1]=frame.jitter[1];k.jitter[2]=valid?1.0f:0.0f;k.jitter[3]=settings.blend;
    k.size[0]=static_cast<float>(width_);k.size[1]=static_cast<float>(height_);k.size[2]=1.0f/width_;k.size[3]=1.0f/height_;
    k.depth[0]=frame.depthRange[0];k.depth[1]=frame.depthRange[1];k.depth[2]=settings.gamma;k.depth[3]=settings.motionBlend;k.filter[0]=settings.sharpness;k.filter[1]=settings.tonemapped?1.0f:0.0f;k.filter[2]=settings.motionScale;k.filter[3]=settings.lumaWeight;
    std::memcpy(k.previousInverseVP,valid?previousInverseViewProjection_:frame.inverseViewProjection,64);
    k.disocclusion[0]=settings.disocclusionFootprints;k.disocclusion[1]=settings.disocclusionRelative;k.disocclusion[2]=disocclusion?1.0f:0.0f;std::memcpy(k.currentVP,frame.viewProjection,64);k.sharpen[0]=std::clamp(settings.sharpen,0.0f,1.0f);
    context->UpdateSubresource(constants_.Get(),0,nullptr,&k,0,0);
    const unsigned next=current_^1u;
    ID3D11RenderTargetView *target=historyTargets_[next].Get();context->OMSetRenderTargets(1,&target,nullptr);
    const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};context->RSSetViewports(1,&viewport);
    context->RSSetState(raster_.Get());context->OMSetDepthStencilState(noDepth_.Get(),0);context->OMSetBlendState(opaque_.Get(),nullptr,0xffffffff);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(),nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->GSSetShader(nullptr,nullptr,0);
    context->PSSetShader(pixel_.Get(),nullptr,0);
    ID3D11Buffer *buffer=constants_.Get();context->PSSetConstantBuffers(0,1,&buffer);
    ID3D11ShaderResourceView *views[5]={color,depth,valid?historyViews_[current_].Get():color,disocclusion?previousDepthView_.Get():depth,motion};context->PSSetShaderResources(0,5,views);
    ID3D11SamplerState *sampler=sampler_.Get();context->PSSetSamplers(0,1,&sampler);
    context->Draw(3,0);
    ID3D11ShaderResourceView *none[5]{};context->PSSetShaderResources(0,5,none);context->OMSetRenderTargets(0,nullptr,nullptr);
    // post102: the output goes back sharpened (CAS) when a target is given; the history
    // keeps the unsharpened resolve, so sharpening never accumulates.
    if(colorTarget&&sharpenPixel_&&k.sharpen[0]>0){
        context->OMSetRenderTargets(1,&colorTarget,nullptr);context->PSSetShader(sharpenPixel_.Get(),nullptr,0);
        ID3D11ShaderResourceView *sharpen[3]={nullptr,nullptr,historyViews_[next].Get()};context->PSSetShaderResources(0,3,sharpen);
        context->Draw(3,0);context->PSSetShaderResources(0,5,none);context->OMSetRenderTargets(0,nullptr,nullptr);++stats_.sharpened;
    }else context->CopyResource(colorResource,history_[next].Get());
    context->CopyResource(previousDepth_.Get(),depthResource.Get());previousDepthReady_=true;
    current_=next;historyValid_=true;std::memcpy(previousViewProjection_,frame.viewProjection,64);std::memcpy(previousInverseViewProjection_,frame.inverseViewProjection,64);
    std::memcpy(previousEye_,frame.eye,12);std::memcpy(previousViewport_,frame.viewport,16);++stats_.resolves;
    return S_OK;
}

void TemporalAAGPU::Reset(){
    vertex_.Reset();pixel_.Reset();sharpenPixel_.Reset();constants_.Reset();sampler_.Reset();raster_.Reset();noDepth_.Reset();opaque_.Reset();
    for(auto &t:history_)t.Reset();for(auto &t:historyTargets_)t.Reset();for(auto &v:historyViews_)v.Reset();
    previousDepth_.Reset();previousDepthView_.Reset();previousDepthSource_={};previousDepthReady_=false;
    current_=0;width_=height_=0;historyValid_=false;
}
}
