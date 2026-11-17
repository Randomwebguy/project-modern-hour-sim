// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include "VehicleWear.h"
#include <cstdint>
namespace GenZH::Modern {
struct DrawPacket;
struct NativeSceneCounts {
    std::uint64_t tagged=0,drawable=0,rigid=0,skinned=0,sorted=0;
    std::uint64_t opaque=0,cutout=0,transparent=0,overlay=0,shader=0,untagged=0;
    void Add(const DrawPacket &p);
    NativeSceneCounts &operator+=(const NativeSceneCounts &x){
        tagged+=x.tagged;drawable+=x.drawable;rigid+=x.rigid;skinned+=x.skinned;sorted+=x.sorted;
        opaque+=x.opaque;cutout+=x.cutout;transparent+=x.transparent;overlay+=x.overlay;shader+=x.shader;untagged+=x.untagged;return *this;
    }
};
// Process-local identities, independent of pointers and gameplay RNG/save data.
// Cloning constructs a new identity; assignment keeps the destination identity.
class NativeLifetimeIdentity {
    inline static std::atomic<std::uint64_t> next_{1};
    const std::uint64_t id_=next_.fetch_add(1,std::memory_order_relaxed);
public:
    NativeLifetimeIdentity()=default;
    NativeLifetimeIdentity(const NativeLifetimeIdentity &):NativeLifetimeIdentity(){}
    NativeLifetimeIdentity &operator=(const NativeLifetimeIdentity &){return *this;}
    std::uint64_t Get()const{return id_;}
};
struct NativeMeshSource {
    std::uint64_t root=0,mesh=0,resource=0,parent=0;
    std::uint32_t drawable=0,pass=0;
    int lod=0,bone=0;
    bool skinned=false,sorted=false;
    // Occupies prior alignment padding; the appearance payload keeps its
    // existing byte offset and the large queued packet does not grow.
    std::uint32_t sortedVertexCount=0; // Populated sorter upload span, excluding spare capacity.
    VehicleWearSample vehicleWear{};
    bool Valid()const{return root&&mesh&&resource;}
};
inline thread_local NativeMeshSource currentNativeMeshSource{};
inline NativeMeshSource GetNativeMeshSource(){return currentNativeMeshSource;}
class ScopedNativeMeshSource {
    NativeMeshSource previous_;
public:
    explicit ScopedNativeMeshSource(NativeMeshSource source):previous_(currentNativeMeshSource){currentNativeMeshSource=source;}
    ~ScopedNativeMeshSource(){currentNativeMeshSource=previous_;}
    ScopedNativeMeshSource(const ScopedNativeMeshSource &)=delete;
    ScopedNativeMeshSource &operator=(const ScopedNativeMeshSource &)=delete;
};
// Native classes and headless fixture classes share this actual extraction code.
// Parent walk is bounded; a cyclic/malformed hierarchy falls back to no source.
template<class Node,class Mesh>
NativeMeshSource CaptureNativeMeshSource(Mesh &mesh,std::uint32_t pass,bool skinned){
    auto *model=mesh.Peek_Model();if(!model)return {};
    Node *leaf=&mesh,*root=leaf,*parent=leaf->Get_Container();
    unsigned depth=0;
    while(root->Get_Container()){
        if(++depth>64)return {};
        root=root->Get_Container();
    }
    NativeMeshSource result;
    result.root=root->Get_Native_Render_ID();result.mesh=leaf->Get_Native_Render_ID();
    result.resource=model->Get_Native_Geometry_ID();result.parent=parent?parent->Get_Native_Render_ID():0;
    result.vehicleWear=GetVehicleWear(result.root);
    result.drawable=root->Get_Native_Drawable_ID();result.lod=root->Get_LOD_Level();
    result.bone=parent?parent->Get_Sub_Object_Bone_Index(leaf):0;result.pass=pass;result.skinned=skinned;
    return result;
}
}
