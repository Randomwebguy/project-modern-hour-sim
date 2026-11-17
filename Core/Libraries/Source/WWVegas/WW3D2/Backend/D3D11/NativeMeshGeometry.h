// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GeometryCache.h"
#include "NativeSceneIdentity.h"
#include "NativeMeshSubmit.h"
#include <cmath>
namespace GenZH::Modern {
struct NativeGeometryRecipe {
    NativeLifetimeIdentity identity;
    std::uint64_t revision=1;
    // Original native polygon IDs in exactly the material category's order.
    std::vector<std::uint32_t> polygons;
};
struct NativeGeometryView {
    void *modelContext=nullptr;
    bool (*current)(const NativeGeometryView&)=nullptr;
    std::shared_ptr<NativeGeometryRecipe> recipe;
    const void *positions=nullptr,*normals=nullptr,*triangles=nullptr,*gapTriangles=nullptr;
    const std::uint32_t *diffuse=nullptr,*specular=nullptr;
    std::array<const void*,8> uv{};
    // Sorting FVF always carries two UV slots. Preserve missing native slots
    // byte-for-byte from the retained original packed buffer, never infer UVs.
    std::array<std::uint32_t,8> uvStrides{};
    std::shared_ptr<void> uvFallbackOwner;
    std::uint32_t UVStride(unsigned channel)const{return uvStrides[channel]?uvStrides[channel]:8;}
    std::uint64_t model=0;
    std::uint32_t fvf=0,vertices=0,triangleCount=0,gapCount=0;
    std::uint32_t legacyFirstVertex=0,legacyFirstIndex=0,first=0,range=0;
    bool Current()const{return !current||current(*this);}
    bool Valid()const{
        if(!recipe||recipe->polygons.empty()||!model||!positions||!normals||!triangles||!range||
           (fvf&D3DFVF_POSITION_MASK)!=D3DFVF_XYZ||!(fvf&D3DFVF_NORMAL)||std::uint64_t(first)+range>vertices)return false;
        const unsigned channels=(fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
        if(channels>8)return false;
        for(unsigned i=0;i<channels;++i)if(!uv[i]||((fvf>>(16+i*2))&3)||(uvStrides[i]&&(uvStrides[i]<8||!uvFallbackOwner)))return false;
        return true;
    }
    bool Matches(const NativeIndexedDraw &draw,std::uint32_t currentFVF,std::uint64_t currentModel)const{
        return Valid()&&currentFVF==fvf&&currentModel==model&&draw.primitive==D3DPT_TRIANGLELIST&&
            draw.firstVertex==legacyFirstVertex&&draw.firstIndex==legacyFirstIndex&&draw.vertices==range&&
            draw.primitives==recipe->polygons.size();
    }
    GeometryKey Key()const{
        // An explicit namespace tag cannot alias a legacy BufferData address.
        return {0x4e41544956453235ull,recipe->revision,recipe->identity.Get(),recipe->revision,
                model,fvf,first,range,recipe->polygons.size(),legacyFirstIndex,0,D3DPT_TRIANGLELIST};
    }
};
// Borrowed native arrays are used synchronously inside an active mesh draw.
// Deferred packets keep only decoded geometry and recipe ownership, not views.
inline thread_local const NativeGeometryView *currentNativeGeometry=nullptr;
class ScopedNativeGeometry {
    const NativeGeometryView *previous_;
public:
    explicit ScopedNativeGeometry(const NativeGeometryView *view):previous_(currentNativeGeometry){currentNativeGeometry=view;}
    ~ScopedNativeGeometry(){currentNativeGeometry=previous_;}
    ScopedNativeGeometry(const ScopedNativeGeometry&)=delete;
    ScopedNativeGeometry &operator=(const ScopedNativeGeometry&)=delete;
};
template<class Model>NativeGeometryView CaptureNativeGeometry(Model &model,std::uint32_t fvf,
    std::shared_ptr<NativeGeometryRecipe> recipe,std::uint32_t legacyFirst,std::uint32_t legacyIndex,std::uint32_t first,std::uint32_t range){
    static_assert(sizeof(*model.Get_Vertex_Array())==12);
    static_assert(sizeof(*model.Get_Vertex_Normal_Array())==12);
    static_assert(sizeof(*model.Get_Polygon_Array())==12);
    static_assert(sizeof(*model.Get_UV_Array_By_Index(0))==8);
    NativeGeometryView view;view.recipe=std::move(recipe);view.model=model.Get_Native_Geometry_ID();view.fvf=fvf;
    view.vertices=model.Get_Vertex_Count();view.triangleCount=model.Get_Polygon_Count();
    view.positions=model.Get_Vertex_Array();view.normals=model.Get_Vertex_Normal_Array();view.triangles=model.Get_Polygon_Array();
    view.diffuse=model.Get_Color_Array(0,false);view.specular=model.Get_Color_Array(1,false);
    const unsigned channels=(fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
    for(unsigned i=0;i<channels&&i<8;++i)view.uv[i]=model.Get_UV_Array_By_Index(i);
    if(auto *gap=model.Get_Gap_Filler()){view.gapTriangles=gap->Get_Polygon_Array();view.gapCount=gap->Get_Polygon_Count();}
    view.modelContext=&model;
    view.current=[](const NativeGeometryView &value){
        // Only called on a cache miss inside the active native mesh draw.
        // Replaced arrays must not be dereferenced from an old registration.
        auto &live=*static_cast<Model*>(value.modelContext);
        if(live.Get_Native_Geometry_ID()!=value.model||static_cast<std::uint32_t>(live.Get_Vertex_Count())!=value.vertices||
           static_cast<std::uint32_t>(live.Get_Polygon_Count())!=value.triangleCount||live.Get_Vertex_Array()!=value.positions||
           live.Get_Vertex_Normal_Array()!=value.normals||live.Get_Polygon_Array()!=value.triangles||
           live.Get_Color_Array(0,false)!=value.diffuse||live.Get_Color_Array(1,false)!=value.specular)return false;
        const unsigned channels=(value.fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
        for(unsigned i=0;i<channels;++i){
            const auto *nativeUV=i<static_cast<unsigned>(live.Get_UV_Array_Count())?live.Get_UV_Array_By_Index(i):nullptr;
            if(value.uvStrides[i]){if(nativeUV||!value.uvFallbackOwner)return false;}
            else if(nativeUV!=value.uv[i])return false;
        }
        const auto *gap=live.Get_Gap_Filler();
        return gap?(gap->Get_Polygon_Array()==value.gapTriangles&&static_cast<std::uint32_t>(gap->Get_Polygon_Count())==value.gapCount):!value.gapTriangles&&!value.gapCount;
    };
    view.legacyFirstVertex=legacyFirst;view.legacyFirstIndex=legacyIndex;view.first=first;view.range=range;return view;
}
inline void NativeColor(std::uint32_t packed,float *rgba){
    rgba[0]=static_cast<float>((packed>>16)&255)/255;rgba[1]=static_cast<float>((packed>>8)&255)/255;
    rgba[2]=static_cast<float>(packed&255)/255;rgba[3]=static_cast<float>((packed>>24)&255)/255;
}
inline HRESULT BuildNativeGeometry(const NativeGeometryView &view,DecodedGeometry &geometry){
    if(!view.Valid()||!view.Current())return E_INVALIDARG;
    // No 448-byte programmable vertex expansion or second native vertex copy.
    std::vector<FixedVertex> vertices(view.range);std::vector<std::uint32_t> indices;
    if(view.recipe->polygons.size()>UINT32_MAX/3)return E_INVALIDARG;indices.reserve(view.recipe->polygons.size()*3);
    float low[3]{},high[3]{};
    const unsigned channels=(view.fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
    for(std::uint32_t i=0;i<view.range;++i){auto &v=vertices[i];const std::size_t source=std::size_t(view.first)+i;
        v.position[3]=1;v.normal[2]=1;for(auto &component:v.diffuse)component=1;
        std::memcpy(v.position,static_cast<const std::uint8_t*>(view.positions)+source*12,12);
        std::memcpy(v.normal,static_cast<const std::uint8_t*>(view.normals)+source*12,12);
        if(view.fvf&D3DFVF_DIFFUSE)NativeColor(view.diffuse?view.diffuse[source]:0xffffffffu,v.diffuse);
        if(view.fvf&D3DFVF_SPECULAR)NativeColor(view.specular?view.specular[source]:0xffffffffu,v.specular);
        for(unsigned channel=0;channel<channels;++channel)std::memcpy(v.uv[channel],static_cast<const std::uint8_t*>(view.uv[channel])+source*view.UVStride(channel),8);
        for(unsigned axis=0;axis<3;++axis){const auto value=v.position[axis];if(!std::isfinite(value))return E_INVALIDARG;
            if(!i)low[axis]=high[axis]=value;else {low[axis]=std::min(low[axis],value);high[axis]=std::max(high[axis],value);}}
    }
    for(auto polygon:view.recipe->polygons){const void *array=view.triangles;std::uint32_t index=polygon;
        if(polygon>=view.triangleCount){index=polygon-view.triangleCount;if(!view.gapTriangles||index>=view.gapCount)return E_INVALIDARG;array=view.gapTriangles;}
        std::uint32_t triangle[3];std::memcpy(triangle,static_cast<const std::uint8_t*>(array)+std::size_t(index)*12,12);
        for(auto vertex:triangle){if(vertex<view.first||std::uint64_t(vertex)-view.first>=view.range)return E_INVALIDARG;indices.push_back(vertex-view.first);}
    }
    // Publish only a complete checked immutable resource.
    geometry.vertexBuffer.Reset();geometry.indexBuffer.Reset();
    geometry.sharedVertices.reset();geometry.sharedIndices.reset();geometry.sharedIndexFirst=0;geometry.sharedIndexBase=0;geometry.nativeFirst=geometry.nativeRange=0;
    geometry.fixedVertices=std::move(vertices);geometry.indices=std::move(indices);geometry.vertices.clear();
    std::memcpy(geometry.low,low,12);std::memcpy(geometry.high,high,12);geometry.boundsReady=true;return S_OK;
}
}
