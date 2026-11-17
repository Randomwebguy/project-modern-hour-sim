// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LegacyDraw.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <limits>
#include <algorithm>
#include <cstring>
#include <cstddef>

namespace GenZH::Modern {
// perf87: a captured sorted-buffer snapshot that no packet references any more
// lends its CPU storage to the next revision of the same buffer key instead of
// being freed and a new one allocated and zero-filled. Same key = same element
// count, so capacities and budget charges are unchanged; the GPU copy is
// released exactly as before. GENZH_SNAPSHOT_RECYCLE=off restores allocation.
inline std::atomic<bool> capturedSnapshotRecycle{true};
static_assert(offsetof(LegacyVertex,registers)==sizeof(FixedVertex));
struct SharedNativeVertices {
    std::vector<FixedVertex> vertices;
    std::vector<TwoUVFixedVertex> twoUVVertices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    void Recycle(){vertices.clear();twoUVVertices.clear();buffer.Reset();}
    bool TwoUV()const{return !twoUVVertices.empty();}
    std::size_t Count()const{return TwoUV()?twoUVVertices.size():vertices.size();}
    UINT Stride()const{return TwoUV()?sizeof(TwoUVFixedVertex):sizeof(FixedVertex);}
    const void *Data()const{return TwoUV()?static_cast<const void*>(twoUVVertices.data()):static_cast<const void*>(vertices.data());}
    const float *Position(std::size_t i)const{return TwoUV()?twoUVVertices[i].position:vertices[i].position;}
    std::size_t Bytes()const{return vertices.capacity()*sizeof(FixedVertex)+twoUVVertices.capacity()*sizeof(TwoUVFixedVertex);}
};
struct SharedBufferedIndices {
    std::vector<std::uint32_t> indices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    void Recycle(){indices.clear();buffer.Reset();}
    std::size_t Bytes()const{return indices.capacity()*sizeof(std::uint32_t);}
};
struct DecodedGeometry {
    std::shared_ptr<SharedNativeVertices> sharedVertices;
    std::shared_ptr<SharedBufferedIndices> sharedIndices;
    std::uint32_t sharedIndexFirst=0;
    std::int32_t sharedIndexBase=0;
    std::vector<LegacyVertex> vertices;
    std::vector<FixedVertex> fixedVertices;
    std::vector<std::uint32_t> indices;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer,indexBuffer;
    float low[3]{},high[3]{};
    bool boundsReady=false;
    std::uint32_t nativeFirst=0,nativeRange=0;
    void CalculateBounds() {
        if(boundsReady||vertices.empty())return;
        for(unsigned axis=0;axis<3;++axis) {
            low[axis]=std::numeric_limits<float>::max();high[axis]=-low[axis];
            for(const auto &v:vertices) {low[axis]=std::min(low[axis],v.position[axis]);high[axis]=std::max(high[axis],v.position[axis]);}
        }
        boundsReady=true;
    }
    void Compact(){
        if(vertices.empty())return;
        CalculateBounds();fixedVertices.resize(vertices.size());
        for(std::size_t i=0;i<vertices.size();++i)std::memcpy(&fixedVertices[i],&vertices[i],sizeof(FixedVertex));
        std::vector<LegacyVertex>().swap(vertices);
    }
    bool CompactVertices()const{return sharedVertices||!fixedVertices.empty();}
    bool TwoUV()const{return sharedVertices&&sharedVertices->TwoUV();}
    UINT Stride()const{return sharedVertices?sharedVertices->Stride():(CompactVertices()?sizeof(FixedVertex):sizeof(LegacyVertex));}
    const void *VertexData()const{return sharedVertices?sharedVertices->Data():(CompactVertices()?static_cast<const void*>(fixedVertices.data()):static_cast<const void*>(vertices.data()));}
    const FixedVertex *FixedData()const{return sharedVertices?sharedVertices->vertices.data():fixedVertices.data();}
    Microsoft::WRL::ComPtr<ID3D11Buffer> &VertexBuffer(){return sharedVertices?sharedVertices->buffer:vertexBuffer;}
    Microsoft::WRL::ComPtr<ID3D11Buffer> &IndexBuffer(){return sharedIndices?sharedIndices->buffer:indexBuffer;}
    UINT IndexFirst()const{return sharedIndices?sharedIndexFirst:0;}
    INT IndexBase()const{return sharedIndices?sharedIndexBase:0;}
    std::size_t VertexCount()const{return sharedVertices?sharedVertices->Count():(fixedVertices.empty()?vertices.size():fixedVertices.size());}
    const float *Position(std::size_t i)const{return sharedVertices?sharedVertices->Position(i):(fixedVertices.empty()?vertices[i].position:fixedVertices[i].position);}
    std::size_t OwnBytes()const{return vertices.capacity()*sizeof(LegacyVertex)+fixedVertices.capacity()*sizeof(FixedVertex)+indices.capacity()*sizeof(std::uint32_t);}
    std::size_t Bytes() const {return OwnBytes()+(sharedVertices?sharedVertices->Bytes():0)+(sharedIndices?sharedIndices->Bytes():0);}
};
// Buffer identity + successful-unlock revision + exact decode range/layout.
// World matrices, lights, texture state and shaders remain per draw packet.
using GeometryKey=std::array<std::uint64_t,12>;
class GeometryCache {
    struct Entry {
        std::shared_ptr<DecodedGeometry> geometry;
        std::weak_ptr<void> vertexOwner,indexOwner;
        std::list<GeometryKey>::iterator recent;
        std::size_t bytes;
        const std::uint64_t *vertexRevision=nullptr,*indexRevision=nullptr;
    };
    struct SharedCharge {std::size_t references=0,bytes=0;};
    std::map<const SharedNativeVertices*,SharedCharge> sharedCharges_;
    std::map<GeometryKey,Entry> entries_;
    std::list<GeometryKey> recent_;
    // Charge both the CPU copy and the immutable GPU copy to this budget.
    std::size_t limit_,bytes_=0;
    std::uint64_t evictions_=0;
    std::uint64_t expired_=0,obsolete_=0,oversized_=0,pressureSkipped_=0;
    unsigned evictionAllowance_=8;
    void Erase(std::map<GeometryKey,Entry>::iterator it) {
        bytes_-=it->second.bytes;
        if(auto *shared=it->second.geometry->sharedVertices.get()){
            auto charge=sharedCharges_.find(shared);
            if(charge!=sharedCharges_.end()&&!--charge->second.references){bytes_-=charge->second.bytes;sharedCharges_.erase(charge);}
        }
        recent_.erase(it->second.recent);entries_.erase(it);
    }
    bool Invalid(std::map<GeometryKey,Entry>::iterator it) {
        // Lock before dereferencing revision fields inside their owners.
        const auto vertex=it->second.vertexOwner.lock(),index=it->second.indexOwner.lock();
        if(!vertex||!index){++expired_;return true;}
        const auto &e=it->second;
        if((e.vertexRevision&&*e.vertexRevision!=it->first[1])||(e.indexRevision&&*e.indexRevision!=it->first[3])){++obsolete_;return true;}
        return false;
    }
public:
    explicit GeometryCache(std::size_t limit=256u*1024u*1024u):limit_(limit){}
    std::shared_ptr<DecodedGeometry> Find(const GeometryKey &key) {
        auto it=entries_.find(key);if(it==entries_.end())return {};
        if(Invalid(it)){Erase(it);return {};}
        recent_.splice(recent_.begin(),recent_,it->second.recent);return it->second.geometry;
    }
    void Insert(const GeometryKey &key,const std::shared_ptr<DecodedGeometry> &geometry,
                const std::shared_ptr<void> &vertexOwner,const std::shared_ptr<void> &indexOwner,
                const std::uint64_t *vertexRevision=nullptr,const std::uint64_t *indexRevision=nullptr) {
        // Captured dynamic buffer snapshots belong to their revision pool,
        // not the immutable mesh-registration cache or its ownership budget.
        if(!geometry||geometry->sharedIndices||!vertexOwner||!indexOwner)return;
        if(geometry->Bytes()>limit_/2){++oversized_;return;}
        if(auto it=entries_.find(key);it!=entries_.end())Erase(it);
        const auto bytes=geometry->OwnBytes()*2;
        const auto *shared=geometry->sharedVertices.get();
        auto required=[&](){return bytes+(shared&&sharedCharges_.find(shared)==sharedCharges_.end()?shared->Bytes()*2:0);};
        // A budget drop or very large insertion must not destroy hundreds of
        // resources during one draw. Uncached packets still render in full.
        while(!recent_.empty()&&bytes_>limit_-required()&&evictionAllowance_){Erase(entries_.find(recent_.back()));++evictions_;--evictionAllowance_;}
        if(bytes_>limit_-required()){++pressureSkipped_;return;}
        if(shared){auto &charge=sharedCharges_[shared];if(!charge.references){charge.bytes=shared->Bytes()*2;bytes_+=charge.bytes;}++charge.references;}
        recent_.push_front(key);entries_.emplace(key,Entry{geometry,vertexOwner,indexOwner,recent_.begin(),bytes,vertexRevision,indexRevision});bytes_+=bytes;
    }
    void PruneExpired() {
        for(auto it=entries_.begin();it!=entries_.end();) {
            if(Invalid(it)){auto old=it++;Erase(old);}else ++it;
        }
    }
    void Clear(){entries_.clear();sharedCharges_.clear();recent_.clear();bytes_=0;}
    std::size_t Bytes()const{return bytes_;}
    std::size_t Size()const{return entries_.size();}
    std::uint64_t Evictions()const{return evictions_;}
    void SetLimit(std::size_t limit){limit_=limit;}
    void BeginSceneBudget(){evictionAllowance_=8;Trim();}
    void Trim(unsigned maximum=8){
        while(!recent_.empty()&&bytes_>limit_&&maximum&&evictionAllowance_){Erase(entries_.find(recent_.back()));++evictions_;--maximum;--evictionAllowance_;}
    }
    std::size_t Limit()const{return limit_;}
    std::uint64_t Expired()const{return expired_;}
    std::uint64_t Obsolete()const{return obsolete_;}
    std::uint64_t Oversized()const{return oversized_;}
    std::uint64_t PressureSkipped()const{return pressureSkipped_;}
};
}
