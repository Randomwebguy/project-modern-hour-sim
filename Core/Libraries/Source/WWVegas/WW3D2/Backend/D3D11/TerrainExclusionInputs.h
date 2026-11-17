// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GeometryCache.h"
#include <array>
#include <vector>
#include <cstring>
#include <cmath>
namespace GenZH::Modern {
struct BufferData;
struct GroundRoadInput {
    GeometryKey identity{};
    std::weak_ptr<void> vertex,index;
    std::array<float,16> world{};
    std::uint32_t primitive=0,sourcePrimitive=0;
    bool indexed=false;
    bool Live()const{
        if(vertex.expired()||(indexed&&index.expired()))return false;
        for(float value:world)if(!std::isfinite(value))return false;
        return true;
    }
};
class TerrainExclusionInputs {
    std::vector<GroundRoadInput> roads_;
    std::array<float,4> dimensions_{};
    std::uint64_t blocks_=0;
    bool valid_=false;
    std::weak_ptr<void> mapOwner_;
    std::uint64_t mapRevision_=0;
public:
    void Reset(){roads_.clear();valid_=false;mapOwner_.reset();mapRevision_=0;}
    bool MapMatches(const std::shared_ptr<void> &owner,std::uint64_t revision)const{
        return owner&&mapOwner_.lock()==owner&&mapRevision_==revision;
    }
    void CommitMap(const std::shared_ptr<void> &owner,std::uint64_t revision){mapOwner_=owner;mapRevision_=revision;}
    bool Matches(std::uint64_t blocks,const std::array<float,4> &dimensions,const std::vector<GroundRoadInput> &roads)const{
        if(!valid_||blocks!=blocks_||dimensions!=dimensions_||roads.size()!=roads_.size())return false;
        for(std::size_t i=0;i<roads.size();++i){const auto &a=roads[i],&b=roads_[i];
            // No pointer-only reuse: source lifetime and captured revision/range
            // both matter. UP draws have no owners and always rebuild.
            if(!a.Live()||!b.Live()||a.identity!=b.identity||a.primitive!=b.primitive||a.sourcePrimitive!=b.sourcePrimitive||a.indexed!=b.indexed||
               std::memcmp(a.world.data(),b.world.data(),sizeof(a.world)))return false;
        }return true;
    }
    void Commit(std::uint64_t blocks,const std::array<float,4> &dimensions,std::vector<GroundRoadInput> roads){
        blocks_=blocks;dimensions_=dimensions;roads_=std::move(roads);valid_=true;
    }
};
}
