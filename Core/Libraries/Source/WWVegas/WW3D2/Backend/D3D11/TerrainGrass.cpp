// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainGrass.h"
#include "CompatibilityRenderer.h"
#include "LegacyShaderSource.h"
#include "ProgramSource.h"
#include "ShaderBytecodeCache.h"
#include "LoadTelemetry.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace GenZH::Modern {
namespace {
using Microsoft::WRL::ComPtr;
struct GrassConstants {float grid[4],counts[4],wind[4],shape[4];};
static_assert(sizeof(GrassConstants)==64);
struct InteractionConstants {float window[4],center[4],vehicles[TerrainGrassGPU::MaxVehicles][4];};
static_assert(sizeof(InteractionConstants)==32+16*TerrainGrassGPU::MaxVehicles);
}

bool TerrainGrassGPU::VisibleGrid(const LegacyConstants &c,const TerrainGrassInputs &inputs,const TerrainGrassSettings &s,
                                  std::array<int,2> &first,std::array<std::uint32_t,2> &count){
    using namespace DirectX;
    XMFLOAT4X4 view,projection;std::memcpy(&view,c.view,64);std::memcpy(&projection,c.projection,64);
    XMVECTOR determinant;const XMMATRIX inverse=XMMatrixInverse(&determinant,XMMatrixMultiply(XMLoadFloat4x4(&view),XMLoadFloat4x4(&projection)));
    if(!std::isfinite(XMVectorGetX(determinant))||std::abs(XMVectorGetX(determinant))<1e-30f)return false;
    const float camera[3]={c.cameraWorld[0],c.cameraWorld[1],c.cameraWorld[2]};
    float minX=camera[0],maxX=camera[0],minY=camera[1],maxY=camera[1];bool any=false;
    // Rays through the screen border meet the ground between its native height
    // range (plus relief); rays above the horizon end at the grass distance.
    for(int i=0;i<16;++i){
        const float edge=(i%4)/2.0f-1,x=i<4?edge:i<8?1.0f:i<12?-edge:-1.0f,y=i<4?-1.0f:i<8?edge:i<12?1.0f:-edge;
        const XMVECTOR nearPoint=XMVector3TransformCoord(XMVectorSet(x,y,0,1),inverse),farPoint=XMVector3TransformCoord(XMVectorSet(x,y,1,1),inverse);
        XMFLOAT3 o,f;XMStoreFloat3(&o,nearPoint);XMStoreFloat3(&f,farPoint);
        const float d[3]={f.x-o.x,f.y-o.y,f.z-o.z};
        for(float plane:{inputs.minHeight-2,inputs.maxHeight+2}){
            float t=std::abs(d[2])>1e-6f?(plane-o.z)/d[2]:-1;
            float px,py;
            if(t>=0&&t<=1){px=o.x+d[0]*t;py=o.y+d[1]*t;}
            else{const float horizontal=std::sqrt(d[0]*d[0]+d[1]*d[1]);if(horizontal<1e-6f)continue;px=o.x+d[0]/horizontal*s.maxDistance;py=o.y+d[1]/horizontal*s.maxDistance;}
            minX=std::min(minX,px);maxX=std::max(maxX,px);minY=std::min(minY,py);maxY=std::max(maxY,py);any=true;
        }
    }
    if(!any)return false;
    const float margin=4;
    minX=std::max(minX-margin,camera[0]-s.maxDistance);maxX=std::min(maxX+margin,camera[0]+s.maxDistance);
    minY=std::max(minY-margin,camera[1]-s.maxDistance);maxY=std::min(maxY+margin,camera[1]+s.maxDistance);
    const float border=inputs.dimensions[2],size=inputs.dimensions[3];
    minX=std::max(minX,-border);minY=std::max(minY,-border);
    maxX=std::min(maxX,inputs.dimensions[0]*size-border);maxY=std::min(maxY,inputs.dimensions[1]*size-border);
    if(!(maxX>minX)||!(maxY>minY))return false;
    first={static_cast<int>(std::floor(minX/s.cellSize)),static_cast<int>(std::floor(minY/s.cellSize))};
    const double cellsX=std::ceil(maxX/s.cellSize)-first[0],cellsY=std::ceil(maxY/s.cellSize)-first[1];
    if(cellsX<=0||cellsY<=0||cellsX*cellsY>16.0e6)return false;
    count={static_cast<std::uint32_t>(cellsX),static_cast<std::uint32_t>(cellsY)};
    return true;
}

HRESULT TerrainGrassGPU::Create(ID3D11Device *device){
    std::array<D3DRESOURCETYPE,8> types{};types[0]=types[1]=D3DRTYPE_TEXTURE;std::string error;
    const auto source=BuildProgramSource(LegacyShaderSource,false,false,{},{},types,error);if(source.empty())return E_FAIL;
    ComPtr<ID3DBlob> cs,vs;bool hit=false;HRESULT hr=S_OK;
    if(FAILED(hr=CompileCachedShader(source,"GrassCS","cs_5_0",cs,error,hit))){stats_.lastError="GrassCS: "+error;return hr;}
    if(FAILED(hr=CompileCachedShader(source,"GrassVS","vs_5_0",vs,error,hit))){stats_.lastError="GrassVS: "+error;return hr;}
    ComPtr<ID3DBlob> interact;
    if(FAILED(hr=CompileCachedShader(source,"GrassInteractCS","cs_5_0",interact,error,hit))){stats_.lastError="GrassInteractCS: "+error;return hr;}
    if(FAILED(hr=device->CreateComputeShader(interact->GetBufferPointer(),interact->GetBufferSize(),nullptr,interactCompute_.GetAddressOf())))return hr;
    {D3D11_TEXTURE2D_DESC map{};map.Width=map.Height=std::clamp<std::uint32_t>(settings.interactionSize,64,2048);map.MipLevels=map.ArraySize=map.SampleDesc.Count=1;
     map.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;map.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
     if(FAILED(hr=device->CreateTexture2D(&map,nullptr,interaction_.GetAddressOf()))||FAILED(hr=device->CreateUnorderedAccessView(interaction_.Get(),nullptr,interactionUAV_.GetAddressOf()))||
        FAILED(hr=device->CreateShaderResourceView(interaction_.Get(),nullptr,interactionSRV_.GetAddressOf())))return hr;}
    if(FAILED(hr=device->CreateComputeShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,compute_.GetAddressOf()))||
       FAILED(hr=device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex_.GetAddressOf())))return hr;
    capacity_=std::clamp<std::uint32_t>(settings.capacity,1024,2000000);
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=capacity_*80;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=80;
    if(FAILED(hr=device->CreateBuffer(&desc,nullptr,blades_.GetAddressOf())))return hr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_UNKNOWN;uav.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=capacity_;uav.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_APPEND;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_UNKNOWN;srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;srv.Buffer.NumElements=capacity_;
    if(FAILED(hr=device->CreateUnorderedAccessView(blades_.Get(),&uav,bladesUAV_.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(blades_.Get(),&srv,bladesSRV_.GetAddressOf())))return hr;
    const UINT arguments[4]{25,0,0,0};D3D11_BUFFER_DESC args{};args.ByteWidth=sizeof(arguments);args.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    D3D11_SUBRESOURCE_DATA init{arguments,0,0};if(FAILED(hr=device->CreateBuffer(&args,&init,arguments_.GetAddressOf())))return hr;
    auto constant=[&](UINT bytes,ComPtr<ID3D11Buffer> &out){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;return device->CreateBuffer(&d,nullptr,out.GetAddressOf());};
    if(FAILED(hr=constant(sizeof(LegacyConstants),legacy_))||FAILED(hr=constant(sizeof(GrassConstants),grass_))||FAILED(hr=constant(16,map_))||
       FAILED(hr=constant(sizeof(InteractionConstants),interactionConstants_)))return hr;
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    return device->CreateRasterizerState(&raster,raster_.GetAddressOf());
}

HRESULT TerrainGrassGPU::Draw(ID3D11Device *device,ID3D11DeviceContext *context,const LegacyConstants &c,const TerrainGrassInputs &inputs,
                              ID3D11PixelShader *pixel,float seconds){
    if(failed_)return S_FALSE;
    if(!device||!context||!pixel||!inputs.cells||!inputs.identity||!inputs.elevation||!inputs.flips||!inputs.atlas)return E_INVALIDARG;
    HRESULT hr=S_OK;
    if(!compute_){
        if(FAILED(hr=Create(device))){failed_=true;++stats_.failures;Reset();failed_=true;
            GenZHLoad::Record("terrain_grass_v97","GPU grass unavailable, legacy blades kept: "+stats_.lastError,0);return S_FALSE;}
    }
    std::array<int,2> first{};std::array<std::uint32_t,2> count{};
    if(!VisibleGrid(c,inputs,settings,first,count))return S_FALSE;
    GrassConstants g{{static_cast<float>(first[0]),static_cast<float>(first[1]),settings.cellSize,settings.maxDistance},
                     {static_cast<float>(count[0]),static_cast<float>(count[1]),seconds,static_cast<float>(capacity_)},
                     {settings.windX,settings.windY,settings.sway,settings.gust},
                     {settings.bladeHeight,settings.bladeWidth,settings.fullDensityDistance,settings.pixelWidth}};
    context->UpdateSubresource(legacy_.Get(),0,nullptr,&c,0,0);context->UpdateSubresource(grass_.Get(),0,nullptr,&g,0,0);
    context->UpdateSubresource(map_.Get(),0,nullptr,inputs.dimensions.data(),0,0);
    // terrain98: interaction map around the visible ground; tracks recover over
    // settings.recoverySeconds (frame time measured here, paused frames included).
    {const float dt=lastSeconds_<0?0:std::clamp(seconds-lastSeconds_,0.0f,.25f);lastSeconds_=seconds;
     D3D11_TEXTURE2D_DESC map{};interaction_->GetDesc(&map);
     windowCenter_[0]=(first[0]+count[0]*.5f)*settings.cellSize;windowCenter_[1]=(first[1]+count[1]*.5f)*settings.cellSize;
     InteractionConstants k{};k.window[0]=settings.interactionCell;k.window[1]=static_cast<float>(map.Width);
     k.window[2]=std::exp(-dt/std::max(settings.recoverySeconds,.1f));k.window[3]=static_cast<float>(vehicles_.size());
     k.center[0]=windowCenter_[0];k.center[1]=windowCenter_[1];
     for(std::size_t i=0;i<vehicles_.size();++i)std::copy_n(vehicles_[i].data(),4,k.vehicles[i]);
     context->UpdateSubresource(interactionConstants_.Get(),0,nullptr,&k,0,0);
     ID3D11Buffer *interactionBuffers[]={map_.Get(),interactionConstants_.Get()};
     context->CSSetShader(interactCompute_.Get(),nullptr,0);context->CSSetConstantBuffers(3,1,&interactionBuffers[0]);context->CSSetConstantBuffers(InteractionConstantSlot,1,&interactionBuffers[1]);
     ID3D11ShaderResourceView *heights[]={inputs.elevation,inputs.flips};context->CSSetShaderResources(ElevationSlot,2,heights);
     ID3D11UnorderedAccessView *map1=interactionUAV_.Get();context->CSSetUnorderedAccessViews(1,1,&map1,nullptr);
     context->Dispatch((map.Width+7)/8,(map.Height+7)/8,1);
     ID3D11UnorderedAccessView *none=nullptr;context->CSSetUnorderedAccessViews(1,1,&none,nullptr);}
    // Placement and culling.
    ID3D11Buffer *buffers[]={legacy_.Get(),map_.Get(),grass_.Get()};
    context->CSSetShader(compute_.Get(),nullptr,0);
    context->CSSetConstantBuffers(0,1,&buffers[0]);context->CSSetConstantBuffers(3,1,&buffers[1]);context->CSSetConstantBuffers(ConstantSlot,1,&buffers[2]);
    // perf116: GrassCS reads the interaction map and constants for the per-tuft gust and push.
    ID3D11Buffer *interactionCBCompute=interactionConstants_.Get();context->CSSetConstantBuffers(InteractionConstantSlot,1,&interactionCBCompute);
    ID3D11ShaderResourceView *interactionViewCompute=interactionSRV_.Get();context->CSSetShaderResources(InteractionSlot,1,&interactionViewCompute);
    ID3D11ShaderResourceView *terrain[]={inputs.height,inputs.identity,inputs.cells,inputs.exclusion};context->CSSetShaderResources(11,4,terrain);
    ID3D11ShaderResourceView *elevation[]={inputs.elevation,inputs.flips,nullptr,inputs.atlas};context->CSSetShaderResources(ElevationSlot,4,elevation);
    {ID3D11ShaderResourceView *classes=inputs.classes;context->CSSetShaderResources(ClassesSlot,1,&classes);}
    if(inputs.library&&inputs.library->Ready())inputs.library->BindCompute(context);
    else{ID3D11ShaderResourceView *none[3]{};context->CSSetShaderResources(TerrainMaterialLibrary::AlbedoSlot,3,none);}
    ID3D11SamplerState *sampler=inputs.terrainSampler;context->CSSetSamplers(9,1,&sampler);
    const UINT zero=0;ID3D11UnorderedAccessView *uav=bladesUAV_.Get();context->CSSetUnorderedAccessViews(0,1,&uav,&zero);
    context->Dispatch((count[0]+7)/8,(count[1]+7)/8,1);
    ID3D11UnorderedAccessView *noUAV=nullptr;context->CSSetUnorderedAccessViews(0,1,&noUAV,nullptr);
    ID3D11ShaderResourceView *noViews[7]{};context->CSSetShaderResources(11,4,noViews);context->CSSetShaderResources(ClassesSlot,1,noViews);context->CSSetShaderResources(TerrainMaterialLibrary::AlbedoSlot,7,noViews);
    context->CSSetShader(nullptr,nullptr,0);
    ID3D11ShaderResourceView *unbindInteraction=nullptr;context->CSSetShaderResources(InteractionSlot,1,&unbindInteraction);
    ID3D11Buffer *unbindInteractionCB=nullptr;context->CSSetConstantBuffers(InteractionConstantSlot,1,&unbindInteractionCB);
    context->CopyStructureCount(arguments_.Get(),4,bladesUAV_.Get());
    // Expansion through the terrain program's pixel shader; every touched
    // pipeline binding is restored for the scene's binding caches.
    ComPtr<ID3D11VertexShader> oldVS;ComPtr<ID3D11HullShader> oldHS;ComPtr<ID3D11DomainShader> oldDS;ComPtr<ID3D11GeometryShader> oldGS;ComPtr<ID3D11PixelShader> oldPS;
    ComPtr<ID3D11InputLayout> oldLayout;ComPtr<ID3D11RasterizerState> oldRaster;ComPtr<ID3D11Buffer> oldB0,oldB9,oldB10;ComPtr<ID3D11ShaderResourceView> oldBlades,oldInteraction;D3D11_PRIMITIVE_TOPOLOGY oldTopology{};
    context->VSGetShader(oldVS.GetAddressOf(),nullptr,nullptr);context->HSGetShader(oldHS.GetAddressOf(),nullptr,nullptr);context->DSGetShader(oldDS.GetAddressOf(),nullptr,nullptr);
    context->GSGetShader(oldGS.GetAddressOf(),nullptr,nullptr);context->PSGetShader(oldPS.GetAddressOf(),nullptr,nullptr);context->IAGetInputLayout(oldLayout.GetAddressOf());
    context->IAGetPrimitiveTopology(&oldTopology);context->RSGetState(oldRaster.GetAddressOf());context->VSGetConstantBuffers(0,1,oldB0.GetAddressOf());
    context->VSGetConstantBuffers(ConstantSlot,1,oldB9.GetAddressOf());context->VSGetShaderResources(BladeSlot,1,oldBlades.GetAddressOf());
    context->VSGetConstantBuffers(InteractionConstantSlot,1,oldB10.GetAddressOf());context->VSGetShaderResources(InteractionSlot,1,oldInteraction.GetAddressOf());
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(vertex_.Get(),nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->GSSetShader(nullptr,nullptr,0);
    context->PSSetShader(pixel,nullptr,0);context->RSSetState(raster_.Get());
    context->VSSetConstantBuffers(0,1,&buffers[0]);context->VSSetConstantBuffers(ConstantSlot,1,&buffers[2]);
    ID3D11ShaderResourceView *blades=bladesSRV_.Get();context->VSSetShaderResources(BladeSlot,1,&blades);
    ID3D11Buffer *interactionCB=interactionConstants_.Get();context->VSSetConstantBuffers(InteractionConstantSlot,1,&interactionCB);
    ID3D11ShaderResourceView *interactionView=interactionSRV_.Get();context->VSSetShaderResources(InteractionSlot,1,&interactionView);
    if(passTimer)passTimer->Mark(context,GpuPhase::GrassDraw);
    context->DrawInstancedIndirect(arguments_.Get(),0);haveBlades_=true;
    ID3D11ShaderResourceView *restoreInteraction=oldInteraction.Get();context->VSSetShaderResources(InteractionSlot,1,&restoreInteraction);
    ID3D11Buffer *restoreB10=oldB10.Get();context->VSSetConstantBuffers(InteractionConstantSlot,1,&restoreB10);
    ID3D11ShaderResourceView *restoreBlades=oldBlades.Get();context->VSSetShaderResources(BladeSlot,1,&restoreBlades);
    ID3D11Buffer *restoreB0=oldB0.Get(),*restoreB9=oldB9.Get();context->VSSetConstantBuffers(0,1,&restoreB0);context->VSSetConstantBuffers(ConstantSlot,1,&restoreB9);
    context->VSSetShader(oldVS.Get(),nullptr,0);context->HSSetShader(oldHS.Get(),nullptr,0);context->DSSetShader(oldDS.Get(),nullptr,0);context->GSSetShader(oldGS.Get(),nullptr,0);
    context->PSSetShader(oldPS.Get(),nullptr,0);context->IASetInputLayout(oldLayout.Get());context->IASetPrimitiveTopology(oldTopology);context->RSSetState(oldRaster.Get());
    ++stats_.frames;stats_.sites+=std::uint64_t(count[0])*count[1];stats_.lastGridX=count[0];stats_.lastGridY=count[1];
    return S_OK;
}

void TerrainGrassGPU::BindPixel(ID3D11DeviceContext *context)const{
    ID3D11Buffer *buffers[]={grass_.Get(),interactionConstants_.Get()};
    context->PSSetConstantBuffers(ConstantSlot,1,&buffers[0]);context->PSSetConstantBuffers(InteractionConstantSlot,1,&buffers[1]);
    ID3D11ShaderResourceView *view=interactionSRV_.Get();context->PSSetShaderResources(InteractionSlot,1,&view);
}

HRESULT TerrainGrassGPU::ReadInteraction(ID3D11Device *device,ID3D11DeviceContext *context,float x,float y,std::array<float,4> &value){
    value={};if(!interaction_)return E_FAIL;D3D11_TEXTURE2D_DESC desc{};interaction_->GetDesc(&desc);
    const int size=static_cast<int>(desc.Width),cx=static_cast<int>(std::floor(x/settings.interactionCell)),cy=static_cast<int>(std::floor(y/settings.interactionCell));
    const UINT tx=static_cast<UINT>(((cx%size)+size)%size),ty=static_cast<UINT>(((cy%size)+size)%size);
    D3D11_TEXTURE2D_DESC staging=desc;staging.Width=staging.Height=1;staging.BindFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> texel;HRESULT hr=device->CreateTexture2D(&staging,nullptr,texel.GetAddressOf());if(FAILED(hr))return hr;
    D3D11_BOX box{tx,ty,0,tx+1,ty+1,1};context->CopySubresourceRegion(texel.Get(),0,0,0,0,interaction_.Get(),0,&box);
    D3D11_MAPPED_SUBRESOURCE map{};if(FAILED(hr=context->Map(texel.Get(),0,D3D11_MAP_READ,0,&map)))return hr;std::memcpy(value.data(),map.pData,16);context->Unmap(texel.Get(),0);return S_OK;
}

HRESULT TerrainGrassGPU::ReadBlades(ID3D11Device *device,ID3D11DeviceContext *context,std::vector<std::array<float,8>> &blades){
    blades.clear();if(!blades_||!arguments_)return E_FAIL;
    auto read=[&](ID3D11Buffer *source,UINT bytes,std::vector<std::uint8_t> &out){
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> staging;
        HRESULT hr=device->CreateBuffer(&desc,nullptr,staging.GetAddressOf());if(FAILED(hr))return hr;
        D3D11_BOX box{0,0,0,bytes,1,1};context->CopySubresourceRegion(staging.Get(),0,0,0,0,source,0,&box);
        D3D11_MAPPED_SUBRESOURCE map{};if(FAILED(hr=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return hr;
        out.assign(static_cast<const std::uint8_t*>(map.pData),static_cast<const std::uint8_t*>(map.pData)+bytes);context->Unmap(staging.Get(),0);return S_OK;};
    std::vector<std::uint8_t> args,data;HRESULT hr=read(arguments_.Get(),16,args);if(FAILED(hr))return hr;
    std::uint32_t count=0;std::memcpy(&count,args.data()+4,4);count=std::min(count,capacity_);if(!count)return S_OK;
    if(FAILED(hr=read(blades_.Get(),count*80,data)))return hr;
    blades.resize(count);for(std::uint32_t i=0;i<count;++i){const auto *p=data.data()+i*80;std::array<float,8> b{};std::memcpy(b.data(),p,32);blades[i]=b;}
    return S_OK;
}

void TerrainGrassGPU::Reset(){
    compute_.Reset();vertex_.Reset();blades_.Reset();arguments_.Reset();legacy_.Reset();grass_.Reset();map_.Reset();
    bladesUAV_.Reset();bladesSRV_.Reset();raster_.Reset();capacity_=0;failed_=false;
    interactCompute_.Reset();interaction_.Reset();interactionUAV_.Reset();interactionSRV_.Reset();interactionConstants_.Reset();
    lastSeconds_=-1;haveBlades_=false;
}
}
