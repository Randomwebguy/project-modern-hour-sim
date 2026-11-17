// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainSurface.h"
#include <vector>
class D3D11GroundBlockRegistry {
    std::vector<D3D11GroundBlock> blocks_;
    std::uint64_t revision_=1;
public:
    void Assign(const std::vector<D3D11GroundBlock> &blocks){
        bool equal=blocks.size()==blocks_.size();
        for(std::size_t i=0;equal&&i<blocks.size();++i){const auto &a=blocks[i],&b=blocks_[i];
            equal=a.x==b.x&&a.y==b.y&&a.major==b.major&&a.minor==b.minor&&a.angle==b.angle;
        }
        if(equal)return;blocks_=blocks;++revision_;
    }
    const std::vector<D3D11GroundBlock> &Blocks()const{return blocks_;}
    std::uint64_t Revision()const{return revision_;}
};
