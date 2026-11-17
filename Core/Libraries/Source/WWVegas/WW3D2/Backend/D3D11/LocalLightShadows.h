// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LocalVolumetricLights.h"
#include "ShadowCascades.h"
#include "ShadowReuseCache.h"
#include <vector>
namespace GenZH::Modern {
constexpr unsigned LocalShadowLightLimit=4,LocalShadowFaces=24,LocalShadowSize=512;
constexpr std::uint64_t LocalShadowTriangleBudget=2000000;
struct LocalShadowConstants {float matrices[LocalShadowFaces][16]{};float positions[4][4]{};float settings[4]{};};
static_assert(sizeof(LocalShadowConstants)==1616);
struct LocalShadowPlan {
 LocalShadowConstants data;std::array<unsigned,4> sources{},faceCounts{};unsigned count=0;
};
inline unsigned LocalShadowFace(float x,float y,float z){
 const float ax=std::abs(x),ay=std::abs(y),az=std::abs(z);
 return ax>=ay&&ax>=az?(x>=0?0u:1u):(ay>=az?(y>=0?2u:3u):(z>=0?4u:5u));
}
inline LocalShadowPlan PlanLocalShadows(const LocalVolumeConstants &sources,const float *camera){
 using namespace DirectX;LocalShadowPlan result;
 if(!camera||!std::isfinite(camera[0])||!std::isfinite(camera[1])||!std::isfinite(camera[2]))return result;
 struct Rank {unsigned source;double score;};std::vector<Rank> ranks;ranks.reserve((std::min)(sources.metadata[0],LocalVolumeLightLimit));
 for(unsigned i=0;i<(std::min)(sources.metadata[0],LocalVolumeLightLimit);++i){const auto &l=sources.lights[i];
  if(l.positionRange[3]<=.2f||(l.radianceType[3]>1.5f&&(l.spot[1]<=.001f||l.spot[1]>=1)))continue; // Degenerate/wide cones retain unshadowed contribution.
  double distance=0;for(unsigned j=0;j<3;++j){const double d=double(l.positionRange[j])-camera[j];distance+=d*d;}distance=std::sqrt(distance);
  if(distance>double(l.positionRange[3])+4096)continue;
  const double d=(std::max)(distance,.1);const double attenuation=(std::max)(double(l.attenuation[0])+l.attenuation[1]*d+l.attenuation[2]*d*d,1e-6);
  ranks.push_back({i,(l.radianceType[0]*.2126+l.radianceType[1]*.7152+l.radianceType[2]*.0722)/attenuation});
 }
 std::stable_sort(ranks.begin(),ranks.end(),[](const Rank&a,const Rank&b){return a.score>b.score;});
 for(const auto &rank:ranks){if(result.count==LocalShadowLightLimit)break;const auto &l=sources.lights[rank.source];const unsigned slot=result.count++;
  result.sources[slot]=rank.source;const bool spot=l.radianceType[3]>1.5f;result.faceCounts[slot]=spot?1:6;
  std::copy_n(l.positionRange,4,result.data.positions[slot]);const auto eye=XMVectorSet(l.positionRange[0],l.positionRange[1],l.positionRange[2],1);
  const float nearPlane=(std::min)(.1f,l.positionRange[3]*.01f);
  const float fov=spot?2*std::acos((std::max)(.001f,l.spot[1])):XM_PIDIV2;
  // Position.w is the angular texel scale for surface filtering, not range.
  // Source range stays in LocalVolumeConstants and the projection far plane.
  result.data.positions[slot][3]=spot?std::tan(fov*.5f):1.0f;
  const auto projection=XMMatrixPerspectiveFovLH(fov,1,nearPlane,l.positionRange[3]);
  constexpr float directions[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  constexpr float ups[6][3]={{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
  for(unsigned face=0;face<result.faceCounts[slot];++face){
   const auto direction=spot?XMVectorSet(l.direction[0],l.direction[1],l.direction[2],0):XMVectorSet(directions[face][0],directions[face][1],directions[face][2],0);
   const auto up=spot?(std::abs(l.direction[2])>.95f?XMVectorSet(0,1,0,0):XMVectorSet(0,0,1,0)):XMVectorSet(ups[face][0],ups[face][1],ups[face][2],0);
   XMFLOAT4X4 matrix;XMStoreFloat4x4(&matrix,XMMatrixLookToLH(eye,direction,up)*projection);std::memcpy(result.data.matrices[slot*6+face],&matrix,64);
  }
 }
 result.data.settings[0]=float(result.count);result.data.settings[1]=1.0f/LocalShadowSize;result.data.settings[2]=.15f;result.data.settings[3]=.00002f;return result;
}
inline bool LocalShadowBoundsIntersect(const std::array<DirectX::XMFLOAT3,8>&corners,const float *vp){
 using namespace DirectX;XMFLOAT4X4 m;std::memcpy(&m,vp,64);const auto matrix=XMLoadFloat4x4(&m);unsigned outside[6]{};
 for(const auto &corner:corners){XMFLOAT4 clip;XMStoreFloat4(&clip,XMVector4Transform(XMVectorSet(corner.x,corner.y,corner.z,1),matrix));
  if(!std::isfinite(clip.x)||!std::isfinite(clip.y)||!std::isfinite(clip.z)||!std::isfinite(clip.w))return true;
  // Homogeneous clipping retains a box straddling the eye; perspective-dividing
  // its behind-camera corners can wrongly reject valid local shadow casters.
  outside[0]+=clip.x < -clip.w;outside[1]+=clip.x > clip.w;outside[2]+=clip.y < -clip.w;outside[3]+=clip.y > clip.w;outside[4]+=clip.z<0;outside[5]+=clip.z>clip.w;
 }
 for(unsigned n:outside)if(n==8)return false;return true;
}
class LocalShadowFaceCache {
 std::vector<ShadowDrawStamp> stamps_;std::array<float,16> matrix_{};bool valid_=false;
public:
 static constexpr unsigned MaxDraws=128;
 void Reset(){std::vector<ShadowDrawStamp>().swap(stamps_);valid_=false;}
 bool Matches(const std::vector<ShadowDrawStamp>&current,const float *vp)const{return valid_&&current.size()<=MaxDraws&&!std::memcmp(matrix_.data(),vp,64)&&stamps_==current;}
 void Commit(std::vector<ShadowDrawStamp> current,const float *vp){if(current.size()>MaxDraws){Reset();return;}stamps_=std::move(current);std::memcpy(matrix_.data(),vp,64);valid_=true;}
 std::size_t Bytes()const{return stamps_.capacity()*sizeof(ShadowDrawStamp);}
};
}
