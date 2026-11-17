// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "EnvironmentLighting.h"
#include "ScenePipelineBindings.h"
#include <d3d11.h>
#include <wrl/client.h>
namespace GenZH::Modern {
class EnvironmentGPU {
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> reflection_,brdf_;
 Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
 Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
public:
 struct Constants {float settings[4]{};std::array<std::array<float,4>,9> sh{};};
 static_assert(sizeof(Constants)==160);
 void Reset(){reflection_.Reset();brdf_.Reset();constants_.Reset();sampler_.Reset();}
 bool Ready()const{return bool(constants_);}
 HRESULT Create(ID3D11Device *device,const IBL::Data &data){
  if(!device||!IBL::Valid(data))return E_INVALIDARG;
  EnvironmentGPU result;std::vector<D3D11_SUBRESOURCE_DATA> subresources;std::size_t offset=0;
  for(unsigned face=0;face<6;++face)for(unsigned mip=0;mip<data.mips;++mip){const unsigned side=std::max(1u,data.size>>mip);subresources.push_back({data.radiance.data()+offset,side*16,0});offset+=std::size_t(side)*side;}
  D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=data.size;desc.MipLevels=data.mips;desc.ArraySize=6;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;HRESULT hr=device->CreateTexture2D(&desc,subresources.data(),texture.GetAddressOf());if(FAILED(hr))return hr;
  if(FAILED(hr=device->CreateShaderResourceView(texture.Get(),nullptr,result.reflection_.GetAddressOf())))return hr;
  desc.Width=desc.Height=data.lutSize;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R32G32_FLOAT;desc.MiscFlags=0;
  D3D11_SUBRESOURCE_DATA initial{data.brdf.data(),data.lutSize*8,0};texture.Reset();if(FAILED(hr=device->CreateTexture2D(&desc,&initial,texture.GetAddressOf())))return hr;
  if(FAILED(hr=device->CreateShaderResourceView(texture.Get(),nullptr,result.brdf_.GetAddressOf())))return hr;
  Constants constants;constants.settings[0]=1;constants.settings[1]=static_cast<float>(data.mips-1);constants.settings[2]=static_cast<float>(data.lutSize-1)/data.lutSize;constants.settings[3]=.5f/data.lutSize;constants.sh=data.sh;
  D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(constants);cb.Usage=D3D11_USAGE_IMMUTABLE;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;initial={&constants,0,0};
  if(FAILED(hr=device->CreateBuffer(&cb,&initial,result.constants_.GetAddressOf())))return hr;
  D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
  if(FAILED(hr=device->CreateSamplerState(&sampler,result.sampler_.GetAddressOf())))return hr;
  *this=std::move(result);return S_OK;
 }
 HRESULT LoadProfile(ID3D11Device *device){
  Reset();char mode[16]{};GetEnvironmentVariableA("GENZH_IBL",mode,sizeof(mode));if(!std::strcmp(mode,"off"))return S_FALSE;
  wchar_t profile[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",profile,32768);if(!length||length>=32768)return S_FALSE;
  IBL::Data data;if(!IBL::Load(std::filesystem::path(profile)/L"environment-v46.bin",data))return S_FALSE;
  return Create(device,data);
 }
 void Bind(ID3D11DeviceContext *context,bool enabled=true,ScenePipelineBindings *cache=nullptr)const{
  ID3D11ShaderResourceView *views[]={enabled?reflection_.Get():nullptr,enabled?brdf_.Get():nullptr};
  if(cache)cache->ExtraArray(ScenePipelineBindings::EnvReflection,views,2,[&]{context->PSSetShaderResources(18,2,views);});else context->PSSetShaderResources(18,2,views);
  auto *constants=enabled?constants_.Get():nullptr;
  if(cache)cache->ExtraSingle(ScenePipelineBindings::EnvConstants,constants,[&]{context->PSSetConstantBuffers(5,1,&constants);});else context->PSSetConstantBuffers(5,1,&constants);
  auto *sampler=sampler_.Get();
  if(cache)cache->ExtraSingle(ScenePipelineBindings::EnvSampler,sampler,[&]{context->PSSetSamplers(10,1,&sampler);});else context->PSSetSamplers(10,1,&sampler);
 }
};
}
