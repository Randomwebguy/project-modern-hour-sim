// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "WeatherClimate.h"
namespace GenZH::Modern {
enum class WeatherMode {Dynamic,Clear,Overcast,Rain,Off};
struct WeatherState {float coverage=.42f,rain=0,wetness=0,extinction=.00006f,windX=5,windY=2,time=0,offsetX=0,offsetY=0,flash=0;std::uint64_t flashEvent=0;};
// Render-only controller: does not modify simulation/RNG, map lighting, or replay.
// Update only for a primary world frame. Freeze while no world is submitted.
class WeatherController {
 WeatherMode mode_=WeatherMode::Dynamic;double time_=0,offsetX_=0,offsetY_=0;float wet_=0;
 MapClimate climate_=MapClimate::Temperate;bool nativeSnow_=false;
public:
 static std::uint32_t VisualHash(std::uint32_t v){v^=v>>16;v*=0x7feb352d;v^=v>>15;v*=0x846ca68b;return v^(v>>16);}
 static float FlashEnvelope(double age){
  auto pulse=[](double t,double length){if(t<=0||t>=length)return 0.0;const double x=std::sin(3.141592653589793*t/length);return x*x;};
  return static_cast<float>(pulse(age,.09)+.65*pulse(age-.14,.08)+.38*pulse(age-.31,.07));
 }
 void ResetWorld(){time_=offsetX_=offsetY_=0;wet_=0;}
 void Reset(WeatherMode mode){mode_=mode;climate_=MapClimate::Temperate;nativeSnow_=false;ResetWorld();}
 void SetClimate(MapClimate climate,bool nativeSnow){climate_=climate;nativeSnow_=nativeSnow;}
 WeatherState Sample()const{
  WeatherState s;s.time=static_cast<float>(std::fmod(time_,2500.0));s.offsetX=static_cast<float>(offsetX_);s.offsetY=static_cast<float>(offsetY_);
  float storm=0;
  if(mode_==WeatherMode::Dynamic){
   const double phase=std::fmod(time_,360.0)/360.0;
   // Raised-cosine changes have zero derivative at both ends; no abrupt presets.
   storm=static_cast<float>(.5-.5*std::cos(phase*6.283185307179586));
  }else if(mode_==WeatherMode::Overcast)storm=.75f;
  else if(mode_==WeatherMode::Rain)storm=1;
  else if(mode_==WeatherMode::Clear||mode_==WeatherMode::Off)s.coverage=.12f;
  if(mode_==WeatherMode::Dynamic||mode_==WeatherMode::Overcast||mode_==WeatherMode::Rain)s.coverage=.42f+.45f*storm;
  if(mode_!=WeatherMode::Overcast)s.rain=std::clamp((storm-.55f)/.45f,0.0f,1.0f);
  // Automatic precipitation follows actual map climate. A user-forced rain
  // preview remains available, but the mission's native snow always wins.
  if(mode_==WeatherMode::Dynamic&&climate_==MapClimate::Arid){s.coverage=.12f+.32f*storm;s.rain=0;}
  if((mode_==WeatherMode::Dynamic&&climate_==MapClimate::Alpine)||nativeSnow_)s.rain=0;
  // Deterministic render time only. Do not consume the simulation RNG or use
  // the wrapped rain animation clock: lightning remains continuous at 2500 s.
  if(s.rain>.55f&&s.coverage>.7f){
   const auto event=static_cast<std::uint64_t>(std::floor(time_/14.0));
   const auto seed=VisualHash(static_cast<std::uint32_t>(event)^static_cast<std::uint32_t>(event>>32));
   const double delay=1.5+6.0*seed/4294967295.0;
   s.flash=FlashEnvelope(std::fmod(time_,14.0)-delay)*std::clamp((s.rain-.55f)/.45f,0.0f,1.0f);s.flashEvent=event+1;
  }
  s.wetness=wet_;s.extinction=.00006f+.00018f*storm;s.windX=5+8*storm;s.windY=2+3*storm;
  return s;
 }
 WeatherState Update(double seconds){
  if(std::isfinite(seconds)&&seconds>0){
   const double dt=std::min(seconds,.25);time_+=dt;
   const auto state=Sample();const auto target=state.rain;
   offsetX_=std::fmod(offsetX_+state.windX*dt,18000.0);offsetY_=std::fmod(offsetY_+state.windY*dt,18000.0);
   const double rate=target>wet_?1.0/25.0:1.0/120.0;
   wet_+=static_cast<float>((target-wet_)*(1-std::exp(-dt*rate)));
   wet_=std::clamp(wet_,0.0f,1.0f);
  }
  return Sample();
 }
};
}
