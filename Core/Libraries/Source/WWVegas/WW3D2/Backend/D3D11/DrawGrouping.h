// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CompatibilityRenderer.h"
#include <cstring>
#include <DirectXMath.h>
#include <cmath>
#include "OpaqueSortPolicy.h"
namespace GenZH::Modern {
enum class InstanceReason:unsigned {Eligible,Geometry,Topology,Special,Shader,Layout,Depth,BlendStencil,DepthFunction,Count};
inline InstanceReason InstanceEligibility(const DrawPacket &p){
    if(!p.geometry||p.Indices().empty())return InstanceReason::Geometry;
    if(p.primitive!=D3DPT_TRIANGLELIST)return InstanceReason::Topology;
    if(p.outputOverlay||p.terrain||p.groundOverlay||p.nativeSource.skinned)return InstanceReason::Special;
    if(p.vertexShader||p.pixelShader||p.States().render[D3DRS_CLIPPLANEENABLE])return InstanceReason::Shader;
    if(!(p.fvf&D3DFVF_NORMAL)||!(p.constants.geometryFlags[0]<.5f))return InstanceReason::Layout;
    if(p.States().render[D3DRS_ZENABLE]!=D3DZB_TRUE||!p.States().render[D3DRS_ZWRITEENABLE])return InstanceReason::Depth;
    if((p.States().render[D3DRS_ALPHABLENDENABLE]&&(p.States().render[D3DRS_SRCBLEND]!=D3DBLEND_ONE||p.States().render[D3DRS_DESTBLEND]!=D3DBLEND_ZERO))||
       !OpaqueStencilSafe(p.States().render[D3DRS_STENCILENABLE],p.States().render[D3DRS_STENCILFUNC],p.States().render[D3DRS_STENCILFAIL],p.States().render[D3DRS_STENCILZFAIL],p.States().render[D3DRS_STENCILPASS]))return InstanceReason::BlendStencil;
    if(p.States().render[D3DRS_ZFUNC]!=D3DCMP_LESS&&p.States().render[D3DRS_ZFUNC]!=D3DCMP_LESSEQUAL)return InstanceReason::DepthFunction;
    return InstanceReason::Eligible;
}
inline bool CanInstance(const DrawPacket &p){
    return InstanceEligibility(p)==InstanceReason::Eligible;
}
inline bool SameInstanceState(const DrawPacket &a,const DrawPacket &b){
    if(a.geometry!=b.geometry||a.fvf!=b.fvf||!SameStateArrays(a,b))return false;
    // World and inverse-transpose normal matrices are the only variable fields.
    if(!BytesEqual(a.constants.view,b.constants.view,128)||
       !BytesEqual(a.constants.textureMatrices,b.constants.textureMatrices,sizeof(LegacyConstants)-offsetof(LegacyConstants,textureMatrices)))return false;
    for(unsigned i=0;i<8;++i)if(a.capturedTextures[i].Get()!=b.capturedTextures[i].Get()||a.textures[i]!=b.textures[i])return false;
    for(unsigned i=0;i<2;++i)if(a.materialTextures[i].Get()!=b.materialTextures[i].Get())return false;
    for(unsigned i=0;i<3;++i)if(a.wearTextures[i].Get()!=b.wearTextures[i].Get())return false;
    return a.heightTexture.Get()==b.heightTexture.Get()&&a.emissiveTexture.Get()==b.emissiveTexture.Get();
}
inline DrawGroups GroupOpaque(const std::vector<const DrawPacket*> &packets){
    DrawGroups result;result.reserve(packets.size());
    for(auto *p:packets){
        // Only adjacent equivalent draws merge. Even opaque coplanar surfaces
        // can depend on LESS/LESSEQUAL submission order; never reorder them.
        if(CanInstance(*p)&&!result.empty()&&CanInstance(*result.back().front())&&SameInstanceState(*result.back().front(),*p))result.back().push_back(p);
        else result.emplace_back(p);
    }
    return result;
}
struct ScreenBounds {float low[2]{},high[2]{};bool valid=false;};
inline bool ConservativeCameraBounds(const DrawPacket &p){
    // Frustum rejection does not reorder visible draws. Safe depth-winning
    // player/occluder stencil marking can therefore retain its exact state.
    // Custom/skinned/displaced/translucent draws still use the complete path.
    return p.geometry&&p.geometry->boundsReady&&!p.Indices().empty()&&
        p.primitive==D3DPT_TRIANGLELIST&&!p.outputOverlay&&!p.terrain&&!p.groundOverlay&&!p.nativeSource.skinned&&
        !p.vertexShader&&!p.pixelShader&&(p.fvf&D3DFVF_POSITION_MASK)==D3DFVF_XYZ&&
        p.constants.geometryFlags[0]<.5f&&!p.States().render[D3DRS_CLIPPLANEENABLE]&&
        OpaqueSortBypass(p.States().render[D3DRS_ALPHABLENDENABLE],p.States().render[D3DRS_SRCBLEND],p.States().render[D3DRS_DESTBLEND],
            p.States().render[D3DRS_ZENABLE],p.States().render[D3DRS_ZWRITEENABLE],p.States().render[D3DRS_ZFUNC],p.States().render[D3DRS_STENCILENABLE],0,
            p.States().render[D3DRS_STENCILFUNC],p.States().render[D3DRS_STENCILFAIL],p.States().render[D3DRS_STENCILZFAIL],p.States().render[D3DRS_STENCILPASS]);
}
inline ScreenBounds ProjectCameraBounds(const DrawPacket &p){
    ScreenBounds result;
    if(!ConservativeCameraBounds(p)||p.constants.viewport[2]<1||p.constants.viewport[3]<1)return result;
    using namespace DirectX;
    XMFLOAT4X4 w,v,pr;std::memcpy(&w,p.constants.world,64);std::memcpy(&v,p.constants.view,64);std::memcpy(&pr,p.constants.projection,64);
    auto matrix=XMMatrixMultiply(XMMatrixMultiply(XMLoadFloat4x4(&w),XMLoadFloat4x4(&v)),XMLoadFloat4x4(&pr));
    result.low[0]=result.low[1]=std::numeric_limits<float>::max();result.high[0]=result.high[1]=-result.low[0];
    const auto &g=*p.geometry;
    for(unsigned axis=0;axis<3;++axis)if(!std::isfinite(g.low[axis])||!std::isfinite(g.high[axis])||g.low[axis]>g.high[axis])return result;
    for(unsigned corner=0;corner<8;++corner){XMFLOAT4 clip;
        XMStoreFloat4(&clip,XMVector4Transform(XMVectorSet(corner&1?g.high[0]:g.low[0],corner&2?g.high[1]:g.low[1],corner&4?g.high[2]:g.low[2],1),matrix));
        // Near-plane crossings and invalid transforms are submission barriers.
        if(!std::isfinite(clip.w)||clip.w<=.00001f||!std::isfinite(clip.z)||clip.z<0)return ScreenBounds{};
        for(unsigned axis=0;axis<2;++axis){float x=(axis?clip.y:clip.x)/clip.w;if(!std::isfinite(x))return ScreenBounds{};
            result.low[axis]=std::min(result.low[axis],x);result.high[axis]=std::max(result.high[axis],x);}
    }
    // Two pixels on each side protects raster coverage/rounding at the edge.
    for(unsigned axis=0;axis<2;++axis){float pad=4/p.constants.viewport[axis+2];result.low[axis]-=pad;result.high[axis]+=pad;}
    result.valid=true;return result;
}
inline ScreenBounds ProjectInstanceBounds(const DrawPacket &p){return CanInstance(p)?ProjectCameraBounds(p):ScreenBounds{};}
inline bool Disjoint(const ScreenBounds &a,const ScreenBounds &b){
    return a.valid&&b.valid&&(a.high[0]<b.low[0]||b.high[0]<a.low[0]||a.high[1]<b.low[1]||b.high[1]<a.low[1]);
}
struct GroupingStats {
    std::array<std::uint64_t,static_cast<unsigned>(InstanceReason::Count)> reasons{};
    std::uint64_t seen=0,groups=0,reordered=0;
};
struct PreparedInstance {
    const DrawPacket *packet=nullptr;
    InstanceReason reason=InstanceReason::Geometry;
    ScreenBounds bounds;
};
// Reorder only across proven screen-disjoint opaque fixed-function packets.
// Shadow draws keep GroupOpaque: a camera-space proof does not apply there.
inline DrawGroups GroupScreenDisjoint(const std::vector<const DrawPacket*> &packets,GroupingStats &stats,const std::vector<PreparedInstance> *prepared=nullptr){
    DrawGroups result;std::vector<ScreenBounds> bounds;std::vector<InstanceReason> reasons;
    result.reserve(packets.size());bounds.reserve(packets.size());reasons.reserve(packets.size());
    for(std::size_t packetIndex=0;packetIndex<packets.size();++packetIndex){
        auto *p=packets[packetIndex];
        const auto *input=prepared&&prepared->size()==packets.size()&&(*prepared)[packetIndex].packet==p?&(*prepared)[packetIndex]:nullptr;
        const auto reason=input?input->reason:InstanceEligibility(*p);const bool eligible=reason==InstanceReason::Eligible;
        ++stats.seen;++stats.reasons[static_cast<unsigned>(reason)];
        auto rect=input?input->bounds:ProjectInstanceBounds(*p);bool merged=false;
        const std::size_t begin=result.size()>32?result.size()-32:0;
        if(eligible)for(std::size_t end=result.size();end>begin;){const auto i=--end;auto *candidate=result[i].front();
            if(reasons[i]==InstanceReason::Eligible&&SameInstanceState(*candidate,*p)){
                if(i+1!=result.size())++stats.reordered;
                result[i].push_back(p);
                if(bounds[i].valid&&rect.valid)for(unsigned axis=0;axis<2;++axis){bounds[i].low[axis]=std::min(bounds[i].low[axis],rect.low[axis]);bounds[i].high[axis]=std::max(bounds[i].high[axis],rect.high[axis]);}
                else bounds[i].valid=false;
                merged=true;break;
            }
            // Different cameras/viewports cannot share an NDC-space proof.
            if(reasons[i]!=InstanceReason::Eligible||std::memcmp(candidate->constants.view,p->constants.view,128)||
               std::memcmp(candidate->constants.viewport,p->constants.viewport,16)||!Disjoint(bounds[i],rect))break;
        }
        if(!merged){result.emplace_back(p);bounds.push_back(rect);reasons.push_back(reason);}
    }
    stats.groups+=result.size();return result;
}
}
