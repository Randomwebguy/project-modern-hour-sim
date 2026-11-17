// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainMaterialLibrary.h"
#include "LoadTelemetry.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace GenZH::Modern {
namespace {
bool SafeLayerId(const std::string &id){
    if(id.empty()||id.size()>64)return false;
    return std::all_of(id.begin(),id.end(),[](char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-';});
}
}

bool ParseTerrainLibraryManifest(std::istream &in,TerrainLibraryManifest &out,std::string &error){
    out={};std::string line;bool header=false;
    struct ClassLine {std::uint32_t ground,primary,partner;float tint;};std::vector<ClassLine> classes;
    struct ReliefLine {std::uint32_t ground;float amplitude,macroTile,tessellation;};std::vector<ReliefLine> reliefs;
    auto fail=[&](const char *why){out={};error=why;return false;};
    while(std::getline(in,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty())continue;
        std::istringstream words(line);std::string kind;words>>kind;
        if(!header){int version=0;if(kind!="terrain-library"||!(words>>version)||version!=1)return fail("terrain library: bad header");header=true;continue;}
        if(kind=="size"){std::uint32_t size=0;if(!(words>>size)||size<64||size>4096||(size&(size-1)))return fail("terrain library: bad size");out.size=size;}
        else if(kind=="layer"){
            std::uint32_t index=0;TerrainLibraryLayer layer;
            if(!(words>>index>>layer.id>>layer.tile>>layer.mean[0]>>layer.mean[1]>>layer.mean[2]))return fail("terrain library: bad layer line");
            if(index!=out.layers.size()||index>=TerrainLibraryMaxLayers)return fail("terrain library: layers must be contiguous and at most 16");
            if(!SafeLayerId(layer.id))return fail("terrain library: bad layer id");
            if(!std::isfinite(layer.tile)||layer.tile<1||layer.tile>1000)return fail("terrain library: bad tile size");
            for(float m:layer.mean)if(!std::isfinite(m)||m<=0||m>1)return fail("terrain library: bad mean albedo");
            out.layers.push_back(std::move(layer));
        }
        else if(kind=="class"){
            ClassLine c{};if(!(words>>c.ground>>c.primary>>c.partner>>c.tint))return fail("terrain library: bad class line");
            if(c.ground<1||c.ground>=out.classes.size()||!std::isfinite(c.tint)||c.tint<0||c.tint>1)return fail("terrain library: bad class");
            classes.push_back(c);
        }
        else if(kind=="relief"){
            ReliefLine c{};if(!(words>>c.ground>>c.amplitude>>c.macroTile>>c.tessellation))return fail("terrain library: bad relief line");
            if(c.ground<1||c.ground>=out.classes.size()||!std::isfinite(c.amplitude)||c.amplitude<0||c.amplitude>2.4f||
               !std::isfinite(c.macroTile)||c.macroTile<10||c.macroTile>1000||!std::isfinite(c.tessellation)||c.tessellation<0||c.tessellation>1)return fail("terrain library: bad relief");
            reliefs.push_back(c);
        }
        else return fail("terrain library: unknown line");
        std::string extra;if(words>>extra)return fail("terrain library: trailing words");
    }
    if(!header||!out.size||out.layers.empty())return fail("terrain library: incomplete manifest");
    for(const auto &c:classes){
        if(c.primary>=out.layers.size()||c.partner>=out.layers.size()||out.classes[c.ground].enabled)return fail("terrain library: bad class layer");
        out.classes[c.ground]={c.primary,c.partner,c.tint,true};
    }
    std::array<bool,16> reliefSeen{};
    for(const auto &c:reliefs){
        if(!out.classes[c.ground].enabled||reliefSeen[c.ground])return fail("terrain library: relief needs one enabled class");
        reliefSeen[c.ground]=true;auto &k=out.classes[c.ground];k.relief=c.amplitude;k.macroTile=c.macroTile;k.tessellation=c.tessellation;
    }
    error.clear();return true;
}

std::array<float,TerrainLibraryTableEntries*4> TerrainLibraryTable(const TerrainLibraryManifest &manifest){
    std::array<float,TerrainLibraryTableEntries*4> table{};
    for(std::size_t i=0;i<manifest.layers.size()&&i<TerrainLibraryMaxLayers;++i){
        const auto &layer=manifest.layers[i];table[i*4]=layer.tile;
        for(std::size_t c=0;c<3;++c)table[i*4+1+c]=layer.mean[c];
    }
    for(std::size_t g=0;g<manifest.classes.size();++g){
        const auto &c=manifest.classes[g];const auto at=(16+g)*4;
        table[at]=static_cast<float>(c.primary);table[at+1]=static_cast<float>(c.partner);table[at+2]=c.tint;table[at+3]=c.enabled?1.0f:0.0f;
        const auto relief=(48+g)*4;table[relief]=c.relief;table[relief+1]=c.macroTile;table[relief+2]=c.tessellation;
    }
    return table;
}

void TerrainMaterialLibrary::Start(const std::filesystem::path &folder){
    if(started_&&folder==folder_)return;
    Stop();folder_=folder;cancel_=false;started_=true;startedAt_=GenZHLoad::Seconds();
    try{worker_=std::thread([this,folder]{Run(folder);});}
    catch(...){Fail("terrain library: worker start failed");}
}

void TerrainMaterialLibrary::Run(std::filesystem::path folder){
    auto publishError=[&](const std::string &why){std::lock_guard<std::mutex> lock(mutex_);workerFailed_=true;workerError_=why;};
    try{
        TerrainLibraryManifest manifest;std::string error;
        {std::ifstream file(folder/L"manifest.txt",std::ios::binary);
         if(!file){publishError("terrain library: manifest missing");return;}
         if(!ParseTerrainLibraryManifest(file,manifest,error)){publishError(error);return;}}
        {std::lock_guard<std::mutex> lock(mutex_);manifest_=manifest;manifestReady_=true;}
        for(std::uint32_t i=0;i<manifest.layers.size();++i){
            {std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return cancel_.load()||decoded_.size()<2;});}
            if(cancel_)return;
            Decoded item;item.layer=i;const auto &id=manifest.layers[i].id;
            if(!LoadTGA(folder/(id+"_albedo_height.tga"),item.albedo,error)||!LoadTGA(folder/(id+"_surface.tga"),item.surface,error)){publishError("terrain library: "+id+": "+error);return;}
            for(const auto *image:{&item.albedo,&item.surface})
                if(image->width!=manifest.size||image->height!=manifest.size||image->pixels.size()!=static_cast<std::size_t>(manifest.size)*manifest.size*4){publishError("terrain library: "+id+": size mismatch");return;}
            std::lock_guard<std::mutex> lock(mutex_);decoded_.push_back(std::move(item));
        }
    }catch(const std::exception &e){publishError(std::string("terrain library: ")+e.what());}
    catch(...){publishError("terrain library: decode exception");}
}

void TerrainMaterialLibrary::Fail(const std::string &why){
    failed_=true;GenZHLoad::Record("terrain_library_v96",why+" (native atlas kept)",0);
}

HRESULT TerrainMaterialLibrary::Create(ID3D11Device *device){
    const auto layers=static_cast<UINT>(manifest_.layers.size());HRESULT hr=S_OK;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=manifest_.size;desc.MipLevels=0;desc.ArraySize=layers;desc.SampleDesc.Count=1;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // sRGB albedo; height in linear alpha.
    if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,albedo_.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(albedo_.Get(),nullptr,albedoView_.GetAddressOf())))return hr;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; // normal XY (OpenGL), AO, roughness.
    if(FAILED(hr=device->CreateTexture2D(&desc,nullptr,surface_.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(surface_.Get(),nullptr,surfaceView_.GetAddressOf())))return hr;
    const auto values=TerrainLibraryTable(manifest_);
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(values);buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    buffer.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;buffer.StructureByteStride=16;
    D3D11_SUBRESOURCE_DATA initial{values.data(),0,0};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_UNKNOWN;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=TerrainLibraryTableEntries;
    if(FAILED(hr=device->CreateBuffer(&buffer,&initial,table_.GetAddressOf()))||FAILED(hr=device->CreateShaderResourceView(table_.Get(),&view,tableView_.GetAddressOf())))return hr;
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_ANISOTROPIC;sampler.MaxAnisotropy=8;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(hr=device->CreateSamplerState(&sampler,sampler_.GetAddressOf())))return hr;
    created_=true;return S_OK;
}

HRESULT TerrainMaterialLibrary::Pump(ID3D11Device *device,ID3D11DeviceContext *context,std::size_t byteBudget){
    if(!started_||ready_||failed_)return S_OK;
    if(!device||!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return E_INVALIDARG;
    std::size_t used=0;
    for(;;){
        Decoded item;
        {std::lock_guard<std::mutex> lock(mutex_);
         if(workerFailed_){const auto why=workerError_;Fail(why);return S_OK;}
         if(!manifestReady_)return S_OK;
         if(!created_){HRESULT hr=Create(device);if(FAILED(hr)){albedo_.Reset();surface_.Reset();albedoView_.Reset();surfaceView_.Reset();table_.Reset();tableView_.Reset();sampler_.Reset();created_=false;Fail("terrain library: GPU allocation failed");return S_OK;}}
         if(decoded_.empty())break;
         item=std::move(decoded_.front());decoded_.pop_front();}
        wake_.notify_one();
        D3D11_TEXTURE2D_DESC desc{};albedo_->GetDesc(&desc);
        const UINT subresource=D3D11CalcSubresource(0,item.layer,desc.MipLevels),pitch=manifest_.size*4;
        context->UpdateSubresource(albedo_.Get(),subresource,nullptr,item.albedo.pixels.data(),pitch,0);
        context->UpdateSubresource(surface_.Get(),subresource,nullptr,item.surface.pixels.data(),pitch,0);
        const auto bytes=item.albedo.pixels.size()+item.surface.pixels.size();used+=bytes;uploaded_+=bytes;++received_;
        if(used>=byteBudget)break;
    }
    if(received_<manifest_.layers.size())return S_OK;
    context->GenerateMips(albedoView_.Get());context->GenerateMips(surfaceView_.Get());
    if(worker_.joinable())worker_.join();
    ready_=true;++revision_;
    std::ostringstream detail;detail<<"layers="<<manifest_.layers.size()<<" size="<<manifest_.size<<" classes=";
    for(std::size_t g=0;g<manifest_.classes.size();++g)if(manifest_.classes[g].enabled)detail<<g<<':'<<manifest_.layers[manifest_.classes[g].primary].id<<'+'<<manifest_.layers[manifest_.classes[g].partner].id<<' ';
    GenZHLoad::Record("terrain_library_v96",detail.str(),(GenZHLoad::Seconds()-startedAt_)*1000,uploaded_);
    return S_OK;
}

void TerrainMaterialLibrary::Bind(ID3D11DeviceContext *context,bool pixel)const{
    ID3D11ShaderResourceView *views[]={albedoView_.Get(),surfaceView_.Get(),tableView_.Get()};
    ID3D11SamplerState *sampler=sampler_.Get();
    if(pixel){context->PSSetShaderResources(AlbedoSlot,3,views);context->PSSetSamplers(SamplerSlot,1,&sampler);}
    context->HSSetShaderResources(AlbedoSlot,3,views);context->HSSetSamplers(SamplerSlot,1,&sampler);
    context->DSSetShaderResources(AlbedoSlot,3,views);context->DSSetSamplers(SamplerSlot,1,&sampler);
}

void TerrainMaterialLibrary::BindCompute(ID3D11DeviceContext *context)const{
    ID3D11ShaderResourceView *views[]={albedoView_.Get(),surfaceView_.Get(),tableView_.Get()};
    ID3D11SamplerState *sampler=sampler_.Get();
    context->CSSetShaderResources(AlbedoSlot,3,views);context->CSSetSamplers(SamplerSlot,1,&sampler);
}

void TerrainMaterialLibrary::Stop(){
    cancel_=true;wake_.notify_all();
    if(worker_.joinable())worker_.join();
    {std::lock_guard<std::mutex> lock(mutex_);decoded_.clear();manifestReady_=workerFailed_=false;workerError_.clear();}
    albedo_.Reset();surface_.Reset();albedoView_.Reset();surfaceView_.Reset();table_.Reset();tableView_.Reset();sampler_.Reset();
    manifest_={};received_=0;uploaded_=0;started_=created_=ready_=failed_=false;folder_.clear();
}
}
