// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <algorithm>
namespace GenZH::Modern {
// Distant sky only: no native scene capture, visibility queries or game RNG.
// Bound updates at 8 Hz; camera relocation and changed native lighting invalidate.
class WeatherProbeState {
 bool ready_=false;double age_=0;std::array<float,13> identity_{};
 static std::array<float,13> Identity(const float *camera,const float *sun,const float *color,const float *ambient){
  return {std::floor(camera[0]/256),std::floor(camera[1]/256),std::floor(camera[2]/64),sun[0],sun[1],sun[2],color[0],color[1],color[2],sun[3],ambient?ambient[0]:0,ambient?ambient[1]:0,ambient?ambient[2]:0};
 }
public:
 static constexpr double Interval=.125;
 void Advance(double seconds){if(std::isfinite(seconds)&&seconds>0)age_+=(std::min)(seconds,.25);}
 bool NeedsUpdate(const float *camera,const float *sun,const float *color,const float *ambient=nullptr)const{
  if(!ready_||age_>=Interval)return true;
  const auto current=Identity(camera,sun,color,ambient);
  for(unsigned i=0;i<current.size();++i)if(std::abs(current[i]-identity_[i])>.0001f)return true;
  return false;
 }
 void Commit(const float *camera,const float *sun,const float *color,const float *ambient=nullptr){
  identity_=Identity(camera,sun,color,ambient);
  ready_=true;age_=0;
 }
 float Blend()const{return static_cast<float>((std::min)(age_/Interval,1.0));}
 bool Ready()const{return ready_;}
};
}
