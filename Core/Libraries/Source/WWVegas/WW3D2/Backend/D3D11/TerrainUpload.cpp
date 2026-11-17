// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainUpload.h"
#include "LoadTelemetry.h"
#include <algorithm>
namespace GenZH::Modern {
HRESULT TerrainUpload::Pump(ID3D11Device *device,ID3D11DeviceContext *context,std::size_t byteBudget,double milliseconds){
 if(!device||!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!data_||!data_->width||!data_->height||data_->width>4096||data_->height>4096||(data_->cells.empty()&&stage_!=Stage::Done&&stage_!=Stage::CellBytes)||byteBudget<static_cast<std::size_t>(data_->width)*4||milliseconds<=0)return E_INVALIDARG;
 if(Ready())return S_OK;const auto started=GenZHLoad::Seconds();std::size_t used=0;
 while(!Ready()&&(GenZHLoad::Seconds()-started)*1000<milliseconds){
  HRESULT hr=S_OK;
  if(stage_==Stage::CreateSurface){
   if(data_->pixels[surface_].size()!=static_cast<std::size_t>(data_->width)*data_->height*4)return E_INVALIDARG;
   D3D11_TEXTURE2D_DESC desc{};desc.Width=data_->width;desc.Height=data_->height;desc.MipLevels=surface_==4?1:0;desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
   desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(surface_==4?0:D3D11_BIND_RENDER_TARGET);desc.MiscFlags=surface_==4?0:D3D11_RESOURCE_MISC_GENERATE_MIPS;
   if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,textures_[surface_].GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(textures_[surface_].Get(),nullptr,views_[surface_].GetAddressOf())))return hr;
   stage_=Stage::Rows;row_=0;return S_OK; // At most one resource allocation per pump.
  }
  if(stage_==Stage::Rows){
   const UINT pitch=data_->width*4;const auto rows=std::min<std::size_t>(data_->height-row_,(byteBudget-used)/pitch);if(!rows)return S_OK;
   D3D11_BOX box{0,row_,0,data_->width,row_+static_cast<UINT>(rows),1};const auto bytes=rows*pitch;
   context->UpdateSubresource(textures_[surface_].Get(),0,&box,data_->pixels[surface_].data()+static_cast<std::size_t>(row_)*pitch,pitch,0);
   used+=bytes;uploaded_+=bytes;row_+=static_cast<UINT>(rows);if(row_==data_->height){std::vector<std::uint8_t>().swap(data_->pixels[surface_]);stage_=Stage::Mips;}
   if(used==byteBudget)return S_OK;continue;
  }
  if(stage_==Stage::Mips){
   if(surface_!=4)context->GenerateMips(views_[surface_].Get());++surface_;stage_=surface_==5?Stage::CreateCells:Stage::CreateSurface;return S_OK;
  }
  if(stage_==Stage::CreateCells){
   const auto bytes=data_->cells.size()*sizeof(D3D11TerrainCell);if(bytes>80ull*1024*1024)return E_INVALIDARG;
   D3D11_BUFFER_DESC desc{};desc.ByteWidth=static_cast<UINT>(bytes);desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=sizeof(D3D11TerrainCell);
   if(FAILED(hr=device->CreateBuffer(&desc,nullptr,cellsBuffer_.GetAddressOf())))return hr;
   D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_UNKNOWN;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=static_cast<UINT>(data_->cells.size());
   if(FAILED(hr=device->CreateShaderResourceView(cellsBuffer_.Get(),&view,cellsView_.GetAddressOf())))return hr;
   // terrain101: the per-cell class records (16 bytes per cell) in one immutable upload.
   if(data_->classes.size()==data_->cells.size()){
    D3D11_BUFFER_DESC classes{};classes.ByteWidth=static_cast<UINT>(data_->classes.size()*16);classes.Usage=D3D11_USAGE_IMMUTABLE;classes.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{data_->classes.data(),0,0};
    if(FAILED(hr=device->CreateBuffer(&classes,&initial,classesBuffer_.GetAddressOf())))return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC typed{};typed.Format=DXGI_FORMAT_R32G32B32A32_UINT;typed.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;typed.Buffer.NumElements=static_cast<UINT>(data_->classes.size());
    if(FAILED(hr=device->CreateShaderResourceView(classesBuffer_.Get(),&typed,classesView_.GetAddressOf())))return hr;
    uploaded_+=data_->classes.size()*16;std::vector<std::array<std::uint32_t,4>>().swap(data_->classes);
   }
   stage_=Stage::CellBytes;return S_OK;
  }
  if(stage_==Stage::CellBytes){
   const auto size=data_->cells.size()*sizeof(D3D11TerrainCell);const auto bytes=std::min(size-cellOffset_,(byteBudget-used)/80*80);if(!bytes)return S_OK;
   D3D11_BOX box{static_cast<UINT>(cellOffset_),0,0,static_cast<UINT>(cellOffset_+bytes),1,1};
   context->UpdateSubresource(cellsBuffer_.Get(),0,&box,reinterpret_cast<const unsigned char*>(data_->cells.data())+cellOffset_,0,0);
   used+=bytes;uploaded_+=bytes;cellOffset_+=bytes;if(cellOffset_==size){std::vector<D3D11TerrainCell>().swap(data_->cells);stage_=Stage::Done;}if(used==byteBudget)return S_OK;
  }
 }
 return S_OK;
}
}
