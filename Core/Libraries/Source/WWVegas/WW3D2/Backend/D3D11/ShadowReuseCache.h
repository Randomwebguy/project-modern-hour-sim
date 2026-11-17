// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "ByteEquality.h"
namespace GenZH::Modern {
// Exact state, not a probabilistic hash. Weak leases prevent pointer recycling
// from validating stale depth and do not pin geometry/textures in GPU budgets.
struct ShadowDrawStamp {
    std::array<std::uint8_t,3552> bytes{};
    std::array<std::weak_ptr<void>,11> owners;
    std::uint16_t ownerMask=0;
    bool SameOwners(const ShadowDrawStamp &other)const{
        if(ownerMask!=other.ownerMask)return false;
        for(unsigned i=0;i<owners.size();++i)if(ownerMask&(1u<<i)){
            const auto a=owners[i].lock(),b=other.owners[i].lock();
            if(!a||!b||a!=b)return false;
        }
        return true;
    }
    bool operator==(const ShadowDrawStamp &other)const{return BytesEqual(bytes.data(),other.bytes.data(),bytes.size())&&SameOwners(other);}
};
class ShadowReuseCache {
public:
    static constexpr std::size_t MaxDraws=512;
    struct Plan {std::vector<bool> stationary;bool hit=false;std::size_t staticDraws=0;
        std::size_t terrainTracked=0,terrainFirst=0,terrainWorldChanged=0,terrainCameraChanged=0,
            terrainSourceChanged=0,terrainMapChanged=0,terrainShapeChanged=0,terrainOwnerChanged=0;
    };
    Plan Prepare(std::vector<ShadowDrawStamp> current,const float *vp){const auto count=current.size();return Prepare(current,count,vp);}
    // The first `count` stamps of `current` are this frame's state. On return
    // `current` holds the previous frame's stamps for reuse as next frame's
    // scratch (perf86): no per-frame allocation, and stationary stamps are only
    // copied into the cache when a rebuilt layer is committed.
    Plan Prepare(std::vector<ShadowDrawStamp> &current,std::size_t count,const float *vp){
        count=(std::min)({count,current.size(),MaxDraws});
        Plan plan;plan.stationary.resize(count);pending_.clear();
        for(std::size_t i=0;i<count;++i){
            if(current[i].ownerMask&(1u<<10)){
                ++plan.terrainTracked;
                if(i>=previousCount_||!(previous_[i].ownerMask&(1u<<10)))++plan.terrainFirst;
                else{
                    // Canonical perf69 stamp: world, view/projection/viewport,
                    // grass eligibility, full source revision, immutable maps,
                    // occupancy revision, topology. Diagnostics never relax it.
                    const auto *a=current[i].bytes.data(),*b=previous_[i].bytes.data();
                    plan.terrainWorldChanged+=std::memcmp(a,b,64)!=0;
                    plan.terrainCameraChanged+=std::memcmp(a+64,b+64,144)!=0;
                    plan.terrainShapeChanged+=std::memcmp(a+208,b+208,8)!=0||std::memcmp(a+360,b+360,12)!=0;
                    plan.terrainSourceChanged+=std::memcmp(a+216,b+216,96)!=0;
                    plan.terrainMapChanged+=std::memcmp(a+312,b+312,48)!=0;
                    plan.terrainOwnerChanged+=!current[i].SameOwners(previous_[i]);
                }
            }
            if(i<previousCount_&&current[i]==previous_[i]){plan.stationary[i]=true;pending_.push_back(i);}
        }
        plan.staticDraws=pending_.size();
        plan.hit=valid_&&!std::memcmp(matrix_.data(),vp,64)&&pending_.size()==cached_.size();
        if(plan.hit)for(std::size_t i=0;i<pending_.size();++i)if(!(current[pending_[i]]==cached_[i])){plan.hit=false;break;}
        std::memcpy(pendingMatrix_.data(),vp,64);previous_.swap(current);previousCount_=count;return plan;
    }
    // Commit only after the complete static depth layer was successfully drawn.
    void Commit(){
        cached_.clear();cached_.reserve(pending_.size());
        for(const auto index:pending_)cached_.push_back(previous_[index]);
        pending_.clear();matrix_=pendingMatrix_;valid_=true;
    }
    void DiscardPending(){pending_.clear();}
    void Reset(){std::vector<ShadowDrawStamp>().swap(previous_);std::vector<ShadowDrawStamp>().swap(cached_);std::vector<std::size_t>().swap(pending_);previousCount_=0;valid_=false;}
    std::size_t StateBytes()const{return (previous_.capacity()+cached_.capacity())*sizeof(ShadowDrawStamp)+pending_.capacity()*sizeof(std::size_t);}
private:
    std::vector<ShadowDrawStamp> previous_,cached_;
    std::vector<std::size_t> pending_; // indices of stationary stamps in previous_
    std::size_t previousCount_=0;
    std::array<float,16> matrix_{},pendingMatrix_{};
    bool valid_=false;
};
}
