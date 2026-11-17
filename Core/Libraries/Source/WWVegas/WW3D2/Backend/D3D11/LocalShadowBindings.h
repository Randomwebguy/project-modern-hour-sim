// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LocalLightShadows.h"
namespace GenZH::Modern {
// Native identity omits unused point directions, diffuse alpha and shadow tag.
// Normalize the same spot lanes as the volumetric source collector, but avoid
// repeating sRGB exponentiation while binding each surface draw.
template<class Native> LocalVolumeLight NativeLocalShadowKey(const Native &c,unsigned i){
 LocalVolumeLight key;const float type=c.lightPosition[i][3];if(type!=1&&type!=2)return key;
 std::copy_n(c.lightPosition[i],3,key.positionRange);key.positionRange[3]=c.lightAttenuation[i][3];key.radianceType[3]=type;
 for(unsigned j=0;j<3;++j){key.radianceType[j]=(std::max)(c.lightDiffuse[i][j],0.0f);key.attenuation[j]=c.lightAttenuation[i][j];}
 if(type==2){float length=0;for(unsigned j=0;j<3;++j)length+=c.lightDirection[i][j]*c.lightDirection[i][j];
  if(length>1e-8f)for(unsigned j=0;j<3;++j)key.direction[j]=c.lightDirection[i][j]/std::sqrt(length);
  key.spot[0]=std::clamp(c.lightSpot[i][0],-1.0f,1.0f);key.spot[1]=std::clamp(c.lightSpot[i][1],-1.0f,1.0f);key.spot[2]=c.lightSpot[i][2];
 }
 for(auto *field:{key.positionRange,key.radianceType,key.direction,key.attenuation,key.spot})for(unsigned j=0;j<4;++j)if(field[j]==0)field[j]=0;
 return key;
}
class LocalShadowBindings {
 std::array<LocalVolumeLight,LocalShadowLightLimit> keys_{};std::array<float,LocalShadowLightLimit> tags_{};
 // perf86: consecutive draws usually carry the same four native lights. Apply
 // is a pure function of these light fields and the bindings, so an identical
 // block (bitwise) reuses the previous tags. Clear/Add start a new generation.
 struct Memo {std::uint64_t generation=0;float lights[5][4][4]{};float tags[4]{};unsigned tagged=0;};
 mutable Memo memo_;std::uint64_t generation_=1;
 template<class Native> static void LightBlock(const Native &c,float (&out)[5][4][4]){
  std::memcpy(out[0],c.lightPosition,64);std::memcpy(out[1],c.lightDiffuse,64);std::memcpy(out[2],c.lightAttenuation,64);
  std::memcpy(out[3],c.lightSpot,64);std::memcpy(out[4],c.lightDirection,64);for(unsigned i=0;i<4;++i)out[4][i][3]=0; // overwritten before use
 }
public:
 void Clear(){tags_={};++generation_;}
 void Add(const LocalVolumeLight &key,unsigned slot){if(slot<LocalShadowLightLimit){keys_[slot]=key;tags_[slot]=float(slot+1);++generation_;}}
 template<class Native> unsigned Apply(Native &c)const{
  float block[5][4][4];LightBlock(c,block);
  if(memo_.generation==generation_&&!std::memcmp(block,memo_.lights,sizeof(block))){
   for(unsigned i=0;i<4;++i)c.lightDirection[i][3]=memo_.tags[i];
   return memo_.tagged;
  }
  unsigned tagged=0;
  for(unsigned i=0;i<4;++i){c.lightDirection[i][3]=0;if(c.lightPosition[i][3]!=1&&c.lightPosition[i][3]!=2)continue;const auto key=NativeLocalShadowKey(c,i);
   for(unsigned slot=0;slot<LocalShadowLightLimit;++slot)if(tags_[slot]>0&&key==keys_[slot]){c.lightDirection[i][3]=tags_[slot];++tagged;break;}
  }
  memo_.generation=generation_;std::memcpy(memo_.lights,block,sizeof(block));
  for(unsigned i=0;i<4;++i)memo_.tags[i]=c.lightDirection[i][3];memo_.tagged=tagged;
  return tagged;
 }
};
}
