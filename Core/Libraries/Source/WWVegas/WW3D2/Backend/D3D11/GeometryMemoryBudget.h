// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <algorithm>
namespace GenZH::Modern {
struct GeometryMemorySample {
    std::uint64_t videoBudget=0,videoUsage=0,physicalAvailable=0,virtualAvailable=0;
    bool videoKnown=false,cpuKnown=false;
};
// The cache charges CPU + immutable GPU copies. This is a cache-owned budget;
// native resources and in-flight packets are also covered by OS headroom.
inline std::uint64_t GeometryMemoryTarget(const GeometryMemorySample &s,std::uint64_t ownedBytes,unsigned pointerBits){
    constexpr std::uint64_t MiB=1024ull*1024;
    const auto half=ownedBytes/2;
    const std::uint64_t ceiling=(pointerBits==64?1024ull:512ull)*MiB;
    auto reclaim=[](std::uint64_t owned,std::uint64_t available,std::uint64_t reserve){
        if(available>=reserve)return owned+(available-reserve)/4;
        const auto deficit=reserve-available;return owned>deficit?owned-deficit:0;
    };
    std::uint64_t cpu=128*MiB,gpu=128*MiB;
    if(s.cpuKnown)cpu=std::min({ceiling,reclaim(half,s.physicalAvailable,512*MiB),
        reclaim(half,s.virtualAvailable,(pointerBits==64?512ull:1024ull)*MiB)});
    if(s.videoKnown){
        // Don't grow into all remaining VRAM. Retain a budget share for terrain,
        // textures, shadows, driver resources and other render work.
        auto room=half+s.videoBudget;
        room=room>s.videoUsage?room-s.videoUsage:0;
        const auto reserve=std::max(64*MiB,s.videoBudget/16);
        room=room>reserve?room-reserve:0;
        gpu=std::min({ceiling,s.videoBudget/8,room});
    }
    const auto total=2*std::min(cpu,gpu);
    return total/(16*MiB)*(16*MiB);
}
}
