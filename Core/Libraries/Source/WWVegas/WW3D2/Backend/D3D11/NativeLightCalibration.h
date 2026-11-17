// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
namespace GenZH::Modern {
inline float NativeLightLinear(float encoded){
 const float x=(std::max)(encoded,0.f);
 return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);
}
inline float NativeLightRadiance(float encoded){return NativeLightLinear(encoded)*3.14159265f;}
// Explicit physical helpers; legacy light powers are artist inputs, not measured lux.
inline double PhotometricExposure(double ev100){return 1/(1.2*std::exp2(ev100));}
inline double LambertLuminance(double albedo,double illuminanceLux){return albedo*illuminanceLux/3.14159265358979323846;}
inline double PointIlluminance(double candela,double distanceMeters){return candela/(std::max)(distanceMeters*distanceMeters,1e-8);}
struct NativeLightInput {float global[4]{},diffuse[4][4]{},ambient[4][4]{};};
struct NativeLightData {float global[4]{},radiance[4][4]{},ambient[4][4]{};};
static_assert(sizeof(NativeLightInput)==144&&sizeof(NativeLightData)==144);
struct NativeLightStats {std::uint64_t requests=0,conversions=0,hits=0,evictions=0,invalid=0,uploads=0,skips=0,excluded=0;};
class NativeLightCache {
 struct Entry {NativeLightInput input;NativeLightData output;};
 std::array<Entry,128> entries_{};unsigned used_=0,next_=0,last_=0;
public:
 NativeLightStats stats;
 void Reset(){used_=next_=last_=0;stats={};}
 template<class Native> bool Resolve(const Native &c,NativeLightData &out){
  NativeLightInput key;std::memcpy(key.global,c.globalAmbient,16);std::memcpy(key.diffuse,c.lightDiffuse,64);std::memcpy(key.ambient,c.lightAmbient,64);++stats.requests;
  auto copy=[&](unsigned i){out=entries_[i].output;last_=i;++stats.hits;return true;};
  if(used_&&!std::memcmp(&key,&entries_[last_].input,sizeof(key)))return copy(last_);
  for(unsigned i=0;i<used_;++i)if(i!=last_&&!std::memcmp(&key,&entries_[i].input,sizeof(key)))return copy(i);
  auto finite=[](const auto &value){for(float v:value.global)if(!std::isfinite(v))return false;for(const auto &row:value.diffuse)for(float v:row)if(!std::isfinite(v))return false;for(const auto &row:value.ambient)for(float v:row)if(!std::isfinite(v))return false;return true;};
  if(!finite(key)){++stats.invalid;return false;}
  NativeLightData linear;linear.global[3]=1;
  for(unsigned channel=0;channel<3;++channel){
   linear.global[channel]=NativeLightLinear(key.global[channel]);
   for(unsigned light=0;light<4;++light){linear.radiance[light][channel]=NativeLightRadiance(key.diffuse[light][channel]);linear.ambient[light][channel]=NativeLightLinear(key.ambient[light][channel]);}
  }
  for(float v:linear.global)if(!std::isfinite(v)){++stats.invalid;return false;}
  for(const auto &row:linear.radiance)for(float v:row)if(!std::isfinite(v)){++stats.invalid;return false;}
  for(const auto &row:linear.ambient)for(float v:row)if(!std::isfinite(v)){++stats.invalid;return false;}
  unsigned slot;if(used_<entries_.size())slot=used_++;else{slot=next_;next_=(next_+1)%static_cast<unsigned>(entries_.size());++stats.evictions;}
  entries_[slot]={key,linear};last_=slot;out=linear;++stats.conversions;return true;
 }
 std::size_t PayloadBytes()const{return sizeof(entries_);}
 unsigned Size()const{return used_;}
};
}
