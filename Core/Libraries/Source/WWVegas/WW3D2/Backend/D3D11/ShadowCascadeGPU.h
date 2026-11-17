// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RenderDevice.h"
#include <dxgi1_4.h>
namespace GenZH::Modern {
inline bool ShadowCacheMemoryAvailable(ID3D11Device *device){
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIAdapter3> modern;DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())))||FAILED(dxgi->GetAdapter(adapter.GetAddressOf()))||
       FAILED(adapter.As(&modern))||FAILED(modern->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info)))return true;
    return info.Budget>info.CurrentUsage&&info.Budget-info.CurrentUsage>128ull*1024*1024;
}
class ShadowCascadeGPU {
public:
    static constexpr UINT Size=2048,Count=2;
    void Reset(){view_.Reset();for(auto &d:depth_)d.Reset();texture_.Reset();}
    HRESULT Create(ID3D11Device *device,UINT size=Size){
        Reset();D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=size;desc.MipLevels=desc.SampleDesc.Count=1;
        desc.ArraySize=Count;desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL;
        HRESULT hr=device->CreateTexture2D(&desc,nullptr,texture_.GetAddressOf());if(FAILED(hr))return hr;
        for(UINT i=0;i<Count;++i){D3D11_DEPTH_STENCIL_VIEW_DESC d{};d.Format=DXGI_FORMAT_D32_FLOAT;d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.FirstArraySlice=i;d.Texture2DArray.ArraySize=1;
            if(FAILED(hr=device->CreateDepthStencilView(texture_.Get(),&d,depth_[i].GetAddressOf()))){Reset();return hr;}}
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32_FLOAT;s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        s.Texture2DArray.ArraySize=Count;s.Texture2DArray.MipLevels=1;
        hr=device->CreateShaderResourceView(texture_.Get(),&s,view_.GetAddressOf());if(FAILED(hr))Reset();return hr;
    }
    ID3D11DepthStencilView *Depth(UINT i)const{return i<Count?depth_[i].Get():nullptr;}
    ID3D11ShaderResourceView *View()const{return view_.Get();}
    void CopyLayerFrom(ID3D11DeviceContext *context,const ShadowCascadeGPU &source,UINT layer){
        // Both depth arrays are DEFAULT, equal-sized R32 typeless resources.
        // Caller unbinds OM depth and receiver SRVs before the GPU-side copy.
        context->CopySubresourceRegion(texture_.Get(),layer,0,0,0,source.texture_.Get(),layer,nullptr);
    }
private:
    ComPtr<ID3D11Texture2D> texture_;
    std::array<ComPtr<ID3D11DepthStencilView>,Count> depth_;
    ComPtr<ID3D11ShaderResourceView> view_;
};
}
