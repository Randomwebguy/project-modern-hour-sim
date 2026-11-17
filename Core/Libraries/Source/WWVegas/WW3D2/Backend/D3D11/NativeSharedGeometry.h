// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeMeshGeometry.h"
namespace GenZH::Modern {
using NativeVertexKey=std::array<std::uint64_t,24>;
inline NativeVertexKey NativeVerticesKey(const NativeGeometryView &v){
    NativeVertexKey key{v.model,v.fvf,v.vertices,v.recipe->revision,
        reinterpret_cast<std::uintptr_t>(v.positions),reinterpret_cast<std::uintptr_t>(v.normals),
        reinterpret_cast<std::uintptr_t>(v.diffuse),reinterpret_cast<std::uintptr_t>(v.specular)};
    for(unsigned i=0;i<8;++i){key[8+i]=reinterpret_cast<std::uintptr_t>(v.uv[i]);key[16+i]=v.UVStride(i);}return key;
}
class NativeVertexPool {
    std::map<NativeVertexKey,std::weak_ptr<SharedNativeVertices>> vertices_;
public:
    std::shared_ptr<SharedNativeVertices> Find(const NativeVertexKey &key){
        auto it=vertices_.find(key);if(it==vertices_.end())return {};auto result=it->second.lock();
        if(!result)vertices_.erase(it);return result;
    }
    void Remember(const NativeVertexKey &key,const std::shared_ptr<SharedNativeVertices> &value){
        // Metadata only; ownership and CPU/GPU budget stay with GeometryCache.
        if(vertices_.size()<8192)vertices_[key]=value;
    }
    void Prune(){for(auto it=vertices_.begin();it!=vertices_.end();)if(it->second.expired())it=vertices_.erase(it);else ++it;}
    void Clear(){vertices_.clear();}
    std::size_t Size()const{return vertices_.size();}
};
struct NativeVertexStats {std::uint64_t hits=0,misses=0,decodedBytes=0,submeshBytes=0,draws=0,errors=0,budgetBypass=0,localFallback=0;};
inline bool NativeSharedFits(const NativeGeometryView &v,std::size_t limit){
    const auto vertices=std::uint64_t(v.vertices)*sizeof(FixedVertex);
    const auto indices=std::uint64_t(v.recipe->polygons.size())*12;
    return vertices<=UINT32_MAX&&indices<=UINT32_MAX&&vertices<=limit/2&&indices<=limit/2-vertices;
}
inline HRESULT BuildNativeSharedGeometry(const NativeGeometryView &v,NativeVertexPool &pool,DecodedGeometry &out,NativeVertexStats &stats){
    if(!v.Valid()||!v.Current()){++stats.errors;return E_INVALIDARG;}
    if(v.recipe->polygons.size()>UINT32_MAX/3){++stats.errors;return E_INVALIDARG;}
    std::vector<std::uint32_t> indices;indices.reserve(v.recipe->polygons.size()*3);
    for(auto polygon:v.recipe->polygons){const void *array=v.triangles;auto index=polygon;
        if(polygon>=v.triangleCount){index=polygon-v.triangleCount;if(!v.gapTriangles||index>=v.gapCount){++stats.errors;return E_INVALIDARG;}array=v.gapTriangles;}
        std::uint32_t tri[3];std::memcpy(tri,static_cast<const std::uint8_t*>(array)+std::size_t(index)*12,12);
        for(auto vertex:tri){if(vertex<v.first||std::uint64_t(vertex)-v.first>=v.range){++stats.errors;return E_INVALIDARG;}indices.push_back(vertex);}
    }
    const auto key=NativeVerticesKey(v);auto shared=pool.Find(key);
    if(shared)++stats.hits;
    else {
        auto full=v;full.first=0;full.range=v.vertices;DecodedGeometry decoded;
        HRESULT hr=BuildNativeGeometry(full,decoded);if(FAILED(hr)){++stats.errors;return hr;}
        shared=std::make_shared<SharedNativeVertices>();shared->vertices=std::move(decoded.fixedVertices);
        stats.decodedBytes+=shared->vertices.size()*sizeof(FixedVertex);++stats.misses;
        pool.Remember(key,shared);
    }
    // Retain the original submesh vertex-range bounds; full shared vertices
    // do not widen visibility, reorder, shadow or terrain-exclusion bounds.
    float low[3]{},high[3]{};
    for(std::uint32_t i=0;i<v.range;++i){const auto *p=shared->vertices[v.first+i].position;
        for(unsigned axis=0;axis<3;++axis)if(!i)low[axis]=high[axis]=p[axis];else {low[axis]=std::min(low[axis],p[axis]);high[axis]=std::max(high[axis],p[axis]);}}
    out.vertexBuffer.Reset();out.indexBuffer.Reset();out.vertices.clear();out.fixedVertices.clear();
    out.sharedIndices.reset();out.sharedIndexFirst=0;out.sharedIndexBase=0;
    out.sharedVertices=std::move(shared);out.indices=std::move(indices);out.nativeFirst=v.first;out.nativeRange=v.range;
    std::memcpy(out.low,low,12);std::memcpy(out.high,high,12);out.boundsReady=true;
    stats.submeshBytes+=std::uint64_t(v.range)*sizeof(FixedVertex);return S_OK;
}
}
