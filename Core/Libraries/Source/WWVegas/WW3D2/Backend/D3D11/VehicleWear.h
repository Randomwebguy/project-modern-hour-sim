// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <cstdlib>
#include <new>
#include <limits>
namespace GenZH::Modern {
// Client-only appearance. No gameplay RNG, health mutation or simulation state.
struct VehicleWearSample {
 std::array<float,4> age{}; // scratches, dust, late corrosion, vehicle enabled
 std::array<float,4> profile{}; // root Z, geometry height, reserved, decal kind
};
// Registry storage remains independent of the legacy game heap and its locks,
// including pre-main initialization and post-game allocator shutdown.
template<class T> struct VehicleWearAllocator {
 using value_type=T;
 VehicleWearAllocator() noexcept=default;
 template<class U> VehicleWearAllocator(const VehicleWearAllocator<U>&) noexcept{}
 T *allocate(std::size_t n){
  if(n>(std::numeric_limits<std::size_t>::max)()/sizeof(T))throw std::bad_array_new_length();
  auto *p=std::malloc(n*sizeof(T));if(!p)throw std::bad_alloc();return static_cast<T*>(p);
 }
 void deallocate(T *p,std::size_t) noexcept{std::free(p);}
 template<class U> bool operator==(const VehicleWearAllocator<U>&)const noexcept{return true;}
 template<class U> bool operator!=(const VehicleWearAllocator<U>&)const noexcept{return false;}
};
using VehicleWearRegistry=std::unordered_map<std::uint64_t,VehicleWearSample,
 std::hash<std::uint64_t>,std::equal_to<std::uint64_t>,VehicleWearAllocator<std::pair<const std::uint64_t,VehicleWearSample>>>;
inline VehicleWearRegistry &VehicleWearBindings(){static VehicleWearRegistry bindings;return bindings;}
inline VehicleWearSample GetVehicleWear(std::uint64_t root){
 const auto it=VehicleWearBindings().find(root);return it==VehicleWearBindings().end()?VehicleWearSample{}:it->second;
}
class VehicleWearState {
 std::uint64_t root_=0;std::uint32_t object_=0,frame_=0;
 float x_=0,y_=0,health_=1,seconds_=0,distance_=0,damage_=0;bool initialized_=false;
public:
 VehicleWearState()=default;VehicleWearState(const VehicleWearState &)=delete;
 ~VehicleWearState(){Unbind();}
 void Unbind(){if(root_)VehicleWearBindings().erase(root_);root_=0;}
 VehicleWearSample Advance(std::uint32_t object,std::uint32_t frame,float x,float y,float z,float height,float health,bool active,bool airborne=false){
  VehicleWearSample result;
  if(!object||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(health))return result;
  health=std::clamp(health,0.f,1.f);
  if(!initialized_||object_!=object||frame<frame_||!active){
   object_=object;frame_=frame;x_=x;y_=y;health_=health;seconds_=distance_=damage_=0;initialized_=active;
  }else if(frame!=frame_){
   const float dt=float(frame-frame_)/30.f;seconds_+=dt;
   const float dx=x-x_,dy=y-y_,travel=std::sqrt(dx*dx+dy*dy);
   // A teleport must not make a factory-new vehicle instantly filthy.
   if(travel<=(std::max)(50.f,dt*80.f))distance_+=travel;
   damage_+=(std::max)(0.f,health_-health);frame_=frame;x_=x;y_=y;health_=health;
  }
  if(!active)return result;
  const float wear=std::clamp(seconds_/2400.f+distance_/20000.f+damage_*1.15f,0.f,1.f);
  const float dust=std::clamp((seconds_/3600.f+distance_/2200.f)*(airborne?.22f:1.f),0.f,1.f);
  const float rust=std::clamp((seconds_-600.f)/1200.f,0.f,1.f)*(.35f+.65f*wear);
  result.age={wear,dust,rust,1};result.profile={z,(std::max)(height,.1f),0.f,0};return result;
 }
 void Bind(std::uint64_t root,VehicleWearSample sample){
  if(root_!=root){Unbind();root_=root;}
  if(root_&&sample.age[3]>.5f)VehicleWearBindings()[root_]=sample;
  else if(root_)VehicleWearBindings().erase(root_);
 }
};
}
