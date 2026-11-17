// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
namespace GenZH::Modern {
// Retain array allocations, never MeshModel -> renderer -> buffer cycles.
template<class Array>std::shared_ptr<void> RetainNativeArray(Array *array){
    if(!array)return {};array->Add_Ref();return {array,[](void *p){static_cast<Array*>(p)->Release_Ref();}};
}
using NativeArrayOwners=std::vector<std::shared_ptr<void>>;
struct DeferredLegacyStats {
    std::uint64_t reservedBuffers=0,reservedBytes=0,materializedBuffers=0,materializedBytes=0,packedBytes=0,failedFlushes=0,liveDeferredBuffers=0,liveDeferredBytes=0;
};
inline DeferredLegacyStats deferredLegacyStats;
class DeferredLegacyBuffer {
public:
    using Writer=std::function<bool(std::uint8_t*,std::size_t)>;
private:
    struct Write {std::uint32_t offset,bytes;Writer writer;};
    std::vector<Write> writes_;
    std::uint32_t capacity_=0;bool enabled_=false,materialized_=false;
public:
    DeferredLegacyBuffer()=default;
    DeferredLegacyBuffer(const DeferredLegacyBuffer&)=delete;
    DeferredLegacyBuffer &operator=(const DeferredLegacyBuffer&)=delete;
    ~DeferredLegacyBuffer(){if(enabled_&&!materialized_){--deferredLegacyStats.liveDeferredBuffers;deferredLegacyStats.liveDeferredBytes-=capacity_;}}
    void Reserve(std::uint32_t bytes){
        if(enabled_||!bytes)throw std::invalid_argument("Duplicate or empty legacy reservation");
        capacity_=bytes;enabled_=true;++deferredLegacyStats.reservedBuffers;deferredLegacyStats.reservedBytes+=bytes;
        ++deferredLegacyStats.liveDeferredBuffers;deferredLegacyStats.liveDeferredBytes+=bytes;
    }
    bool Enabled()const{return enabled_;}
    std::size_t Pending()const{return writes_.size();}
    void Add(std::uint32_t offset,std::uint32_t bytes,Writer writer){
        if(!enabled_||!bytes||offset>capacity_||bytes>capacity_-offset||!writer)throw std::invalid_argument("Deferred legacy buffer range");
        writes_.push_back({offset,bytes,std::move(writer)});
    }
    void Created(){if(enabled_&&!materialized_){materialized_=true;++deferredLegacyStats.materializedBuffers;deferredLegacyStats.materializedBytes+=capacity_;--deferredLegacyStats.liveDeferredBuffers;deferredLegacyStats.liveDeferredBytes-=capacity_;}}
    template<class Lock,class Unlock>bool Flush(Lock lock,Unlock unlock){
        // Keep all recipes until a complete successful flush, so retries cannot
        // publish incomplete content. Lock failures never dereference output.
        for(const auto &write:writes_){std::uint8_t *out=nullptr;
            if(!lock(write.offset,write.bytes,&out)){++deferredLegacyStats.failedFlushes;return false;}
            if(!out){unlock();++deferredLegacyStats.failedFlushes;return false;}
            bool filled=false;try{filled=write.writer(out,write.bytes);}catch(...){unlock();++deferredLegacyStats.failedFlushes;throw;}
            const bool released=unlock();if(!filled||!released){++deferredLegacyStats.failedFlushes;return false;}
        }
        for(const auto &write:writes_)deferredLegacyStats.packedBytes+=write.bytes;
        writes_.clear();return true;
    }
};
}
