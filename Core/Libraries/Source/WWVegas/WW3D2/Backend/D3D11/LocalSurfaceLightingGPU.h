// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RenderDevice.h"
#include "LocalLightShadows.h"
#include "ScenePipelineBindings.h"
namespace GenZH::Modern {
// Surface lighting has its own lifetime; WeatherMode::Off does not disable it.
class LocalSurfaceLightingGPU {
 ComPtr<ID3D11Buffer> constants_;ComPtr<ID3D11ShaderResourceView> atlas_;
 std::array<float,16> view_{},projection_{};D3D11_VIEWPORT viewport_{};bool ready_=false;
public:
 void Reset(){*this=LocalSurfaceLightingGPU{};}
 HRESULT Create(ID3D11Device *device){Reset();D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(LocalShadowConstants);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;return device->CreateBuffer(&d,nullptr,constants_.GetAddressOf());}
 void BeginFrame(){ready_=false;atlas_.Reset();}
 void Prepare(ID3D11DeviceContext *c,const LocalShadowConstants &data,ID3D11ShaderResourceView *atlas,const float *view,const float *projection,const D3D11_VIEWPORT &viewport){
  BeginFrame();if(!constants_||!atlas||data.settings[0]<.5f||!view||!projection||viewport.Width<=0||viewport.Height<=0||viewport.MinDepth!=0||viewport.MaxDepth!=1)return;
  for(unsigned i=0;i<16;++i)if(!std::isfinite(view[i])||!std::isfinite(projection[i]))return;
  c->UpdateSubresource(constants_.Get(),0,nullptr,&data,0,0);std::copy_n(view,16,view_.data());std::copy_n(projection,16,projection_.data());viewport_=viewport;atlas_=atlas;ready_=true;
 }
 bool Matches(const float *view,const float *projection,const D3D11_VIEWPORT &v)const{return ready_&&view&&projection&&!std::memcmp(view_.data(),view,64)&&!std::memcmp(projection_.data(),projection,64)&&v.TopLeftX==viewport_.TopLeftX&&v.TopLeftY==viewport_.TopLeftY&&v.Width==viewport_.Width&&v.Height==viewport_.Height&&v.MinDepth==viewport_.MinDepth&&v.MaxDepth==viewport_.MaxDepth;}
 void Bind(ID3D11DeviceContext *c,bool enabled,ID3D11SamplerState *comparison,ScenePipelineBindings *cache=nullptr){enabled=enabled&&ready_;auto *cb=enabled?constants_.Get():nullptr;
  if(cache)cache->ExtraSingle(ScenePipelineBindings::LocalConstants,cb,[&]{c->PSSetConstantBuffers(11,1,&cb);});else c->PSSetConstantBuffers(11,1,&cb);
  auto *atlas=enabled?atlas_.Get():nullptr;
  if(cache)cache->ExtraSingle(ScenePipelineBindings::LocalAtlas,atlas,[&]{c->PSSetShaderResources(36,1,&atlas);});else c->PSSetShaderResources(36,1,&atlas);
  if(cache)cache->ExtraSingle(ScenePipelineBindings::LocalSampler,comparison,[&]{c->PSSetSamplers(13,1,&comparison);});else c->PSSetSamplers(13,1,&comparison);}
 bool Prepared()const{return ready_;}
 std::uint64_t PayloadBytes()const{return constants_?sizeof(LocalShadowConstants):0;}
};
}
