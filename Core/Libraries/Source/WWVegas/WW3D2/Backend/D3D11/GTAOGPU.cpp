// SPDX-License-Identifier: GPL-3.0-or-later
#include "GTAOGPU.h"
#include "GTAOShaderSource.h"
#include "ShaderBytecodeCache.h"
#include <algorithm>
#include <chrono>
namespace GenZH::Modern {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
// The native shader state cache is not aware of the post pass. Restore every
// pipeline slot touched by it, then the caller binds the new HDR ping-pong target.
class SavedGTAOState {
 ID3D11DeviceContext *c_;
 ID3D11RenderTargetView *targets_[8]{};
 Ptr<ID3D11DepthStencilView> depth_;
 Ptr<ID3D11ComputeShader> cs_;
 Ptr<ID3D11VertexShader> vs_;
 Ptr<ID3D11PixelShader> ps_;
 Ptr<ID3D11GeometryShader> gs_;
 Ptr<ID3D11HullShader> hs_;
 Ptr<ID3D11DomainShader> ds_;
 Ptr<ID3D11Buffer> csCB_,psCB_;
 ID3D11ShaderResourceView *csViews_[11]{},*psViews_[11]{};
 ID3D11UnorderedAccessView *outputs_[3]{};
 Ptr<ID3D11SamplerState> csSampler_,psSampler_;
 Ptr<ID3D11BlendState> blend_;
 Ptr<ID3D11DepthStencilState> depthState_;
 Ptr<ID3D11RasterizerState> raster_;
 Ptr<ID3D11InputLayout> layout_;
 FLOAT blendFactor_[4]{};UINT sampleMask_=0,stencil_=0;
 D3D11_PRIMITIVE_TOPOLOGY topology_{};
 UINT viewportCount_=16,scissorCount_=16;
 D3D11_VIEWPORT viewports_[16]{};D3D11_RECT scissors_[16]{};
public:
 explicit SavedGTAOState(ID3D11DeviceContext *c):c_(c){
  c->OMGetRenderTargets(8,targets_,depth_.GetAddressOf());c->CSGetShader(cs_.GetAddressOf(),nullptr,nullptr);c->VSGetShader(vs_.GetAddressOf(),nullptr,nullptr);c->PSGetShader(ps_.GetAddressOf(),nullptr,nullptr);c->GSGetShader(gs_.GetAddressOf(),nullptr,nullptr);c->HSGetShader(hs_.GetAddressOf(),nullptr,nullptr);c->DSGetShader(ds_.GetAddressOf(),nullptr,nullptr);
  c->CSGetConstantBuffers(13,1,csCB_.GetAddressOf());c->PSGetConstantBuffers(13,1,psCB_.GetAddressOf());c->CSGetShaderResources(37,11,csViews_);c->PSGetShaderResources(37,11,psViews_);c->CSGetUnorderedAccessViews(0,3,outputs_);c->CSGetSamplers(12,1,csSampler_.GetAddressOf());c->PSGetSamplers(12,1,psSampler_.GetAddressOf());
  c->OMGetBlendState(blend_.GetAddressOf(),blendFactor_,&sampleMask_);c->OMGetDepthStencilState(depthState_.GetAddressOf(),&stencil_);c->RSGetState(raster_.GetAddressOf());c->RSGetViewports(&viewportCount_,viewports_);c->RSGetScissorRects(&scissorCount_,scissors_);c->IAGetInputLayout(layout_.GetAddressOf());c->IAGetPrimitiveTopology(&topology_);
  c->OMSetRenderTargets(0,nullptr,nullptr);Clear();
 }
 void Clear(){ID3D11ShaderResourceView *empty[11]{};ID3D11UnorderedAccessView *none[3]{};c_->CSSetShaderResources(37,11,empty);c_->PSSetShaderResources(37,11,empty);c_->CSSetUnorderedAccessViews(0,3,none,nullptr);}
 ~SavedGTAOState(){
  Clear();c_->OMSetRenderTargets(0,nullptr,nullptr);c_->OMSetRenderTargets(8,targets_,depth_.Get());
  c_->CSSetShader(cs_.Get(),nullptr,0);c_->VSSetShader(vs_.Get(),nullptr,0);c_->PSSetShader(ps_.Get(),nullptr,0);c_->GSSetShader(gs_.Get(),nullptr,0);c_->HSSetShader(hs_.Get(),nullptr,0);c_->DSSetShader(ds_.Get(),nullptr,0);
  auto *cb=csCB_.Get();c_->CSSetConstantBuffers(13,1,&cb);cb=psCB_.Get();c_->PSSetConstantBuffers(13,1,&cb);c_->CSSetShaderResources(37,11,csViews_);c_->PSSetShaderResources(37,11,psViews_);c_->CSSetUnorderedAccessViews(0,3,outputs_,nullptr);
  auto *sampler=csSampler_.Get();c_->CSSetSamplers(12,1,&sampler);sampler=psSampler_.Get();c_->PSSetSamplers(12,1,&sampler);c_->OMSetBlendState(blend_.Get(),blendFactor_,sampleMask_);c_->OMSetDepthStencilState(depthState_.Get(),stencil_);c_->RSSetState(raster_.Get());c_->RSSetViewports(viewportCount_,viewports_);c_->RSSetScissorRects(scissorCount_,scissors_);c_->IASetInputLayout(layout_.Get());c_->IASetPrimitiveTopology(topology_);
  for(auto *target:targets_)if(target)target->Release();for(auto *view:csViews_)if(view)view->Release();for(auto *view:psViews_)if(view)view->Release();for(auto *output:outputs_)if(output)output->Release();
 }
};
bool TextureFits(ID3D11ShaderResourceView *view,ID3D11Device *device,UINT width,UINT height,DXGI_FORMAT expected){
 if(!view)return false;Ptr<ID3D11Device> owner;view->GetDevice(owner.GetAddressOf());if(owner.Get()!=device)return false;
 D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};view->GetDesc(&viewDesc);if(viewDesc.Format!=expected||viewDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D)return false;
 Ptr<ID3D11Resource> resource;view->GetResource(resource.GetAddressOf());Ptr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture)))return false;D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);return desc.Width==width&&desc.Height==height&&desc.SampleDesc.Count==1;
}
}
bool GTAOGPU::PrepareConstants(const ScreenMaterialGPU::Camera &camera,UINT width,UINT height,float radius,float strength,Constants &out){
 if(!width||!height||!std::isfinite(radius)||radius<=0||radius>1000||!std::isfinite(strength)||strength<0||strength>1)return false;
 for(float v:camera.inverseVP)if(!std::isfinite(v))return false;for(float v:camera.viewMatrix)if(!std::isfinite(v))return false;for(float v:camera.projectionMatrix)if(!std::isfinite(v))return false;for(float v:camera.eye)if(!std::isfinite(v))return false;
 for(float v:camera.viewport)if(!std::isfinite(v))return false;
 if(camera.viewport[0]<0||camera.viewport[1]<0||camera.viewport[2]<=0||camera.viewport[3]<=0||camera.viewport[0]+camera.viewport[2]>float(width)||camera.viewport[1]+camera.viewport[3]>float(height)||!std::isfinite(camera.depthRange[0])||!std::isfinite(camera.depthRange[1])||camera.depthRange[0]<0||camera.depthRange[1]>1||camera.depthRange[1]<=camera.depthRange[0])return false;
 const auto *p=camera.projectionMatrix;const float handedness=p[11];
 if(std::abs(std::abs(handedness)-1)>1e-6f||p[0]<=0||p[5]<=0||p[14]>=0||p[10]*handedness<=1||p[15]!=0)return false;
 for(unsigned i:{1u,2u,3u,4u,6u,7u,12u,13u})if(std::abs(p[i])>1e-6f)return false;
 const auto *v=camera.viewMatrix;if(std::abs(v[3])+std::abs(v[7])+std::abs(v[11])>1e-6f||std::abs(v[15]-1)>1e-6f)return false;
 for(unsigned row=0;row<3;++row)for(unsigned other=0;other<3;++other){float dot=0;for(unsigned k=0;k<3;++k)dot+=v[row*4+k]*v[other*4+k];if(std::abs(dot-(row==other?1.f:0.f))>.001f)return false;}
 const float farDepth=-p[14]/(std::abs(p[10])-1);if(!std::isfinite(farDepth)||farDepth<=0||farDepth>1e7)return false;
 out=Constants{};out.camera=camera;out.camera.depthRange[2]=handedness;
 XeGTAO::GTAOSettings settings;settings.Radius=radius;settings.DenoisePasses=2;
 // AO/depth textures share the complete render target, while the native RTS
 // world occupies a subviewport above the HUD. Map its projection into target
 // NDC so view-space reconstruction retains the original physical pixel scale.
 // Keep camera/inverseVP unchanged for world/contact reconstruction below.
 float targetProjection[16];std::copy_n(p,16,targetProjection);
 const float sx=camera.viewport[2]/float(width),sy=camera.viewport[3]/float(height);
 const float ox=(2*camera.viewport[0]+camera.viewport[2])/float(width)-1;
 const float oy=1-(2*camera.viewport[1]+camera.viewport[3])/float(height);
 targetProjection[0]*=sx;targetProjection[5]*=sy;
 targetProjection[8]=p[8]*sx+handedness*ox;targetProjection[9]=p[9]*sy+handedness*oy;
 XeGTAO::GTAOUpdateConstants(out.ao,static_cast<int>(width),static_cast<int>(height),settings,targetProjection,true,0);
 // Positive linear Z for either native handedness; off-center projection supported.
 out.ao.DepthUnpackConsts={-p[14],std::abs(p[10])};
 out.ao.NDCToViewAdd={(-1-targetProjection[8]/handedness)/targetProjection[0],(1-targetProjection[9]/handedness)/targetProjection[5]};
 out.tuning[0]=strength;out.tuning[1]=farDepth;out.filterDimensions[0]=out.filterDimensions[2]=width;out.filterDimensions[1]=out.filterDimensions[3]=height;
 for(float sun:camera.sunDirection)if(!std::isfinite(sun))return false;
 if(camera.sunDirection[3]>.5f){float length=0;for(unsigned k=0;k<3;++k)length+=camera.sunDirection[k]*camera.sunDirection[k];if(std::abs(length-1)>.001f)return false;}
 out.contact[1]=8;out.contact[2]=.5f;out.contact[3]=.03f;
 return true;
}
HRESULT GTAOGPU::Create(ID3D11Device *device,UINT width,UINT height,float radius,float strength,float contactStrength){
 if(!device||!width||!height||width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||!std::isfinite(radius)||radius<=0||radius>1000||!std::isfinite(strength)||strength<0||strength>1)return E_INVALIDARG;
 if(!std::isfinite(contactStrength)||contactStrength<0||contactStrength>1)return E_INVALIDARG;
 GTAOGPU next;next.device_=device;next.width_=width;next.height_=height;next.radius_=radius;next.strength_=strength;next.contactStrength_=contactStrength;HRESULT hr;
 D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=1;for(UINT w=width,h=height;(w>1||h>1)&&desc.MipLevels<5;w=std::max(1u,w/2),h=std::max(1u,h/2))++desc.MipLevels;
 desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;Ptr<ID3D11Texture2D> texture;
 if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,texture.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(texture.Get(),nullptr,next.depthView_.GetAddressOf())))return hr;
 next.depthMips_.resize(desc.MipLevels);next.depthOutputs_.resize(desc.MipLevels);
 for(UINT mip=0;mip<desc.MipLevels;++mip){D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=desc.Format;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MostDetailedMip=mip;sv.Texture2D.MipLevels=1;D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=desc.Format;uv.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;uv.Texture2D.MipSlice=mip;
  if(FAILED(hr=device->CreateShaderResourceView(texture.Get(),&sv,next.depthMips_[mip].GetAddressOf()))||FAILED(hr=device->CreateUnorderedAccessView(texture.Get(),&uv,next.depthOutputs_[mip].GetAddressOf())))return hr;
 }
 desc.MipLevels=1;desc.Format=DXGI_FORMAT_R32_UINT;
 for(unsigned i=0;i<2;++i){texture.Reset();if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,texture.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(texture.Get(),nullptr,next.aoViews_[i].GetAddressOf()))||FAILED(hr=device->CreateUnorderedAccessView(texture.Get(),nullptr,next.aoOutputs_[i].GetAddressOf())))return hr;}
 desc.Format=DXGI_FORMAT_R8_UNORM;texture.Reset();if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,texture.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(texture.Get(),nullptr,next.edgesView_.GetAddressOf()))||FAILED(hr=device->CreateUnorderedAccessView(texture.Get(),nullptr,next.edgesOutput_.GetAddressOf())))return hr;
 desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;texture.Reset();if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,texture.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(texture.Get(),nullptr,next.colorView_.GetAddressOf()))||FAILED(hr=device->CreateRenderTargetView(texture.Get(),nullptr,next.colorTarget_.GetAddressOf())))return hr;
 D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(Constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(FAILED(hr=device->CreateBuffer(&bd,nullptr,next.constants_.GetAddressOf())))return hr;
 auto compile=[&](const char *entry,const char *profile,Ptr<ID3DBlob> &code){bool hit=false;std::string error;return CompileCachedShader(GTAOShaderSource,entry,profile,code,error,hit);};Ptr<ID3DBlob> code;
 for(const auto &job:{std::pair<const char*,Ptr<ID3D11ComputeShader>*>{"CSGTAODepth",&next.depthCS_},{"CSGTAOReduce",&next.reduceCS_},{"CSGTAO",&next.mainCS_},{"CSGTAODenoise",&next.denoiseCS_}}){if(FAILED(hr=compile(job.first,"cs_5_0",code))||FAILED(hr=device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,job.second->GetAddressOf())))return hr;}
 if(FAILED(hr=compile("VSGTAO","vs_5_0",code))||FAILED(hr=device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,next.compositeVS_.GetAddressOf()))||FAILED(hr=compile("PSGTAO","ps_5_0",code))||FAILED(hr=device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,next.compositePS_.GetAddressOf())))return hr;
 D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;sampler.MaxAnisotropy=1;if(FAILED(hr=device->CreateSamplerState(&sampler,next.sampler_.GetAddressOf())))return hr;
 D3D11_BLEND_DESC blend{};auto &b=blend.RenderTarget[0];b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlend=b.DestBlendAlpha=D3D11_BLEND_ZERO;b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;if(FAILED(hr=device->CreateBlendState(&blend,next.blend_.GetAddressOf())))return hr;
 D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthFunc=D3D11_COMPARISON_ALWAYS;depth.StencilReadMask=depth.StencilWriteMask=255;depth.FrontFace.StencilFunc=depth.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;depth.FrontFace.StencilFailOp=depth.FrontFace.StencilDepthFailOp=depth.FrontFace.StencilPassOp=depth.BackFace.StencilFailOp=depth.BackFace.StencilDepthFailOp=depth.BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;if(FAILED(hr=device->CreateDepthStencilState(&depth,next.noDepth_.GetAddressOf())))return hr;
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;if(FAILED(hr=device->CreateRasterizerState(&raster,next.raster_.GetAddressOf())))return hr;
 next.timer_.Initialize(device);*this=std::move(next);return S_OK;
}
std::uint64_t GTAOGPU::Bytes()const{
 if(!Available())return 0;std::uint64_t bytes=sizeof(Constants)+std::uint64_t(width_)*height_*17;
 for(UINT mip=0;mip<depthMips_.size();++mip)bytes+=std::uint64_t(std::max(1u,width_>>mip))*std::max(1u,height_>>mip)*4;
 return bytes;
}
HRESULT GTAOGPU::Apply(ID3D11DeviceContext *context,const ScreenMaterialGPU &materials,ID3D11ShaderResourceView *depth,ID3D11ShaderResourceView *color){
 valid_=colorOwned_=false;if(!context||!Available())return E_INVALIDARG;Ptr<ID3D11Device> owner;context->GetDevice(owner.GetAddressOf());if(owner.Get()!=device_.Get())return E_INVALIDARG;
 Constants data;if(!materials.CameraReady()||!PrepareConstants(materials.camera,width_,height_,radius_,strength_,data)){++stats.unsupported;return S_FALSE;}
 data.contact[0]=contactStrength_;
 auto *normal=materials.View(ScreenMaterialGPU::NormalOcclusion),*diffuse=materials.View(ScreenMaterialGPU::DiffuseIndirect),*specular=materials.View(ScreenMaterialGPU::SpecularIndirect),*sun=materials.View(ScreenMaterialGPU::DirectionalDirect);
 constexpr auto materialFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
 if(!TextureFits(depth,device_.Get(),width_,height_,DXGI_FORMAT_R24_UNORM_X8_TYPELESS)||!TextureFits(color,device_.Get(),width_,height_,materialFormat)||!TextureFits(normal,device_.Get(),width_,height_,materialFormat)||!TextureFits(diffuse,device_.Get(),width_,height_,materialFormat)||!TextureFits(specular,device_.Get(),width_,height_,materialFormat)||!TextureFits(sun,device_.Get(),width_,height_,materialFormat))return E_INVALIDARG;
 const auto start=std::chrono::steady_clock::now();SavedGTAOState saved(context);timer_.Poll(context,[&](double ms){stats.gpuMs+=ms;++stats.gpuSamples;});timer_.Begin(context);
 auto *cb=constants_.Get();context->CSSetConstantBuffers(13,1,&cb);auto *sampler=sampler_.Get();context->CSSetSamplers(12,1,&sampler);
 context->CSSetShaderResources(37,1,&depth);context->CSSetShaderResources(41,1,&normal);
 UINT previousWidth=width_,previousHeight=height_;
 if(strength_>0){
 for(UINT mip=0;mip<depthMips_.size();++mip){
  const UINT w=std::max(1u,width_>>mip),h=std::max(1u,height_>>mip);data.filterDimensions[0]=previousWidth;data.filterDimensions[1]=previousHeight;data.filterDimensions[2]=w;data.filterDimensions[3]=h;
  context->UpdateSubresource(constants_.Get(),0,nullptr,&data,0,0);ID3D11ShaderResourceView *empty=nullptr;context->CSSetShaderResources(45,1,&empty);ID3D11UnorderedAccessView *none=nullptr;context->CSSetUnorderedAccessViews(0,1,&none,nullptr);
  if(mip){auto *source=depthMips_[mip-1].Get();context->CSSetShaderResources(45,1,&source);}auto *output=depthOutputs_[mip].Get();context->CSSetUnorderedAccessViews(0,1,&output,nullptr);context->CSSetShader(mip?reduceCS_.Get():depthCS_.Get(),nullptr,0);context->Dispatch((w+7)/8,(h+7)/8,1);++stats.dispatches;previousWidth=w;previousHeight=h;
 }
 ID3D11UnorderedAccessView *none[3]{};context->CSSetUnorderedAccessViews(0,3,none,nullptr);auto *allDepth=depthView_.Get();context->CSSetShaderResources(45,1,&allDepth);auto *ao=aoOutputs_[0].Get(),*edges=edgesOutput_.Get();context->CSSetUnorderedAccessViews(1,1,&ao,nullptr);context->CSSetUnorderedAccessViews(2,1,&edges,nullptr);context->CSSetShader(mainCS_.Get(),nullptr,0);context->Dispatch((width_+7)/8,(height_+7)/8,1);++stats.dispatches;
 for(unsigned pass=0;pass<2;++pass){context->CSSetUnorderedAccessViews(0,3,none,nullptr);ID3D11ShaderResourceView *empty=nullptr;context->CSSetShaderResources(46,1,&empty);auto *input=aoViews_[pass?1:0].Get();auto *edgeInput=edgesView_.Get();context->CSSetShaderResources(46,1,&input);context->CSSetShaderResources(47,1,&edgeInput);auto *output=aoOutputs_[pass?0:1].Get();context->CSSetUnorderedAccessViews(1,1,&output,nullptr);data.tuning[2]=float(pass);context->UpdateSubresource(constants_.Get(),0,nullptr,&data,0,0);context->CSSetShader(denoiseCS_.Get(),nullptr,0);context->Dispatch((width_+15)/16,(height_+7)/8,1);++stats.dispatches;}
 }
 context->UpdateSubresource(constants_.Get(),0,nullptr,&data,0,0);
 saved.Clear();auto *target=colorTarget_.Get();context->OMSetRenderTargets(1,&target,nullptr);context->OMSetDepthStencilState(noDepth_.Get(),0);context->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);context->RSSetState(raster_.Get());D3D11_VIEWPORT viewport{0,0,float(width_),float(height_),0,1};context->RSSetViewports(1,&viewport);
 context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(compositeVS_.Get(),nullptr,0);context->PSSetShader(compositePS_.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
 context->PSSetConstantBuffers(13,1,&cb);context->PSSetShaderResources(37,1,&depth);context->PSSetShaderResources(39,1,&diffuse);context->PSSetShaderResources(40,1,&specular);context->PSSetShaderResources(41,1,&normal);context->PSSetShaderResources(42,1,&sun);context->PSSetShaderResources(44,1,&color);auto *finalAO=strength_>0?aoViews_[0].Get():nullptr;context->PSSetShaderResources(46,1,&finalAO);context->Draw(3,0);
 timer_.End(context);valid_=colorOwned_=true;++stats.frames;if(contactStrength_>0&&data.camera.sunDirection[3]>.5f)++stats.contactFrames;stats.cpuMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return S_OK;
}
}
