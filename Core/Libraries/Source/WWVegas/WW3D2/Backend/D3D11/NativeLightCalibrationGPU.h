// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeLightCalibration.h"
#include "ConstantUploadCache.h"
#include "ScenePipelineBindings.h"
#include <wrl/client.h>
namespace GenZH::Modern {
class NativeLightCalibrationGPU {
 Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
 ConstantUploadCache<sizeof(NativeLightData)> uploads_;
 NativeLightCache cache_;
public:
 void Reset(){constants_.Reset();uploads_.Reset();cache_.Reset();}
 HRESULT Create(ID3D11Device *device){
  Reset();if(!device)return E_INVALIDARG;
  D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(NativeLightData);desc.Usage=D3D11_USAGE_DYNAMIC;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
  return device->CreateBuffer(&desc,nullptr,constants_.GetAddressOf());
 }
 static void BindCalibration(ID3D11DeviceContext *context,ScenePipelineBindings *cache,ID3D11Buffer *bind){
  if(cache)cache->ExtraSingle(ScenePipelineBindings::CalibrationConstants,bind,[&]{context->PSSetConstantBuffers(12,1,&bind);});else context->PSSetConstantBuffers(12,1,&bind);
 }
 template<class Native> HRESULT Prepare(ID3D11DeviceContext *context,const Native &c,bool enabled,ScenePipelineBindings *cache=nullptr){
  if(!context)return E_INVALIDARG;
  ID3D11Buffer *bind=nullptr;
  if(enabled&&constants_){
   NativeLightData data;
   if(cache_.Resolve(c,data)){
    const auto result=uploads_.Upload(context,constants_.Get(),&data,cache_.stats.uploads,cache_.stats.skips);
    if(FAILED(result)){BindCalibration(context,cache,bind);return result;}
    bind=constants_.Get();
   }
  }else ++cache_.stats.excluded;
  BindCalibration(context,cache,bind);return bind?S_OK:S_FALSE;
 }
 bool Ready()const{return bool(constants_);}
 const NativeLightStats &Stats()const{return cache_.stats;}
 void ClearStats(){cache_.stats={};}
 std::size_t GPUPayloadBytes()const{return constants_?sizeof(NativeLightData):0;}
 std::size_t CPUPayloadBytes()const{return cache_.PayloadBytes();}
};
}
