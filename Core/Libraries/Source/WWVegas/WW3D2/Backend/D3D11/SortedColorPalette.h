// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CompatibilityRenderer.h"
#include <cmath>
#include <climits>
#include <cstring>
#include <map>
namespace GenZH::Modern {
inline thread_local bool sortedColorPaletteReference=false;
inline thread_local bool sortedMaterialPaletteReference=false;
struct ScopedSortedMaterialPaletteReference {
    bool previous;
    explicit ScopedSortedMaterialPaletteReference(bool reference):previous(sortedMaterialPaletteReference){sortedMaterialPaletteReference=reference;}
    ~ScopedSortedMaterialPaletteReference(){sortedMaterialPaletteReference=previous;}
};
struct ScopedSortedColorPaletteReference {
    bool previous;
    explicit ScopedSortedColorPaletteReference(bool reference):previous(sortedColorPaletteReference){sortedColorPaletteReference=reference;}
    ~ScopedSortedColorPaletteReference(){sortedColorPaletteReference=previous;}
};
struct SortedColorPaletteCounts {
    std::uint64_t builds=0,gpuBytes=0,draws=0,savedDraws=0,materialSavedDraws=0,conflicts=0,budgetBypass=0,gpuFailures=0;
};
inline thread_local SortedColorPaletteCounts sortedColorPaletteCounts;
inline bool SortedColorPacket(const DrawPacket &p){
    return p.nativeSource.Valid()&&p.nativeSource.sorted&&!p.nativeSource.skinned&&p.nativeSource.pass!=UINT_MAX&&
        p.geometry&&p.geometry->sharedVertices&&p.geometry->sharedIndices&&!p.Indices().empty()&&p.Indices().size()%3==0&&
        p.geometry->IndexBase()>=0&&std::uint64_t(p.geometry->IndexBase())<p.VertexCount()&&
        std::uint64_t(p.geometry->IndexBase())*p.geometry->Stride()<=UINT_MAX&&
        p.primitive==D3DPT_TRIANGLELIST&&p.sourcePrimitive==D3DPT_TRIANGLELIST&&TwoUVFixedLayout(p.fvf,44)&&
        !p.terrain&&!p.outputOverlay&&!p.groundOverlay&&!p.groundExclusionOverlay&&!p.pixelShader&&!p.vertexShader&&
        p.constants.geometryFlags[0]<.5f&&!p.States().render[D3DRS_CLIPPLANEENABLE];
}
// World/normal matrices and the three VS-only material colors are indexed
// per original vertex. Emission, power, roughness, light and PS inputs stay uniform. Dynamic stencil
// reference, all appearance/wear/material maps and every UV transform are live.
// perf89: the state comparison for two packets already known to satisfy
// SortedColorPacket (a pure predicate the caller evaluated once per packet).
inline bool SameSortedColorFields(const DrawPacket &a,const DrawPacket &b){
    if(a.fvf!=b.fvf||
       a.geometry->sharedVertices!=b.geometry->sharedVertices||a.geometry->sharedIndices!=b.geometry->sharedIndices||
       a.geometry->IndexBase()!=b.geometry->IndexBase()||!SameStateArrays(a,b)||
       !BytesEqual(a.constants.view,b.constants.view,128)||
       !BytesEqual(a.constants.textureMatrices,b.constants.textureMatrices,sizeof(a.constants.textureMatrices))||
       (sortedMaterialPaletteReference&&!BytesEqual(a.constants.materialDiffuse,b.constants.materialDiffuse,48))||
       !BytesEqual(a.constants.materialEmissive,b.constants.materialEmissive,sizeof(LegacyConstants)-offsetof(LegacyConstants,materialEmissive)))return false;
    for(unsigned i=0;i<8;++i)if(a.textures[i]!=b.textures[i]||a.capturedTextures[i].Get()!=b.capturedTextures[i].Get())return false;
    for(unsigned i=0;i<2;++i)if(a.materialTextures[i].Get()!=b.materialTextures[i].Get())return false;
    for(unsigned i=0;i<3;++i)if(a.wearTextures[i].Get()!=b.wearTextures[i].Get())return false;
    return a.heightTexture.Get()==b.heightTexture.Get()&&a.emissiveTexture.Get()==b.emissiveTexture.Get();
}
inline bool SameSortedColorState(const DrawPacket &a,const DrawPacket &b){
    return SortedColorPacket(a)&&SortedColorPacket(b)&&SameSortedColorFields(a,b);
}
inline bool ConsecutiveSortedColor(const DrawPacket &first,const DrawPacket &previous,const DrawPacket &next){
    return SameSortedColorState(first,next)&&
        std::uint64_t(previous.geometry->IndexFirst())+previous.Indices().size()==next.geometry->IndexFirst();
}
// perf89: ConsecutiveSortedColor for packets whose SortedColorPacket was already true.
inline bool ConsecutiveSortedColorEligible(const DrawPacket &first,const DrawPacket &previous,const DrawPacket &next){
    return SameSortedColorFields(first,next)&&
        std::uint64_t(previous.geometry->IndexFirst())+previous.Indices().size()==next.geometry->IndexFirst();
}
struct SortedColorPalette {
    struct Matrices{float world[16],normal[16],diffuse[4],ambient[4],specular[4];};
    static_assert(sizeof(Matrices)==176);
    static constexpr std::size_t ByteLimit=16u*1024u*1024u;
    std::shared_ptr<SharedNativeVertices> source;
    std::vector<UINT> selectors;
    std::vector<Matrices> matrices;
    ComPtr<ID3D11ShaderResourceView> matrixView,selectorView;
    bool invalid=false;
    explicit SortedColorPalette(const std::shared_ptr<SharedNativeVertices> &vertices):source(vertices){
        if(!source||!source->Count()||source->Count()>ByteLimit/(2*sizeof(UINT))){invalid=true;++sortedColorPaletteCounts.budgetBypass;return;}
        selectors.assign(source->Count(),UINT_MAX);
    }
    void Add(const DrawPacket &p,std::size_t availableBytes=ByteLimit){
        if(invalid)return;
        Matrices value;std::memcpy(value.world,p.constants.world,64);std::memcpy(value.normal,p.constants.normalWorld,64);
        std::memcpy(value.diffuse,p.constants.materialDiffuse,48);
        for(float x:value.world)if(!std::isfinite(x)){invalid=true;++sortedColorPaletteCounts.conflicts;return;}
        for(float x:value.normal)if(!std::isfinite(x)){invalid=true;++sortedColorPaletteCounts.conflicts;return;}
        for(float x:value.diffuse)if(!std::isfinite(x)){invalid=true;++sortedColorPaletteCounts.conflicts;return;}
        for(float x:value.ambient)if(!std::isfinite(x)){invalid=true;++sortedColorPaletteCounts.conflicts;return;}
        for(float x:value.specular)if(!std::isfinite(x)){invalid=true;++sortedColorPaletteCounts.conflicts;return;}
        UINT selection=UINT_MAX,verified=UINT_MAX;
        for(auto index:p.Indices()){
            if(index>=selectors.size()){invalid=true;++sortedColorPaletteCounts.conflicts;return;}
            const auto old=selectors[index];
            if(old!=UINT_MAX){
                // perf115: a selector already matched this value needs no second compare; a mismatch still rejects the table.
                if(old!=verified){
                    if(std::memcmp(&matrices[old],&value,sizeof(value))){invalid=true;++sortedColorPaletteCounts.conflicts;return;}
                    verified=old;
                }
                if(selection==UINT_MAX)selection=old;
            }
        }
        if(selection==UINT_MAX){
            if(selectors.size()*sizeof(UINT)+(matrices.size()+1)*sizeof(Matrices)>ByteLimit){invalid=true;++sortedColorPaletteCounts.budgetBypass;return;}
            if(matrices.size()==matrices.capacity()){
                const auto capacityLimit=std::min((ByteLimit-selectors.capacity()*sizeof(UINT))/sizeof(Matrices),
                    matrices.capacity()+availableBytes/sizeof(Matrices));
                if(capacityLimit<=matrices.size()){invalid=true;++sortedColorPaletteCounts.budgetBypass;return;}
                matrices.reserve(std::min(capacityLimit,std::max(std::size_t(1),matrices.capacity()*2)));
            }
            selection=static_cast<UINT>(matrices.size());matrices.push_back(value);
        }
        for(auto index:p.Indices())if(selectors[index]==UINT_MAX)selectors[index]=selection;
    }
    bool Create(ID3D11Device *device){
        if(invalid||matrices.empty()||!device)return false;
        if(matrixView&&selectorView)return true;
        const auto create=[&](const void *data,UINT count,UINT stride,ComPtr<ID3D11ShaderResourceView> &view)->HRESULT{
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=count*stride;desc.Usage=D3D11_USAGE_IMMUTABLE;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=stride;
            D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;ComPtr<ID3D11Buffer> buffer;
            auto hr=device->CreateBuffer(&desc,&initial,buffer.GetAddressOf());if(FAILED(hr))return hr;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;srv.Buffer.NumElements=count;
            return device->CreateShaderResourceView(buffer.Get(),&srv,view.GetAddressOf());
        };
        ComPtr<ID3D11ShaderResourceView> nextMatrices,nextSelectors;
        if(FAILED(create(matrices.data(),static_cast<UINT>(matrices.size()),sizeof(Matrices),nextMatrices))||
           FAILED(create(selectors.data(),static_cast<UINT>(selectors.size()),sizeof(UINT),nextSelectors))){
            invalid=true;++sortedColorPaletteCounts.gpuFailures;return false;
        }
        matrixView=std::move(nextMatrices);selectorView=std::move(nextSelectors);
        ++sortedColorPaletteCounts.builds;sortedColorPaletteCounts.gpuBytes+=matrices.size()*sizeof(Matrices)+selectors.size()*sizeof(UINT);
        return true;
    }
    void Bind(ID3D11DeviceContext *context)const{
        ID3D11ShaderResourceView *views[]={matrixView.Get(),selectorView.Get()};context->VSSetShaderResources(120,2,views);
    }
    static void Unbind(ID3D11DeviceContext *context){ID3D11ShaderResourceView *views[]={nullptr,nullptr};context->VSSetShaderResources(120,2,views);}
};
// Owned by one color flush. Original packets and source revision snapshots are
// never modified; shadow fit/casters/culling keep their original inputs. A
// conflicting matrix for one shared vertex rejects that complete source table.
class SortedColorPalettes {
    bool enabled_;
    std::size_t bytes_=0;
    std::map<const SharedNativeVertices*,std::unique_ptr<SortedColorPalette>> entries_;
public:
    explicit SortedColorPalettes(bool enabled):enabled_(enabled&&!sortedColorPaletteReference){}
    // known: the caller has already evaluated SortedColorPacket(p) as true (FlushScene's run heads and members).
    void Add(const DrawPacket &p,bool known=false){
        if(!enabled_||(!known&&!SortedColorPacket(p)))return;
        try{
        auto *source=p.geometry->sharedVertices.get();auto it=entries_.find(source);
        if(it==entries_.end()){
            if(entries_.size()>=32){++sortedColorPaletteCounts.budgetBypass;return;}
            if(source->Count()>(SortedColorPalette::ByteLimit-bytes_)/sizeof(UINT)){
                enabled_=false;++sortedColorPaletteCounts.budgetBypass;return;
            }
            it=entries_.emplace(source,std::make_unique<SortedColorPalette>(p.geometry->sharedVertices)).first;
            bytes_+=it->second->selectors.capacity()*sizeof(UINT);
        }
        const auto oldMatrices=it->second->matrices.capacity();
        it->second->Add(p,SortedColorPalette::ByteLimit-bytes_);
        bytes_+=(it->second->matrices.capacity()-oldMatrices)*sizeof(SortedColorPalette::Matrices);
        if(bytes_>SortedColorPalette::ByteLimit){enabled_=false;++sortedColorPaletteCounts.budgetBypass;}
        }catch(const std::bad_alloc&){enabled_=false;entries_.clear();++sortedColorPaletteCounts.budgetBypass;}
    }
    SortedColorPalette *Find(const DrawPacket &p){
        if(!enabled_||!SortedColorPacket(p))return nullptr;
        auto it=entries_.find(p.geometry->sharedVertices.get());return it==entries_.end()||it->second->invalid?nullptr:it->second.get();
    }
};
}
