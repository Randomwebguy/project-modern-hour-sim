// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RenderDevice.h"
#include "LocalLightShadows.h"
namespace GenZH::Modern {
class LocalShadowGPU {
 ComPtr<ID3D11Texture2D> texture_;ComPtr<ID3D11ShaderResourceView> view_;
 std::array<ComPtr<ID3D11DepthStencilView>,LocalShadowFaces> depth_;
public:
 static constexpr std::uint64_t Bytes=std::uint64_t(LocalShadowSize)*LocalShadowSize*LocalShadowFaces*4;
 void Reset(){view_.Reset();for(auto &d:depth_)d.Reset();texture_.Reset();}
 bool Available()const{return bool(view_);}
 ID3D11ShaderResourceView *View()const{return view_.Get();}
 ID3D11DepthStencilView *Depth(unsigned i)const{return i<LocalShadowFaces?depth_[i].Get():nullptr;}
 HRESULT Create(ID3D11Device *device){
  Reset();D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=LocalShadowSize;desc.MipLevels=desc.SampleDesc.Count=1;
  desc.ArraySize=LocalShadowFaces;desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL;
  HRESULT hr=device->CreateTexture2D(&desc,nullptr,texture_.GetAddressOf());if(FAILED(hr))return hr;
  for(unsigned i=0;i<LocalShadowFaces;++i){D3D11_DEPTH_STENCIL_VIEW_DESC d{};d.Format=DXGI_FORMAT_D32_FLOAT;d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.FirstArraySlice=i;d.Texture2DArray.ArraySize=1;
   if(FAILED(hr=device->CreateDepthStencilView(texture_.Get(),&d,depth_[i].GetAddressOf()))){Reset();return hr;}}
  D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32_FLOAT;s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;s.Texture2DArray.ArraySize=LocalShadowFaces;s.Texture2DArray.MipLevels=1;
  hr=device->CreateShaderResourceView(texture_.Get(),&s,view_.GetAddressOf());if(FAILED(hr))Reset();return hr;
 }
};
}
