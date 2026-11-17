// SPDX-License-Identifier: GPL-3.0-or-later
#include "AtmosphereGPU.h"
#include "CloudNoise.h"
#include "CloudLayer.h"
#include "AtmosphereShaderSource.h"
#include "ShaderBytecodeCache.h"
#include <chrono>
#include <cstring>
namespace GenZH::Modern {
void AtmosphereGPU::Reset(){*this=AtmosphereGPU{};}
HRESULT AtmosphereGPU::Create(ID3D11Device *d,UINT width,UINT height,WeatherMode mode,bool diagnosticCloudSky){
 Reset();if(!d||!width||!height)return E_INVALIDARG;if(mode==WeatherMode::Off)return S_FALSE;
 AtmosphereGPU a;a.controller_.Reset(mode);a.diagnosticCloudSky_=diagnosticCloudSky;HRESULT hr;
 auto compile=[&](const char *entry,const char *profile,ComPtr<ID3DBlob> &blob){bool hit=false;std::string error;
  const HRESULT r=CompileCachedShader(AtmosphereShaderSource,entry,profile,blob,error,hit);
  if(FAILED(r))GenZHLoad::Record("atmosphere_shader_error",std::string(entry)+": "+error,0);return r;};
 ComPtr<ID3DBlob> code;
 if(FAILED(hr=compile("CSCloudShadow","cs_5_0",code))||FAILED(hr=d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.cloudCS_.GetAddressOf())))return hr;
 if(FAILED(hr=compile("CSAtmosphere","cs_5_0",code))||FAILED(hr=d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.volumeCS_.GetAddressOf())))return hr;
 if(FAILED(hr=compile("CSLocalClusters","cs_5_0",code))||FAILED(hr=d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.localClusterCS_.GetAddressOf())))return hr;
 if(FAILED(hr=compile("VSAtmosphere","vs_5_0",code))||FAILED(hr=d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.quadVS_.GetAddressOf())))return hr;
 if(FAILED(hr=compile("VSRain","vs_5_0",code))||FAILED(hr=d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.rainVS_.GetAddressOf())))return hr;
 if(FAILED(hr=compile(diagnosticCloudSky?"PSCloudSky":"PSRTSBackground","ps_5_0",code))||FAILED(hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.skyPS_.GetAddressOf())))return hr;
 if(diagnosticCloudSky){if(FAILED(hr=compile("PSCloudResolve","ps_5_0",code))||FAILED(hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.resolvePS_.GetAddressOf())))return hr;}
 if(diagnosticCloudSky){if(FAILED(hr=compile("PSCloudTemporal","ps_5_0",code))||FAILED(hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.temporalPS_.GetAddressOf())))return hr;}
 if(FAILED(hr=compile("PSRain","ps_5_0",code))||FAILED(hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.rainPS_.GetAddressOf())))return hr;
 if(FAILED(hr=compile("PSShroudFog","ps_5_0",code))||FAILED(hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.shroudPS_.GetAddressOf())))return hr;
 if(FAILED(hr=compile("PSShroudFade","ps_5_0",code))||FAILED(hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,a.shroudFadePS_.GetAddressOf())))return hr;
 std::vector<std::uint8_t> noise;
 {GenZHLoad::Scope timing("cloud_noise_v36","128 cubed periodic gradient/Worley RG8");
  wchar_t folder[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",folder,32768);
  if(length&&length<32768)timing.hit=LoadCloudNoise(std::filesystem::path(folder)/L"cloud-noise-v36.bin",noise);
  if(!timing.hit)noise=GenerateCloudNoise();timing.bytes=noise.size();
 }
 auto noiseMips=CloudNoisePyramid(std::move(noise));std::array<D3D11_SUBRESOURCE_DATA,CloudNoiseMips> init{};
 for(unsigned mip=0;mip<CloudNoiseMips;++mip){const auto side=CloudNoiseSide>>mip;init[mip]={noiseMips[mip].data(),side*2,side*side*2};}
 D3D11_TEXTURE3D_DESC td{};td.Width=td.Height=td.Depth=CloudNoiseSide;td.MipLevels=CloudNoiseMips;td.Format=DXGI_FORMAT_R8G8_UNORM;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture3D> texture;
 if(FAILED(hr=d->CreateTexture3D(&td,init.data(),texture.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(texture.Get(),nullptr,a.noise_.GetAddressOf())))return hr;
 texture.Reset();td.Width=160;td.Height=90;td.Depth=64;td.MipLevels=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
 if(FAILED(hr=d->CreateTexture3D(&td,nullptr,texture.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(texture.Get(),nullptr,a.volume_.GetAddressOf()))||FAILED(hr=d->CreateUnorderedAccessView(texture.Get(),nullptr,a.volumeUAV_.GetAddressOf())))return hr;
 D3D11_TEXTURE2D_DESC t2{};t2.Width=t2.Height=256;t2.MipLevels=t2.ArraySize=t2.SampleDesc.Count=1;t2.Format=DXGI_FORMAT_R16_FLOAT;t2.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture2D> tex2;
 if(FAILED(hr=d->CreateTexture2D(&t2,nullptr,tex2.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(tex2.Get(),nullptr,a.cloudShadow_.GetAddressOf()))||FAILED(hr=d->CreateUnorderedAccessView(tex2.Get(),nullptr,a.cloudUAV_.GetAddressOf())))return hr;
 if(diagnosticCloudSky){
 t2.Width=a.skyWidth_=std::max(1u,width/2);t2.Height=a.skyHeight_=std::max(1u,height/2);t2.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;t2.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;tex2.Reset();
 if(FAILED(hr=d->CreateTexture2D(&t2,nullptr,tex2.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(tex2.Get(),nullptr,a.sky_.GetAddressOf()))||FAILED(hr=d->CreateRenderTargetView(tex2.Get(),nullptr,a.skyTarget_.GetAddressOf())))return hr;
 for(auto &bank:a.skyHistory_){tex2.Reset();if(FAILED(hr=d->CreateTexture2D(&t2,nullptr,tex2.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(tex2.Get(),nullptr,bank.view.GetAddressOf()))||FAILED(hr=d->CreateRenderTargetView(tex2.Get(),nullptr,bank.target.GetAddressOf())))return hr;}
 }
 D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(AtmosphereConstants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.constants_.GetAddressOf())))return hr;
 bd.ByteWidth=32;if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.rainDepthConstants_.GetAddressOf())))return hr;
 bd.ByteWidth=sizeof(ShroudFogConstants);if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.shroudConstants_.GetAddressOf())))return hr;
 bd.ByteWidth=sizeof(ShroudFadeConstants);if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.shroudFadeConstants_.GetAddressOf())))return hr;
 if(diagnosticCloudSky){bd.ByteWidth=sizeof(CloudHistoryConstants);if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.skyHistoryConstants_.GetAddressOf())))return hr;}
 bd.ByteWidth=sizeof(ShadowConstants);if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.shadows_.GetAddressOf())))return hr;
 bd.ByteWidth=sizeof(LocalVolumeConstants);if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.localLightConstants_.GetAddressOf())))return hr;
 bd.ByteWidth=sizeof(LocalShadowConstants);if(FAILED(hr=d->CreateBuffer(&bd,nullptr,a.localShadowConstants_.GetAddressOf())))return hr;
 bd.ByteWidth=LocalVolumeClusters*16;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=16;
 ComPtr<ID3D11Buffer> clusters;if(FAILED(hr=d->CreateBuffer(&bd,nullptr,clusters.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(clusters.Get(),nullptr,a.localClusters_.GetAddressOf()))||FAILED(hr=d->CreateUnorderedAccessView(clusters.Get(),nullptr,a.localClustersUAV_.GetAddressOf())))return hr;
 D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.MaxLOD=D3D11_FLOAT32_MAX;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
 if(FAILED(hr=d->CreateSamplerState(&sd,a.wrap_.GetAddressOf())))return hr;
 sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;if(FAILED(hr=d->CreateSamplerState(&sd,a.clamp_.GetAddressOf())))return hr;
 sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;std::fill_n(sd.BorderColor,4,1.0f);
 if(FAILED(hr=d->CreateSamplerState(&sd,a.comparison_.GetAddressOf())))return hr;
 D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
 if(FAILED(hr=d->CreateDepthStencilState(&dd,a.noDepth_.GetAddressOf())))return hr;
 dd.DepthEnable=TRUE;if(FAILED(hr=d->CreateDepthStencilState(&dd,a.rainDepth_.GetAddressOf())))return hr;
 D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
 if(FAILED(hr=d->CreateRasterizerState(&rd,a.raster_.GetAddressOf())))return hr;
 D3D11_BLEND_DESC blend{};auto &rt=blend.RenderTarget[0];rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
 if(FAILED(hr=d->CreateBlendState(&blend,a.opaque_.GetAddressOf())))return hr;
 rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_SRC_ALPHA;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
 if(FAILED(hr=d->CreateBlendState(&blend,a.rainBlend_.GetAddressOf())))return hr;
 // Fog of war: premultiplied in-scattering plus scene*transmission (alpha);
 // the flat mode multiplies by the native shroud colour like the original pass.
 rt.SrcBlend=D3D11_BLEND_ONE;rt.DestBlend=D3D11_BLEND_SRC_ALPHA;rt.SrcBlendAlpha=D3D11_BLEND_ZERO;rt.DestBlendAlpha=D3D11_BLEND_ONE;
 if(FAILED(hr=d->CreateBlendState(&blend,a.shroudBlend_.GetAddressOf())))return hr;
 rt.SrcBlend=D3D11_BLEND_ZERO;rt.DestBlend=D3D11_BLEND_SRC_COLOR;
 if(FAILED(hr=d->CreateBlendState(&blend,a.shroudFlatBlend_.GetAddressOf())))return hr;
 if(FAILED(hr=a.CreateProbes(d)))return hr;
 a.timer_.Initialize(d);a.shroudTimer_.Initialize(d);*this=std::move(a);return S_OK;
}
HRESULT AtmosphereGPU::Prepare(ID3D11DeviceContext *c,const float *view,const float *projection,const float *sun,const float *color,
 const ShadowConstants &shadows,ID3D11ShaderResourceView *shadowMap,const D3D11_VIEWPORT &viewport,double delta,const float *ambient){
 using namespace DirectX;ready_=false;if(!Available()||!c||!view||!projection||!sun||!color)return S_FALSE;
 for(unsigned i=0;i<16;++i)if(!std::isfinite(view[i])||!std::isfinite(projection[i]))return S_FALSE;
 for(unsigned i=0;i<3;++i)if(!std::isfinite(sun[i])||!std::isfinite(color[i]))return S_FALSE;
 if(ambient)for(unsigned i=0;i<3;++i)if(!std::isfinite(ambient[i]))return S_FALSE;
 if(viewport.Width<=0||viewport.Height<=0||viewport.MinDepth!=0||viewport.MaxDepth!=1)return S_FALSE;
 const auto start=std::chrono::steady_clock::now();XMFLOAT4X4 v,p;std::memcpy(&v,view,64);std::memcpy(&p,projection,64);
 XMVECTOR det;auto vp=XMLoadFloat4x4(&v)*XMLoadFloat4x4(&p);auto inv=XMMatrixInverse(&det,vp);
 if(!ShadowFinite(det)||std::abs(XMVectorGetX(det))<1e-12f)return S_FALSE;
 auto eye=XMVector3TransformCoord(XMVectorZero(),XMMatrixInverse(nullptr,XMLoadFloat4x4(&v)));
 if(!ShadowFinite(eye))return S_FALSE;
 auto light=XMVectorSet(sun[0],sun[1],sun[2],0);if(XMVectorGetX(XMVector3LengthSq(light))<1e-8f)return S_FALSE;
 const bool firstPreparation=!ticked_;
 if(firstPreparation)probeState_.Advance(delta);
 auto state=ticked_?controller_.Sample():controller_.Update(delta);ticked_=true;data_={};
 XMFLOAT4X4 matrix;XMStoreFloat4x4(&matrix,inv);std::memcpy(data_.inverseVP,&matrix,64);XMStoreFloat4x4(&matrix,vp);std::memcpy(data_.viewProjection,&matrix,64);
 XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(data_.camera),eye);data_.camera[3]=1;
 XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(data_.sun),XMVector3Normalize(light));
 data_.sun[3]=std::max(0.0f,color[0]*.2126f+color[1]*.7152f+color[2]*.0722f)*3.14159265f;
 for(unsigned i=0;i<3;++i)data_.sunColor[i]=std::max(0.0f,color[i])/std::max(data_.sun[3]/3.14159265f,.001f);
 // Native map ambient is already linear. No artificial night or minimum light.
 if(ambient)for(unsigned i=0;i<3;++i)data_.ambient[i]=std::max(0.0f,ambient[i]);
 data_.weather[0]=state.coverage;data_.weather[1]=state.rain;data_.weather[2]=state.wetness;data_.weather[3]=state.extinction;
 // Integrated wind displacement avoids discontinuities when wind speed changes.
 data_.wind[0]=state.offsetX;data_.wind[1]=state.offsetY;data_.wind[2]=state.time;data_.wind[3]=state.windX;
 const auto layer=WeatherCloudLayer(state.coverage);
 data_.range[0]=4096;data_.range[1]=layer.base;data_.range[2]=layer.top;data_.range[3]=layer.extinction;
 if(state.flash>0){
  if(flashEvent_!=state.flashEvent){
   flashEvent_=state.flashEvent;const auto seed=WeatherController::VisualHash(static_cast<std::uint32_t>(flashEvent_)^static_cast<std::uint32_t>(mapEpoch_));
   const float x=float(seed&65535)/65535-.5f,y=float(seed>>16)/65535-.5f;
   flashAnchor_[0]=std::floor(data_.camera[0]/128)*128+x*2200;flashAnchor_[1]=std::floor(data_.camera[1]/128)*128+y*2200;
   flashAnchor_[2]=data_.range[1]+(data_.range[2]-data_.range[1])*.45f;++stats.flashEvents;
  }
  std::copy_n(flashAnchor_,3,data_.flashPosition);data_.flashPosition[3]=650;
  data_.flashRadiance[0]=state.flash*10;data_.flashRadiance[1]=state.flash*11;data_.flashRadiance[2]=state.flash*12;data_.flashRadiance[3]=state.flash;
  // Duplicate primary flushes do not advance weather or count another event.
  if(firstPreparation)++stats.flashFrames;
 }
 data_.shadowArea[0]=std::floor(data_.camera[0]/64)*64;data_.shadowArea[1]=std::floor(data_.camera[1]/64)*64;data_.shadowArea[2]=8192;
 data_.viewDepth[0]=v._13;data_.viewDepth[1]=v._23;data_.viewDepth[2]=v._33;data_.viewDepth[3]=v._43;
 std::memcpy(view_,view,64);std::memcpy(projection_,projection,64);viewport_=viewport;
 c->UpdateSubresource(constants_.Get(),0,nullptr,&data_,0,0);c->UpdateSubresource(shadows_.Get(),0,nullptr,&shadows,0,0);
 ID3D11Buffer *cb=constants_.Get();c->CSSetConstantBuffers(7,1,&cb);cb=shadows_.Get();c->CSSetConstantBuffers(2,1,&cb);
 ID3D11ShaderResourceView *empty[3]{};c->PSSetShaderResources(22,3,empty);c->CSSetShaderResources(22,3,empty);
 ID3D11ShaderResourceView *noise=noise_.Get();c->CSSetShaderResources(22,1,&noise);
 ID3D11SamplerState *samplers[]={wrap_.Get(),clamp_.Get()};c->CSSetSamplers(11,2,samplers);auto *cmp=comparison_.Get();c->CSSetSamplers(9,1,&cmp);
 c->CSSetSamplers(13,1,&cmp);
 timer_.Poll(c,[&](double ms){stats.gpuMs+=ms;++stats.gpuSamples;});timer_.Begin(c);
 c->UpdateSubresource(localLightConstants_.Get(),0,nullptr,&localLights_,0,0);auto *localCB=localLightConstants_.Get();c->CSSetConstantBuffers(10,1,&localCB);
 c->UpdateSubresource(localShadowConstants_.Get(),0,nullptr,&localShadowData_,0,0);auto *localShadowCB=localShadowConstants_.Get();c->CSSetConstantBuffers(11,1,&localShadowCB);auto *localAtlas=localShadowAtlas_.Get();c->CSSetShaderResources(36,1,&localAtlas);
 ID3D11ShaderResourceView *localView=nullptr;c->CSSetShaderResources(35,1,&localView);
 if(localLights_.metadata[0]){auto *clusters=localClustersUAV_.Get();c->CSSetUnorderedAccessViews(0,1,&clusters,nullptr);c->CSSetShader(localClusterCS_.Get(),nullptr,0);c->Dispatch(5,3,2);++stats.localClusterPasses;stats.localLightSources+=localLights_.metadata[0];
  clusters=nullptr;c->CSSetUnorderedAccessViews(0,1,&clusters,nullptr);localView=localClusters_.Get();c->CSSetShaderResources(35,1,&localView);
 }
 auto *output=cloudUAV_.Get();c->CSSetUnorderedAccessViews(0,1,&output,nullptr);c->CSSetShader(cloudCS_.Get(),nullptr,0);c->Dispatch(32,32,1);++stats.cloudPasses;
 output=nullptr;c->CSSetUnorderedAccessViews(0,1,&output,nullptr);
 auto *cloud=cloudShadow_.Get();c->CSSetShaderResources(24,1,&cloud);c->CSSetShaderResources(10,1,&shadowMap);
 output=volumeUAV_.Get();c->CSSetUnorderedAccessViews(0,1,&output,nullptr);c->CSSetShader(volumeCS_.Get(),nullptr,0);c->Dispatch(20,12,1);++stats.volumePasses;
 output=nullptr;c->CSSetUnorderedAccessViews(0,1,&output,nullptr);c->CSSetShaderResources(22,3,empty);shadowMap=nullptr;c->CSSetShaderResources(10,1,&shadowMap);c->CSSetShader(nullptr,nullptr,0);
 localView=nullptr;c->CSSetShaderResources(35,1,&localView);localCB=nullptr;c->CSSetConstantBuffers(10,1,&localCB);
 localAtlas=nullptr;c->CSSetShaderResources(36,1,&localAtlas);localShadowCB=nullptr;c->CSSetConstantBuffers(11,1,&localShadowCB);
 timer_.End(c);
 // The cube uses the same density source and sun as the visible sky.
 auto *probeNoise=noise_.Get();c->CSSetShaderResources(22,1,&probeNoise);PrepareProbes(c);probeNoise=nullptr;c->CSSetShaderResources(22,1,&probeNoise);
 ready_=true;++stats.preparations;stats.cpuMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return S_OK;
}
bool AtmosphereGPU::Matches(const float *view,const float *projection,const D3D11_VIEWPORT &vp)const{
 return ready_&&view&&projection&&!std::memcmp(view_,view,64)&&!std::memcmp(projection_,projection,64)&&
 vp.TopLeftX==viewport_.TopLeftX&&vp.TopLeftY==viewport_.TopLeftY&&vp.Width==viewport_.Width&&vp.Height==viewport_.Height&&vp.MinDepth==viewport_.MinDepth&&vp.MaxDepth==viewport_.MaxDepth;
}
void AtmosphereGPU::Bind(ID3D11DeviceContext *c,bool enabled,ScenePipelineBindings *cache){
 enabled=enabled&&ready_;auto *cb=enabled?constants_.Get():nullptr;if(cache)cache->ExtraSingle(ScenePipelineBindings::AtmosphereConstants,cb,[&]{c->PSSetConstantBuffers(7,1,&cb);});else c->PSSetConstantBuffers(7,1,&cb);
 ID3D11ShaderResourceView *views[]={enabled?noise_.Get():nullptr,enabled?volume_.Get():nullptr,enabled?cloudShadow_.Get():nullptr};if(cache)cache->ExtraArray(ScenePipelineBindings::AtmosphereNoise,views,3,[&]{c->PSSetShaderResources(22,3,views);});else c->PSSetShaderResources(22,3,views);
 // perf114: a scene draw always follows with the local surface bind, which rewrites these three slots; only the direct path (fog pass) keeps them.
 if(!cache){auto *localCB=enabled?localShadowConstants_.Get():nullptr;c->PSSetConstantBuffers(11,1,&localCB);auto *localAtlas=enabled?localShadowAtlas_.Get():nullptr;c->PSSetShaderResources(36,1,&localAtlas);auto *localCmp=comparison_.Get();c->PSSetSamplers(13,1,&localCmp);}
 ID3D11ShaderResourceView *probes[]={enabled?probes_[currentProbe_].reflection.Get():nullptr,enabled?probes_[currentProbe_].diffuse.Get():nullptr,
                                   enabled?probes_[previousProbe_].reflection.Get():nullptr,enabled?probes_[previousProbe_].diffuse.Get():nullptr};
 if(cache)cache->ExtraArray(ScenePipelineBindings::AtmosphereProbes,probes,4,[&]{c->PSSetShaderResources(29,4,probes);});else c->PSSetShaderResources(29,4,probes);
 ID3D11SamplerState *samplers[]={wrap_.Get(),clamp_.Get()};if(cache)cache->ExtraArray(ScenePipelineBindings::AtmosphereSamplers,samplers,2,[&]{c->PSSetSamplers(11,2,samplers);});else c->PSSetSamplers(11,2,samplers);if(enabled)++stats.binds;
}
void AtmosphereGPU::DrawSky(ID3D11DeviceContext *c,ID3D11RenderTargetView *scene){
 if(!ready_||skyDrawn_||!scene||!c)return;skyDrawn_=true;Bind(c,true);
 if(!diagnosticCloudSky_){
  // RTS clouds affect ground lighting and probes, not a visible sky volume.
  // No cloud raymarch, HDR history allocation, low-res pass or resolve.
  auto *cb=constants_.Get();c->VSSetConstantBuffers(7,1,&cb);
  c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->GSSetShader(nullptr,nullptr,0);
  c->VSSetShader(quadVS_.Get(),nullptr,0);c->PSSetShader(skyPS_.Get(),nullptr,0);
  c->OMSetRenderTargets(1,&scene,nullptr);c->RSSetViewports(1,&viewport_);
  c->OMSetDepthStencilState(noDepth_.Get(),0);c->OMSetBlendState(opaque_.Get(),nullptr,0xffffffff);c->RSSetState(raster_.Get());
  c->Draw(3,0);++stats.skyPasses;return;
 }
 auto history=skyHistoryState_.Prepare(data_,projection_,skyWidth_,skyHeight_);
 const bool flash=data_.flashRadiance[3]>0;
 if(flash||skyFlash_)history.settings[0]=0;skyFlash_=flash;
 if(history.settings[0]<.5f)++stats.cloudHistoryResets;
 c->UpdateSubresource(skyHistoryConstants_.Get(),0,nullptr,&history,0,0);auto *historyCB=skyHistoryConstants_.Get();c->PSSetConstantBuffers(9,1,&historyCB);
 auto *cb=constants_.Get();c->VSSetConstantBuffers(7,1,&cb);c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->GSSetShader(nullptr,nullptr,0);c->VSSetShader(quadVS_.Get(),nullptr,0);
 c->OMSetDepthStencilState(noDepth_.Get(),0);c->OMSetBlendState(opaque_.Get(),nullptr,0xffffffff);c->RSSetState(raster_.Get());
 ID3D11ShaderResourceView *sky=nullptr;c->PSSetShaderResources(25,1,&sky);
 auto *target=skyTarget_.Get();c->OMSetRenderTargets(1,&target,nullptr);
 D3D11_VIEWPORT low{0,0,static_cast<float>(skyWidth_),static_cast<float>(skyHeight_),0,1};c->RSSetViewports(1,&low);c->PSSetShader(skyPS_.Get(),nullptr,0);c->Draw(3,0);
 const unsigned next=1-currentSky_;target=skyHistory_[next].target.Get();c->OMSetRenderTargets(1,&target,nullptr);
 sky=sky_.Get();c->PSSetShaderResources(25,1,&sky);auto *previous=skyHistory_[currentSky_].view.Get();c->PSSetShaderResources(34,1,&previous);
 c->PSSetShader(temporalPS_.Get(),nullptr,0);c->Draw(3,0);previous=nullptr;c->PSSetShaderResources(34,1,&previous);
 c->OMSetRenderTargets(1,&scene,nullptr);c->RSSetViewports(1,&viewport_);sky=skyHistory_[next].view.Get();c->PSSetShaderResources(25,1,&sky);c->PSSetShader(resolvePS_.Get(),nullptr,0);c->Draw(3,0);
 sky=nullptr;c->PSSetShaderResources(25,1,&sky);historyCB=nullptr;c->PSSetConstantBuffers(9,1,&historyCB);
 currentSky_=next;skyHistoryState_.Commit(data_,projection_);++stats.skyPasses;++stats.cloudHistoryPasses;
}
void AtmosphereGPU::DrawRain(ID3D11DeviceContext *c,ID3D11RenderTargetView *scene,ID3D11DepthStencilView *depth,ID3D11ShaderResourceView *sceneDepth,ID3D11DepthStencilView *readOnlyDepth){
 if(!c||!ready_||data_.weather[1]<.01f||!scene||!depth)return;
 bool soft=sceneDepth&&readOnlyDepth&&viewport_.Width>0&&viewport_.Height>0&&viewport_.MaxDepth>viewport_.MinDepth;
 if(soft){
  ComPtr<ID3D11Resource> a,b,d;sceneDepth->GetResource(a.GetAddressOf());readOnlyDepth->GetResource(b.GetAddressOf());depth->GetResource(d.GetAddressOf());
  D3D11_DEPTH_STENCIL_VIEW_DESC dv{};readOnlyDepth->GetDesc(&dv);
  soft=a.Get()==b.Get()&&a.Get()==d.Get()&&(dv.Flags&(D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL))==(D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL);
 }
 const float settings[8]={viewport_.TopLeftX,viewport_.TopLeftY,viewport_.Width,viewport_.Height,soft?1.f:0.f,2.f,viewport_.MinDepth,viewport_.MaxDepth};
 c->UpdateSubresource(rainDepthConstants_.Get(),0,nullptr,settings,0,0);auto *depthCB=rainDepthConstants_.Get();c->PSSetConstantBuffers(13,1,&depthCB);
 Bind(c,true);auto *cb=constants_.Get();c->VSSetConstantBuffers(7,1,&cb);c->OMSetRenderTargets(1,&scene,soft?readOnlyDepth:depth);c->RSSetViewports(1,&viewport_);c->RSSetState(raster_.Get());
 auto *depthInput=soft?sceneDepth:nullptr;c->PSSetShaderResources(37,1,&depthInput);
 c->OMSetDepthStencilState(rainDepth_.Get(),0);c->OMSetBlendState(rainBlend_.Get(),nullptr,0xffffffff);
 c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(rainVS_.Get(),nullptr,0);c->PSSetShader(rainPS_.Get(),nullptr,0);
 c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->GSSetShader(nullptr,nullptr,0);c->DrawInstanced(6,4096,0,0);++stats.rainPasses;
 depthInput=nullptr;c->PSSetShaderResources(37,1,&depthInput);depthCB=nullptr;c->PSSetConstantBuffers(13,1,&depthCB);
}
bool AtmosphereGPU::UpdateShroudVision(ID3D11DeviceContext *c,const std::uint8_t *cells,UINT cellsX,UINT cellsY,std::uint8_t border,std::uint64_t revision,
                                       UINT width,UINT height,double seconds){
 if(!c||!cells||!shroudFadePS_||!cellsX||!cellsY||cellsX+2>width||cellsY+2>height||width>8192||height>8192){visionValid_=false;return false;}
 HRESULT hr=S_OK;
 if(width!=visionWidth_||height!=visionHeight_||!visionTarget_){
  ComPtr<ID3D11Device> d;c->GetDevice(d.GetAddressOf());visionValid_=false;visionRevision_=0;visionWidth_=visionHeight_=0;
  visionTarget_.Reset();visionTargetView_.Reset();for(auto &bank:vision_)bank={};
  D3D11_TEXTURE2D_DESC t{};t.Width=width;t.Height=height;t.MipLevels=t.ArraySize=t.SampleDesc.Count=1;t.Format=DXGI_FORMAT_R8_UNORM;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  if(FAILED(hr=d->CreateTexture2D(&t,nullptr,visionTarget_.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(visionTarget_.Get(),nullptr,visionTargetView_.GetAddressOf())))return false;
  // Full mip chain: the fog shape reads the vision one mip down (2x2 cells).
  t.MipLevels=0;t.Format=DXGI_FORMAT_R16_FLOAT;t.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;t.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
  for(auto &bank:vision_){ComPtr<ID3D11Texture2D> texture;D3D11_RENDER_TARGET_VIEW_DESC rv{};rv.Format=t.Format;rv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
   if(FAILED(hr=d->CreateTexture2D(&t,nullptr,texture.GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(texture.Get(),nullptr,bank.view.GetAddressOf()))||
      FAILED(hr=d->CreateRenderTargetView(texture.Get(),&rv,bank.target.GetAddressOf()))){visionTarget_.Reset();return false;}}
  visionWidth_=width;visionHeight_=height;
 }
 const bool snap=!visionValid_;
 if(revision!=visionRevision_||snap){
  visionUpload_.assign(std::size_t(width)*height,border);
  for(UINT y=0;y<cellsY;++y)std::memcpy(&visionUpload_[std::size_t(y+1)*width+1],cells+std::size_t(y)*cellsX,cellsX);
  c->UpdateSubresource(visionTarget_.Get(),0,nullptr,visionUpload_.data(),width,0);visionRevision_=revision;
 }
 const auto fade=MakeShroudFadeConstants(seconds,snap);c->UpdateSubresource(shroudFadeConstants_.Get(),0,nullptr,&fade,0,0);
 const unsigned next=1-visionCurrent_;
 ID3D11ShaderResourceView *inputs[]={vision_[visionCurrent_].view.Get(),visionTargetView_.Get()};ID3D11ShaderResourceView *none[2]{};c->PSSetShaderResources(40,2,none);
 auto *target=vision_[next].target.Get();c->OMSetRenderTargets(1,&target,nullptr);
 const D3D11_VIEWPORT area{0,0,float(width),float(height),0,1};c->RSSetViewports(1,&area);c->RSSetState(raster_.Get());
 c->OMSetDepthStencilState(noDepth_.Get(),0);c->OMSetBlendState(opaque_.Get(),nullptr,0xffffffff);
 c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->GSSetShader(nullptr,nullptr,0);
 auto *cb=shroudFadeConstants_.Get();c->PSSetConstantBuffers(6,1,&cb);c->PSSetShaderResources(40,2,inputs);
 c->VSSetShader(quadVS_.Get(),nullptr,0);c->PSSetShader(shroudFadePS_.Get(),nullptr,0);c->Draw(3,0);
 c->PSSetShaderResources(40,2,none);cb=nullptr;c->PSSetConstantBuffers(6,1,&cb);target=nullptr;c->OMSetRenderTargets(1,&target,nullptr);
 c->GenerateMips(vision_[next].view.Get());
 visionCurrent_=next;visionValid_=true;return true;
}
bool AtmosphereGPU::DrawShroudFog(ID3D11DeviceContext *c,ID3D11RenderTargetView *scene,ID3D11ShaderResourceView *sceneDepth,
                                  ID3D11ShaderResourceView *shroud,const D3D11ShroudMapping &mapping,bool flat,ID3D11ShaderResourceView *vision){
 if(!c||!ready_||!scene||!sceneDepth||!shroud||!shroudPS_||!ValidShroudMapping(mapping))return false;
 if(viewport_.Width<=0||viewport_.Height<=0||viewport_.MaxDepth<=viewport_.MinDepth)return false;
 const float area[4]={viewport_.TopLeftX,viewport_.TopLeftY,viewport_.Width,viewport_.Height};
 const auto data=MakeShroudFogConstants(mapping,area,viewport_.MinDepth,viewport_.MaxDepth,flat,vision!=nullptr);
 c->UpdateSubresource(shroudConstants_.Get(),0,nullptr,&data,0,0);
 shroudTimer_.Poll(c,[&](double ms){stats.shroudGpuMs+=ms;++stats.shroudGpuSamples;});shroudTimer_.Begin(c);
 Bind(c,true);auto *cb=constants_.Get();c->VSSetConstantBuffers(7,1,&cb);
 auto *fogCB=shroudConstants_.Get();c->PSSetConstantBuffers(12,1,&fogCB);
 // Depth is an input here: bind the colour target alone before the depth view.
 c->OMSetRenderTargets(1,&scene,nullptr);ID3D11ShaderResourceView *inputs[]={sceneDepth,shroud,vision};c->PSSetShaderResources(38,3,inputs);
 c->RSSetViewports(1,&viewport_);c->RSSetState(raster_.Get());c->OMSetDepthStencilState(noDepth_.Get(),0);
 c->OMSetBlendState(flat?shroudFlatBlend_.Get():shroudBlend_.Get(),nullptr,0xffffffff);
 c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->GSSetShader(nullptr,nullptr,0);
 c->VSSetShader(quadVS_.Get(),nullptr,0);c->PSSetShader(shroudPS_.Get(),nullptr,0);c->Draw(3,0);
 inputs[0]=inputs[1]=inputs[2]=nullptr;c->PSSetShaderResources(38,3,inputs);fogCB=nullptr;c->PSSetConstantBuffers(12,1,&fogCB);
 shroudTimer_.End(c);++stats.shroudPasses;return true;
}
}
