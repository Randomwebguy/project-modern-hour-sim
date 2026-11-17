// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeSceneIdentity.h"
#include <cstring>
namespace GenZH::Modern {
inline thread_local bool orderedSortReference=false;
struct ScopedOrderedSortReference {
    bool previous;
    explicit ScopedOrderedSortReference(bool reference):previous(orderedSortReference){orderedSortReference=reference;}
    ~ScopedOrderedSortReference(){orderedSortReference=previous;}
};
struct OrderedSortCounts {std::uint64_t beforeDraws=0,afterDraws=0,triangles=0,translatedNodes=0,translatedVertices=0,sourceReject=0,materialReject=0,shaderReject=0,textureReject=0,transformReject=0,lightReject=0,pools=0,poolNodes=0,poolVertices=0,poolNodeMax=0,repeatedNodeRuns=0;};
inline thread_local OrderedSortCounts orderedSortCounts{};
// All nodes in this pool already have their evaluated rigid poses in the
// common captured VB/IB. Root/bone/pass/LOD IDs do not enter the fixed shader:
// world/view, material, lighting and wear below are the authoritative inputs.
// Source resources retain their original references through the entire flush.
// A merged draw keeps its first native source as representative diagnostics.
inline bool SameOrderedSortAppearance(const NativeMeshSource &a,const NativeMeshSource &b){
    return a.Valid()&&b.Valid()&&a.sorted&&b.sorted&&!a.skinned&&!b.skinned&&
        a.pass!=UINT32_MAX&&b.pass!=UINT32_MAX&&a.sortedVertexCount==b.sortedVertexCount&&
        a.vehicleWear.age==b.vehicleWear.age&&a.vehicleWear.profile==b.vehicleWear.profile;
}
template<class State>
bool SameOrderedSortState(const State &a,const State &b,const NativeMeshSource &sa,const NativeMeshSource &sb){
    if(!SameOrderedSortAppearance(sa,sb)){++orderedSortCounts.sourceReject;return false;}
    if(a.shader.Get_Bits()!=b.shader.Get_Bits()){++orderedSortCounts.shaderReject;return false;}
    if(!a.material||!b.material||!a.material->Is_Static_Draw_Equivalent(*b.material)){
        ++orderedSortCounts.materialReject;return false;
    }
    for(unsigned i=0;i<sizeof(a.Textures)/sizeof(a.Textures[0]);++i)
        if(a.Textures[i]!=b.Textures[i]){++orderedSortCounts.textureReject;return false;}
    if(std::memcmp(&a.world,&b.world,sizeof(a.world))||std::memcmp(&a.view,&b.view,sizeof(a.view))){
        ++orderedSortCounts.transformReject;return false;
    }
    for(unsigned i=0;i<4;++i)
        if(a.LightEnable[i]!=b.LightEnable[i]||(a.LightEnable[i]&&std::memcmp(&a.Lights[i],&b.Lights[i],sizeof(a.Lights[i])))){
            ++orderedSortCounts.lightReject;return false;
        }
    return true;
}
}
