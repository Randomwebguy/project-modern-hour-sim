// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeLightCalibration.h"
#include <array>
#include <unordered_set>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>
namespace GenZH::Modern {
constexpr unsigned LocalVolumeLightLimit=128,LocalVolumeClusters=20*12*8;
struct LocalVolumeLight {
 float positionRange[4]{},radianceType[4]{},direction[4]{},attenuation[4]{},spot[4]{};
 bool operator==(const LocalVolumeLight &other)const{return !std::memcmp(this,&other,sizeof(*this));}
};
static_assert(sizeof(LocalVolumeLight)==80);
struct LocalVolumeLightHash {std::size_t operator()(const LocalVolumeLight &v)const{
 const auto *bytes=reinterpret_cast<const unsigned char*>(&v);std::uint64_t h=14695981039346656037ull;for(unsigned i=0;i<sizeof(v);++i)h=(h^bytes[i])*1099511628211ull;return static_cast<std::size_t>(h);
}};
struct LocalVolumeConstants {std::uint32_t metadata[4]{};std::array<LocalVolumeLight,LocalVolumeLightLimit> lights{};};
static_assert(sizeof(LocalVolumeConstants)==10256);
template<class Native> bool PrimaryLocalVolumePacket(const Native &c,const Native &primary,bool zEnabled){
 return zEnabled&&c.geometryFlags[0]<.5f&&c.geometryFlags[1]>.5f&&c.bridgeFlags[0]<.5f&&
  !std::memcmp(c.view,primary.view,64)&&!std::memcmp(c.projection,primary.projection,64)&&!std::memcmp(c.viewport,primary.viewport,16)&&
  c.bridgeFlags[2]==primary.bridgeFlags[2]&&c.bridgeFlags[3]==primary.bridgeFlags[3];
}
class LocalVolumeSources {
 std::unordered_set<LocalVolumeLight,LocalVolumeLightHash> unique_;
public:
 LocalVolumeConstants data;unsigned rejected=0,overflow=0,duplicates=0;
 LocalVolumeSources(){unique_.reserve(LocalVolumeLightLimit);}
 enum Outcome:std::uint8_t {NotLocal,Inserted,Duplicate,Rejected,Overflow};
 template<class Native> bool Add(const Native &c,unsigned i){return AddOutcome(c,i)==Inserted;}
 // perf86: the same light inputs again give: Inserted/Duplicate -> Duplicate,
 // Rejected -> Rejected, Overflow -> Overflow (counters match per-light calls).
 void Repeat(const std::array<std::uint8_t,4> &outcomes){
  for(const auto o:outcomes){if(o==Inserted||o==Duplicate)++duplicates;else if(o==Rejected)++rejected;else if(o==Overflow)++overflow;}
 }
 template<class Native> Outcome AddOutcome(const Native &c,unsigned i){
  const bool added=AddChecked(c,i);
  return added?Inserted:lastOutcome_;
 }
private:
 Outcome lastOutcome_=NotLocal;
 template<class Native> bool AddChecked(const Native &c,unsigned i){
  lastOutcome_=NotLocal;
  const float type=c.lightPosition[i][3];if(type!=1&&type!=2)return false;
  lastOutcome_=Rejected;
  LocalVolumeLight v;v.positionRange[3]=c.lightAttenuation[i][3];v.radianceType[3]=type;
  auto finite=[](float x){return std::isfinite(x)&&std::abs(x)<1e7f;};
  for(unsigned j=0;j<3;++j){const float color=c.lightDiffuse[i][j];if(!finite(c.lightPosition[i][j])||!finite(color)||!finite(c.lightAttenuation[i][j])||c.lightAttenuation[i][j]<0){++rejected;return false;}
   v.positionRange[j]=c.lightPosition[i][j];const float x=(std::max)(color,0.0f);v.radianceType[j]=(std::min)(65504.0f,NativeLightRadiance(x));v.attenuation[j]=c.lightAttenuation[i][j];
  }
  if(!finite(v.positionRange[3])||v.positionRange[3]<=0||v.attenuation[0]+v.attenuation[1]+v.attenuation[2]<=0||v.radianceType[0]+v.radianceType[1]+v.radianceType[2]<=0){++rejected;return false;}
  if(type==2){float length=0;for(unsigned j=0;j<3;++j){if(!finite(c.lightDirection[i][j])||!finite(c.lightSpot[i][j])){++rejected;return false;}length+=c.lightDirection[i][j]*c.lightDirection[i][j];}
   if(length<1e-8f||c.lightSpot[i][2]<0||c.lightSpot[i][0]<c.lightSpot[i][1]){++rejected;return false;}
   for(unsigned j=0;j<3;++j)v.direction[j]=c.lightDirection[i][j]/std::sqrt(length);
   v.spot[0]=std::clamp(c.lightSpot[i][0],-1.0f,1.0f);v.spot[1]=std::clamp(c.lightSpot[i][1],-1.0f,1.0f);v.spot[2]=c.lightSpot[i][2];
  }
  // Canonical signed zero makes exact native repeats stable hash keys.
  for(auto *field:{v.positionRange,v.radianceType,v.direction,v.attenuation,v.spot})for(unsigned j=0;j<4;++j)if(field[j]==0)field[j]=0;
  if(unique_.find(v)!=unique_.end()){++duplicates;lastOutcome_=Duplicate;return false;}
  if(data.metadata[0]>=LocalVolumeLightLimit){++overflow;lastOutcome_=Overflow;return false;}
  unique_.insert(v);data.lights[data.metadata[0]++]=v;return true;
 }
};
}
