// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GeometryMemoryBudget.h"
namespace GenZH::Modern {
// GPU-resident authored maps. Decoded CPU slices are released after upload;
// geometry/native textures/shadows keep their own charges and OS headroom.
inline std::uint64_t MaterialMemoryTarget(const GeometryMemorySample &s,std::uint64_t owned,unsigned pointerBits){
 constexpr std::uint64_t MiB=1024ull*1024;
 const std::uint64_t ceiling=(pointerBits==64?2048ull:1024ull)*MiB;
 std::uint64_t target=512*MiB;
 if(s.videoKnown){
  const auto reserve=std::max(128*MiB,s.videoBudget/16);
  const auto headroom=s.videoBudget>s.videoUsage?s.videoBudget-s.videoUsage:0;
  const auto room=headroom>=reserve?owned+(headroom-reserve)/3:(owned>reserve-headroom?owned-(reserve-headroom):0);
  target=std::min({ceiling,s.videoBudget/6,room});
 }
 if(s.cpuKnown){
  // Preserve address space for x86 native resources and the largest staging
  // slice. This is cache eviction, never map downsampling or channel removal.
  const auto reserve=(pointerBits==64?512ull:768ull)*MiB;
  if(s.virtualAvailable<reserve)target=std::min(target,owned>reserve-s.virtualAvailable?owned-(reserve-s.virtualAvailable):0);
  if(s.physicalAvailable<256*MiB)target=std::min(target,owned/2);
 }
 return std::clamp(target/(32*MiB)*(32*MiB),128*MiB,ceiling);
}
}
