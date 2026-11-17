// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>
namespace GenZH::Modern {
struct CloudHistoryConstants {
 float previousVP[16]{},previousCamera[4]{},displacement[4]{},settings[4]{},pixels[4]{};
};
static_assert(sizeof(CloudHistoryConstants)==128);
// Render-only history policy. Camera matrices remain authoritative; no native
// scene/object sampling, simulation state or gameplay RNG is consulted.
class CloudHistoryState {
 bool valid_=false;unsigned frame_=0;
 float projection_[16]{},previousVP_[16]{},camera_[4]{},sun_[4]{},color_[4]{},ambient_[4]{},wind_[4]{},coverage_=0;
public:
 void Invalidate(){valid_=false;frame_=0;}
 bool Valid()const{return valid_;}
 static float WrappedDelta(float previous,float current){float d=previous-current;if(d>9000)d-=18000;if(d<-9000)d+=18000;return d;}
 template<class Atmosphere> CloudHistoryConstants Prepare(const Atmosphere &data,const float *projection,unsigned width,unsigned height)const{
  CloudHistoryConstants c;std::memcpy(c.previousVP,previousVP_,64);std::memcpy(c.previousCamera,camera_,16);c.pixels[0]=float(width);c.pixels[1]=float(height);
  bool valid=valid_&&projection&&!std::memcmp(projection_,projection,64);
  float distance=0;for(unsigned i=0;i<3;++i){const float d=camera_[i]-data.camera[i];distance+=d*d;}
  valid=valid&&distance<128*128&&std::abs(data.weather[0]-coverage_)<.05f;
  for(unsigned i=0;i<4;++i)valid=valid&&std::abs(data.sun[i]-sun_[i])<.002f;
  // sunColor.w is the independent weather-probe bank blend, not solar RGB.
  // It changes during every healthy probe transition and must not reset clouds.
  for(unsigned i=0;i<3;++i)valid=valid&&std::abs(data.sunColor[i]-color_[i])<.002f&&std::abs(data.ambient[i]-ambient_[i])<.002f;
  c.displacement[0]=WrappedDelta(wind_[0],data.wind[0]);c.displacement[1]=WrappedDelta(wind_[1],data.wind[1]);
  c.displacement[2]=wind_[3]-data.wind[3];c.settings[0]=valid?1.0f:0.0f;c.settings[1]=.85f;c.settings[2]=float(frame_%1024);
  return c;
 }
 template<class Atmosphere> void Commit(const Atmosphere &data,const float *projection){
  std::memcpy(projection_,projection,64);std::memcpy(previousVP_,data.viewProjection,64);std::memcpy(camera_,data.camera,16);
  std::memcpy(sun_,data.sun,16);std::memcpy(color_,data.sunColor,16);std::memcpy(ambient_,data.ambient,16);std::memcpy(wind_,data.wind,16);
  coverage_=data.weather[0];valid_=true;++frame_;
 }
};
}
