// SPDX-License-Identifier: GPL-3.0-or-later
#include "AtmosphereGPU.h"
#include "AtmosphereShaderSource.h"
#include "ShaderBytecodeCache.h"
namespace GenZH::Modern {
HRESULT AtmosphereGPU::CreateProbes(ID3D11Device *device){
 auto shader=[&](const char *entry,ComPtr<ID3D11ComputeShader> &out){
  ComPtr<ID3DBlob> code;std::string error;bool hit=false;
  HRESULT hr=CompileCachedShader(AtmosphereShaderSource,entry,"cs_5_0",code,error,hit);
  if(FAILED(hr))return hr;
  return device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,out.GetAddressOf());
 };
 HRESULT hr;if(FAILED(hr=shader("CSWeatherSky",skyProbeCS_))||FAILED(hr=shader("CSWeatherSpecular",specularCS_))||FAILED(hr=shader("CSWeatherDiffuse",diffuseCS_)))return hr;
 auto cube=[&](UINT size,UINT mips,ComPtr<ID3D11ShaderResourceView> &srv,ID3D11Texture2D **out){
  D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=size;desc.MipLevels=mips;desc.ArraySize=6;
  desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
  HRESULT result=device->CreateTexture2D(&desc,nullptr,out);if(FAILED(result))return result;
  return device->CreateShaderResourceView(*out,nullptr,srv.GetAddressOf());
 };
 auto view=[&](ID3D11Texture2D *texture,UINT mip,ComPtr<ID3D11UnorderedAccessView> &out){
  D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
  desc.Texture2DArray.MipSlice=mip;desc.Texture2DArray.ArraySize=6;return device->CreateUnorderedAccessView(texture,&desc,out.GetAddressOf());
 };
 ComPtr<ID3D11Texture2D> texture;
 if(FAILED(hr=cube(64,1,rawSky_,texture.GetAddressOf()))||FAILED(hr=view(texture.Get(),0,rawSkyUAV_)))return hr;
 for(auto &bank:probes_){
  texture.Reset();if(FAILED(hr=cube(64,7,bank.reflection,texture.GetAddressOf())))return hr;
  for(UINT mip=0;mip<7;++mip)if(FAILED(hr=view(texture.Get(),mip,bank.mips[mip])))return hr;
  texture.Reset();if(FAILED(hr=cube(16,1,bank.diffuse,texture.GetAddressOf()))||FAILED(hr=view(texture.Get(),0,bank.diffuseUAV)))return hr;
 }
 D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 if(FAILED(hr=device->CreateBuffer(&desc,nullptr,probeFilter_.GetAddressOf())))return hr;
 probeTimer_.Initialize(device);return S_OK;
}
void AtmosphereGPU::PrepareProbes(ID3D11DeviceContext *context){
 probeTimer_.Poll(context,[&](double ms){stats.probeGpuMs+=ms;++stats.probeGpuSamples;});
 const bool first=!probeState_.Ready();
 if(probeState_.NeedsUpdate(data_.camera,data_.sun,data_.sunColor,data_.ambient)){
  // Explicitly remove all aliases before any UAV write. No scene colour capture.
  ID3D11ShaderResourceView *empty[5]{};context->PSSetShaderResources(29,4,empty);context->CSSetShaderResources(29,5,empty);
  unsigned next=first?0:1-currentProbe_;probeTimer_.Begin(context);
  auto *uav=rawSkyUAV_.Get();context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->CSSetShader(skyProbeCS_.Get(),nullptr,0);context->Dispatch(8,8,6);
  uav=nullptr;context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
  auto *raw=rawSky_.Get();context->CSSetShaderResources(33,1,&raw);auto *filter=probeFilter_.Get();context->CSSetConstantBuffers(8,1,&filter);
  context->CSSetShader(specularCS_.Get(),nullptr,0);
  for(UINT mip=0;mip<7;++mip){
   const UINT side=64>>mip;float settings[]={float(mip)/6,float(side),0,0};context->UpdateSubresource(probeFilter_.Get(),0,nullptr,settings,0,0);
   uav=probes_[next].mips[mip].Get();context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->Dispatch((side+7)/8,(side+7)/8,6);
  }
  uav=probes_[next].diffuseUAV.Get();context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->CSSetShader(diffuseCS_.Get(),nullptr,0);context->Dispatch(2,2,6);
  uav=nullptr;context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);raw=nullptr;context->CSSetShaderResources(33,1,&raw);context->CSSetShader(nullptr,nullptr,0);
  probeTimer_.End(context);previousProbe_=first?next:currentProbe_;currentProbe_=next;
  probeState_.Commit(data_.camera,data_.sun,data_.sunColor,data_.ambient);++stats.probeUpdates;
 }
 data_.shadowArea[3]=probeState_.Ready()?1.f:0.f;
 data_.sunColor[3]=previousProbe_==currentProbe_?1.f:probeState_.Blend();
 context->UpdateSubresource(constants_.Get(),0,nullptr,&data_,0,0);
}
}
