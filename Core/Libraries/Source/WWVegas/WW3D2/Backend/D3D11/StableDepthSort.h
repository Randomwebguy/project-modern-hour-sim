// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from olcayseygan/CnCGeneralsZH-Reforged sortingrenderer.cpp,
// commit 9e8c16a9d71b3c474db0316f9bdd553b0fb83b4d (three 11-bit passes).
// Current uint32 triangle/node indices and native/PBR submission are retained.
#pragma once
#include <array>
#include <vector>
#include <cstring>
#include <cstdint>
#include <new>
namespace GenZH::Modern {
inline bool stableDepthSortReference=false;
inline std::uint64_t nativeDepthRadixBatches=0;
inline std::uint32_t DepthSortKey(float depth){
    std::uint32_t bits;std::memcpy(&bits,&depth,4);
    const auto magnitude=bits&0x7fffffffu;
    if(magnitude>0x7f800000u)return UINT32_MAX; // nonfinite legacy data: deterministic NaNs last
    if(!magnitude)return 0x80000000u; // +0 and -0 compare equal
    return bits&0x80000000u?~bits:bits^0x80000000u;
}
template<class Entry>class StableDepthSort {
    std::vector<Entry> scratch_;
public:
    std::uint64_t batches=0,items=0,bypassed=0;
    Entry *Sort(Entry *data,std::size_t count){
        // Scratch is a bounded work allocation, never a model/index limit.
        if(count>64u*1024u*1024u/sizeof(Entry)){++bypassed;return nullptr;}
        try{scratch_.reserve(count);scratch_.resize(count);}catch(const std::bad_alloc &){++bypassed;return nullptr;}
        ++batches;items+=count;if(count<2)return data;
        constexpr unsigned Bits=11,Buckets=1u<<Bits,Passes=3,Mask=Buckets-1;
        std::array<std::array<std::size_t,Buckets>,Passes> histogram{};
        for(std::size_t i=0;i<count;++i){const auto key=DepthSortKey(data[i].z);
            for(unsigned p=0;p<Passes;++p)++histogram[p][(key>>(p*Bits))&Mask];}
        Entry *from=data,*to=scratch_.data();
        for(unsigned p=0;p<Passes;++p){auto &h=histogram[p];std::size_t offset=0;
            for(auto &bucket:h){const auto n=bucket;bucket=offset;offset+=n;}
            for(std::size_t i=0;i<count;++i){const auto key=DepthSortKey(from[i].z);to[h[(key>>(p*Bits))&Mask]++]=from[i];}
            auto *swap=from;from=to;to=swap;
        }return from;
    }
    std::size_t ScratchBytes()const{return scratch_.capacity()*sizeof(Entry);}
};
}
