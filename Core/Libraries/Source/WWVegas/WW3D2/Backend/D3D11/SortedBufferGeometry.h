// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GeometryCache.h"
#include <functional>
#include <climits>
namespace GenZH::Modern {
// Only a successful captured unlock changes the revision. Cache entries own
// immutable CPU/GPU snapshots, never the mutable legacy buffer or its owner.
// Previously submitted packets keep their snapshot after DISCARD/NOOVERWRITE.
template<class Value,std::size_t N> class CapturedBufferPool {
    using Key=std::array<std::uint64_t,N>;
    struct Entry {std::weak_ptr<void> owner;std::shared_ptr<Value> value;std::uint64_t revision,tick;std::size_t bytes;};
    std::map<Key,Entry> entries_;
    std::size_t limit_,bytes_=0;
    std::uint64_t tick_=0;
    void Erase(typename std::map<Key,Entry>::iterator it){bytes_-=it->second.bytes;entries_.erase(it);}
public:
    std::uint64_t hits=0,misses=0,bypassed=0,evicted=0,decodedBytes=0,recycled=0;
    explicit CapturedBufferPool(std::size_t limit):limit_(limit){}
    template<class Build> HRESULT Acquire(const Key &key,std::uint64_t revision,const std::shared_ptr<void> &owner,
                        std::size_t anticipatedBytes,Build build,std::shared_ptr<Value> &out){
        if(!owner)return E_INVALIDARG;
        std::shared_ptr<Value> spare;
        if(auto it=entries_.find(key);it!=entries_.end()){
            auto alive=it->second.owner.lock();
            if(alive&&alive==owner&&it->second.revision==revision){++hits;it->second.tick=++tick_;out=it->second.value;return S_OK;}
            // Packets retain older versions independently; one nothing else holds
            // lends its storage to this key's new snapshot (perf87).
            if(capturedSnapshotRecycle.load(std::memory_order_relaxed)&&it->second.value.use_count()==1)spare=std::move(it->second.value);
            Erase(it);
        }
        // A cache bypass changes resource scheduling, never mesh precision or quality.
        if(!anticipatedBytes||anticipatedBytes>limit_/2){++bypassed;return S_FALSE;}
        while(!entries_.empty()&&(bytes_>limit_-anticipatedBytes*2||entries_.size()>=64)){
            auto oldest=entries_.begin();for(auto it=entries_.begin();it!=entries_.end();++it)if(it->second.tick<oldest->second.tick)oldest=it;
            Erase(oldest);++evicted;
        }
        std::shared_ptr<Value> value;
        if(spare){value=std::move(spare);value->Recycle();++recycled;}else value=std::make_shared<Value>();
        HRESULT hr=build(*value);if(hr!=S_OK)return hr;
        const auto charge=value->Bytes()*2; // CPU snapshot and immutable GPU copy.
        if(charge>limit_||bytes_>limit_-charge){++bypassed;return S_FALSE;}
        decodedBytes+=value->Bytes();++misses;bytes_+=charge;
        entries_.emplace(key,Entry{owner,value,revision,++tick_,charge});out=std::move(value);return S_OK;
    }
    void Prune(){for(auto it=entries_.begin();it!=entries_.end();)if(it->second.owner.expired()){auto old=it++;Erase(old);}else ++it;}
    void Clear(){entries_.clear();bytes_=0;}
    void ClearStats(){hits=misses=bypassed=evicted=decodedBytes=recycled=0;}
    std::size_t Bytes()const{return bytes_;}
    std::size_t Size()const{return entries_.size();}
};
struct SortedBufferView {
    const void *vertexData=nullptr,*indexData=nullptr;
    std::size_t vertexBytes=0,indexBytes=0;
    std::shared_ptr<void> vertexOwner,indexOwner;
    std::uint64_t vertexRevision=0,indexRevision=0;
    UINT stride=0,firstVertex=0,vertexCount=0,firstIndex=0,indexCount=0,base=0;
    DWORD fvf=0;D3DFORMAT indexFormat=D3DFMT_INDEX16;bool compactTwoUV=false;
};
class SortedBufferGeometryPool {
public:
    CapturedBufferPool<SharedNativeVertices,5> vertices;
    CapturedBufferPool<SharedBufferedIndices,3> indices;
    std::uint64_t draws=0,errors=0,twoUVDraws=0,twoUVDecodedBytesSaved=0;
    explicit SortedBufferGeometryPool(std::size_t limit=(sizeof(void*)==8?128u:64u)*1024u*1024u):vertices(limit/2),indices(limit/2){}
    void Prune(){vertices.Prune();indices.Prune();}
    void Clear(){vertices.Clear();indices.Clear();}
    void ClearStats(){vertices.ClearStats();indices.ClearStats();draws=errors=twoUVDraws=twoUVDecodedBytesSaved=0;}
    std::size_t Bytes()const{return vertices.Bytes()+indices.Bytes();}
    std::size_t Size()const{return vertices.Size()+indices.Size();}
};
// donor (perf88): the caller's selected-index vector, moved into the result only
// after every check has passed (same contents and capacity as the copy).
inline HRESULT BuildSortedBufferGeometryImpl(const SortedBufferView &v,const std::vector<std::uint32_t> &selected,std::vector<std::uint32_t> *donor,
                                        SortedBufferGeometryPool &pool,DecodedGeometry &out,std::string &error){
    const auto indexStride=v.indexFormat==D3DFMT_INDEX32?4u:(v.indexFormat==D3DFMT_INDEX16?2u:0u);
    if(!v.vertexData||!v.indexData||!v.vertexOwner||!v.indexOwner||!v.stride||!indexStride||
       !v.vertexCount||!v.indexCount||selected.size()!=v.indexCount||v.vertexBytes%v.stride||v.indexBytes%indexStride||
       v.vertexBytes/v.stride>UINT_MAX||v.indexBytes/indexStride>UINT_MAX||
       std::uint64_t(v.firstVertex)+v.vertexCount>v.vertexBytes/v.stride||
       std::uint64_t(v.firstIndex)+v.indexCount>v.indexBytes/indexStride)return E_INVALIDARG;
    if(v.base>INT_MAX)return S_FALSE; // D3D11's BaseVertexLocation is signed.
    const auto count=static_cast<UINT>(v.vertexBytes/v.stride);
    const bool twoUV=v.compactTwoUV&&TwoUVFixedLayout(v.fvf,v.stride);
    const auto vertexSize=std::uint64_t(count)*(twoUV?sizeof(TwoUVFixedVertex):sizeof(FixedVertex));
    const auto indexSize=std::uint64_t(v.indexBytes/indexStride)*sizeof(std::uint32_t);
    if(vertexSize>UINT_MAX||indexSize>UINT_MAX)return S_FALSE;
    for(auto i:selected)if(i<v.firstVertex||std::uint64_t(i)-v.firstVertex>=v.vertexCount)return E_INVALIDARG;
    std::shared_ptr<SharedNativeVertices> vertices;
    HRESULT hr=pool.vertices.Acquire({reinterpret_cast<std::uintptr_t>(v.vertexOwner.get()),v.fvf,v.stride,v.vertexBytes,twoUV?1u:0u},
        v.vertexRevision,v.vertexOwner,static_cast<std::size_t>(vertexSize),[&](SharedNativeVertices &value){
            if(twoUV){
                const auto decoded=DecodeTwoUVFixedVertices(v.vertexData,v.vertexBytes,v.stride,v.fvf,count,value.twoUVVertices,error);
                if(SUCCEEDED(decoded))pool.twoUVDecodedBytesSaved+=std::uint64_t(count)*(sizeof(FixedVertex)-sizeof(TwoUVFixedVertex));
                return decoded;
            }
            return DecodeFixedVertices(v.vertexData,v.vertexBytes,v.stride,v.fvf,0,count,value.vertices,error);
        },vertices);
    if(hr!=S_OK)return hr;
    std::shared_ptr<SharedBufferedIndices> indices;
    hr=pool.indices.Acquire({reinterpret_cast<std::uintptr_t>(v.indexOwner.get()),static_cast<std::uint64_t>(v.indexFormat),v.indexBytes},
        v.indexRevision,v.indexOwner,static_cast<std::size_t>(indexSize),[&](SharedBufferedIndices &value){
            // Decode raw indices without base. Unused slots are not validated
            // against this draw's vertex range; only selected slots are drawn.
            const auto n=v.indexBytes/indexStride;value.indices.resize(n);
            for(std::size_t i=0;i<n;++i){if(indexStride==4)std::memcpy(&value.indices[i],static_cast<const std::uint8_t*>(v.indexData)+i*4,4);
                else{std::uint16_t x;std::memcpy(&x,static_cast<const std::uint8_t*>(v.indexData)+i*2,2);value.indices[i]=x;}}
            return S_OK;
        },indices);
    if(hr!=S_OK)return hr;
    for(std::size_t i=0;i<selected.size();++i)if(std::uint64_t(indices->indices[v.firstIndex+i])+v.base!=selected[i])return E_INVALIDARG;
    DecodedGeometry next;next.sharedVertices=std::move(vertices);next.sharedIndices=std::move(indices);
    next.sharedIndexFirst=v.firstIndex;next.sharedIndexBase=static_cast<INT>(v.base);
    next.nativeFirst=*std::min_element(selected.begin(),selected.end());
    next.nativeRange=*std::max_element(selected.begin(),selected.end())-next.nativeFirst+1;
    bool first=true;
    for(auto index:selected){const auto *p=next.Position(index);
        for(unsigned axis=0;axis<3;++axis)if(first)next.low[axis]=next.high[axis]=p[axis];else{next.low[axis]=std::min(next.low[axis],p[axis]);next.high[axis]=std::max(next.high[axis],p[axis]);}
        first=false;
    }
    if(donor)next.indices=std::move(*donor);else next.indices=selected;
    next.boundsReady=true;out=std::move(next);++pool.draws;if(twoUV)++pool.twoUVDraws;return S_OK;
}
inline HRESULT BuildSortedBufferGeometry(const SortedBufferView &v,const std::vector<std::uint32_t> &selected,
                                        SortedBufferGeometryPool &pool,DecodedGeometry &out,std::string &error){
    return BuildSortedBufferGeometryImpl(v,selected,nullptr,pool,out,error);
}
// Leaves selected untouched unless the result is S_OK.
inline HRESULT BuildSortedBufferGeometry(const SortedBufferView &v,std::vector<std::uint32_t> &&selected,
                                        SortedBufferGeometryPool &pool,DecodedGeometry &out,std::string &error){
    return BuildSortedBufferGeometryImpl(v,selected,&selected,pool,out,error);
}
}
