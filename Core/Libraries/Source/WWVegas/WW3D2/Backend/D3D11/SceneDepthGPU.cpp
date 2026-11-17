// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneDepthGPU.h"
#include "SceneDepthShaderSource.h"
#include "ShaderBytecodeCache.h"
#include <algorithm>
#include <chrono>
namespace GenZH::Modern {
void SceneDepthGPU::Reset(){*this=SceneDepthGPU{};}
void SceneDepthGPU::Unbind(ID3D11DeviceContext *c){if(c){ID3D11ShaderResourceView *empty[2]{};c->PSSetShaderResources(37,2,empty);c->CSSetShaderResources(37,2,empty);}}
HRESULT SceneDepthGPU::Create(ID3D11Device *device,UINT width,UINT height){
 if(!device||!width||!height||width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)return E_INVALIDARG;
 SceneDepthGPU next;next.device_=device;next.width_=width;next.height_=height;HRESULT hr;
 D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R24G8_TYPELESS;d.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
 D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
 D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
 if(FAILED(hr=device->CreateTexture2D(&d,nullptr,next.depth_.GetAddressOf()))||FAILED(hr=device->CreateDepthStencilView(next.depth_.Get(),&dv,next.writable_.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(next.depth_.Get(),&sv,next.depthView_.GetAddressOf())))return hr;
 dv.Flags=D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL;
 if(FAILED(hr=device->CreateDepthStencilView(next.depth_.Get(),&dv,next.readOnly_.GetAddressOf())))return hr;
 *this=std::move(next);return S_OK;
}
HRESULT SceneDepthGPU::CreateHierarchy(){
 if(!device_||!depth_)return E_UNEXPECTED;
 D3D11_TEXTURE2D_DESC d{};d.Width=(width_+1)/2;d.Height=(height_+1)/2;d.MipLevels=1;
 for(UINT w=d.Width,h=d.Height;w>1||h>1;w=std::max(1u,w/2),h=std::max(1u,h/2))++d.MipLevels;
 d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
 Ptr<ID3D11Texture2D> texture;Ptr<ID3D11ShaderResourceView> all;std::vector<Ptr<ID3D11ShaderResourceView>> views(d.MipLevels);std::vector<Ptr<ID3D11UnorderedAccessView>> outputs(d.MipLevels);HRESULT hr;
 if(FAILED(hr=device_->CreateTexture2D(&d,nullptr,texture.GetAddressOf()))||FAILED(hr=device_->CreateShaderResourceView(texture.Get(),nullptr,all.GetAddressOf())))return hr;
 for(UINT mip=0;mip<d.MipLevels;++mip){
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=d.Format;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MostDetailedMip=mip;sv.Texture2D.MipLevels=1;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=d.Format;uv.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;uv.Texture2D.MipSlice=mip;
  if(FAILED(hr=device_->CreateShaderResourceView(texture.Get(),&sv,views[mip].GetAddressOf()))||FAILED(hr=device_->CreateUnorderedAccessView(texture.Get(),&uv,outputs[mip].GetAddressOf())))return hr;
 }
 Ptr<ID3D11ComputeShader> copy,reduce;Ptr<ID3DBlob> code;bool hit=false;std::string error;
 if(FAILED(hr=CompileCachedShader(SceneDepthShaderSource,"CSSceneDepthCopy","cs_5_0",code,error,hit))||FAILED(hr=device_->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,copy.GetAddressOf())))return hr;
 if(FAILED(hr=CompileCachedShader(SceneDepthShaderSource,"CSSceneDepthReduce","cs_5_0",code,error,hit))||FAILED(hr=device_->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,reduce.GetAddressOf())))return hr;
 D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Ptr<ID3D11Buffer> constants;
 if(FAILED(hr=device_->CreateBuffer(&bd,nullptr,constants.GetAddressOf())))return hr;
 hierarchy_=texture;hierarchyView_=all;mipViews_=std::move(views);mipOutputs_=std::move(outputs);copy_=copy;reduce_=reduce;constants_=constants;timer_.Initialize(device_.Get());return S_OK;
}
std::uint64_t SceneDepthGPU::HierarchyBytes()const{
 if(!hierarchy_)return 0;std::uint64_t bytes=16;
 for(UINT mip=0;mip<Mips();++mip)bytes+=std::uint64_t(std::max(1u,((width_+1)/2)>>mip))*std::max(1u,((height_+1)/2)>>mip)*8;
 return bytes;
}
HRESULT SceneDepthGPU::Build(ID3D11DeviceContext *c){
 valid_=false;if(!c||!depth_)return E_INVALIDARG;
 Ptr<ID3D11Device> owner;c->GetDevice(owner.GetAddressOf());if(owner.Get()!=device_.Get())return E_INVALIDARG;
 if(!hierarchy_){const HRESULT hr=CreateHierarchy();if(FAILED(hr))return hr;}
 const auto start=std::chrono::steady_clock::now();
 Ptr<ID3D11DepthStencilView> oldDepth;ID3D11RenderTargetView *targets[8]{};c->OMGetRenderTargets(8,targets,oldDepth.GetAddressOf());
 Ptr<ID3D11ComputeShader> oldShader;c->CSGetShader(oldShader.GetAddressOf(),nullptr,nullptr);
 Ptr<ID3D11Buffer> oldConstants;c->CSGetConstantBuffers(13,1,oldConstants.GetAddressOf());
 ID3D11ShaderResourceView *oldViews[2]{};c->CSGetShaderResources(37,2,oldViews);
 ID3D11ShaderResourceView *oldPixelViews[2]{};c->PSGetShaderResources(37,2,oldPixelViews);ID3D11ShaderResourceView *empty[2]{};c->PSSetShaderResources(37,2,empty);
 Ptr<ID3D11UnorderedAccessView> oldOutput;c->CSGetUnorderedAccessViews(0,1,oldOutput.GetAddressOf());
 c->OMSetRenderTargets(0,nullptr,nullptr);timer_.Poll(c,[&](double ms){stats.gpuMs+=ms;++stats.gpuSamples;});timer_.Begin(c);
 auto *cb=constants_.Get();c->CSSetConstantBuffers(13,1,&cb);
 UINT sourceWidth=width_,sourceHeight=height_,w=(width_+1)/2,h=(height_+1)/2;
 for(UINT mip=0;mip<Mips();++mip){
  const UINT sizes[4]={sourceWidth,sourceHeight,w,h};c->UpdateSubresource(constants_.Get(),0,nullptr,sizes,0,0);
  c->CSSetShaderResources(37,2,empty);auto *input=mip?mipViews_[mip-1].Get():depthView_.Get();c->CSSetShaderResources(mip?38:37,1,&input);
  auto *output=mipOutputs_[mip].Get();c->CSSetUnorderedAccessViews(0,1,&output,nullptr);c->CSSetShader(mip?reduce_.Get():copy_.Get(),nullptr,0);c->Dispatch((w+7)/8,(h+7)/8,1);
  output=nullptr;c->CSSetUnorderedAccessViews(0,1,&output,nullptr);++stats.dispatches;sourceWidth=w;sourceHeight=h;w=std::max(1u,w/2);h=std::max(1u,h/2);
 }
 timer_.End(c);c->CSSetShaderResources(37,2,empty);auto *output=oldOutput.Get();c->CSSetUnorderedAccessViews(0,1,&output,nullptr);cb=oldConstants.Get();c->CSSetConstantBuffers(13,1,&cb);c->CSSetShader(oldShader.Get(),nullptr,0);
 // Restore depth before the original input views. Original engine callers
 // never bind a writable DSV and its input simultaneously.
 c->OMSetRenderTargets(8,targets,oldDepth.Get());c->CSSetShaderResources(37,2,oldViews);
 c->PSSetShaderResources(37,2,oldPixelViews);for(auto *view:oldPixelViews)if(view)view->Release();
 for(auto *target:targets)if(target)target->Release();for(auto *view:oldViews)if(view)view->Release();
 valid_=true;++stats.builds;stats.cpuMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return S_OK;
}
}
