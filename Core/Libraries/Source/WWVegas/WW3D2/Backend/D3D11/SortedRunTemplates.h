// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CompatibilityRenderer.h"
#include "NativeFixedState.h"
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
namespace GenZH::Modern {
// perf84: a sorted pool submits ~2,000 runs per frame from ~25 nodes, and the
// interleaved depth order revisits the same node constantly. Snapshot and
// CaptureTextures are pure functions of the capture key below (states, fixed
// state, bound textures and their revisions, mesh source, FVF, primitive,
// renderer context, the live texture name and terrain-atlas probe). When the key
// equals one captured earlier in the same scene, the captured fields are copied
// from that packet and only the run's geometry and provenance change. Any
// difference, including padding-insensitive source fields, takes the full path.
// perf84 measured the first implementation slower in the dense base (key copies,
// string allocation, a second packet copy). perf85 compares in place and assigns
// into the queued packet; it stays opt-in (GENZH_SORTED_RUN_TEMPLATES=on) until a
// same-process A/B shows a gain.
inline std::atomic<bool> sortedRunTemplatesEnabled{false};
inline std::atomic<bool> sortedRunTemplateVerify{false};
inline bool SameMeshSource(const NativeMeshSource &a,const NativeMeshSource &b){
    return a.root==b.root&&a.mesh==b.mesh&&a.resource==b.resource&&a.parent==b.parent&&a.drawable==b.drawable&&a.pass==b.pass&&
        a.lod==b.lod&&a.bone==b.bone&&a.skinned==b.skinned&&a.sorted==b.sorted&&a.sortedVertexCount==b.sortedVertexCount&&
        std::memcmp(&a.vehicleWear,&b.vehicleWear,sizeof(a.vehicleWear))==0;
}
// Live texture probes read by CaptureTextures/CaptureTerrain, without allocation.
struct SortedRunProbe {
    DWORD atlasBytes=0,nameSize=0;bool nameValid=false,usable=true;std::array<char,256> name{};
};
struct SortedRunTemplate {
    NativeMeshSource source{};DWORD fvf=0;D3DPRIMITIVETYPE primitive=D3DPT_TRIANGLELIST;
    std::array<std::uint64_t,8> context{};SortedRunProbe probe;
    std::array<const void*,8> textures{};std::array<std::uint64_t,8> revisions{};
    NativeFixedState fixed{};std::array<DWORD,256> render{};std::array<std::array<DWORD,32>,8> stages{};
    DrawPacket packet;
};
inline bool SameProbe(const SortedRunProbe &a,const SortedRunProbe &b){
    return a.usable&&b.usable&&a.atlasBytes==b.atlasBytes&&a.nameValid==b.nameValid&&a.nameSize==b.nameSize&&
        (!a.nameValid||std::memcmp(a.name.data(),b.name.data(),a.nameSize)==0);
}
// Fields Snapshot and CaptureTextures write; geometry, provenance, primitive and
// FVF stay those of the run being submitted.
inline void ApplySortedRunTemplate(const DrawPacket &t,DrawPacket &p){
    p.nativeSource=t.nativeSource;p.constants=t.constants;p.stateSnapshot=t.stateSnapshot;p.stateRevision=t.stateRevision;
    p.textures=t.textures;p.capturedTextures=t.capturedTextures;p.materialTextures=t.materialTextures;
    p.terrainHeight=t.terrainHeight;p.terrainClass=t.terrainClass;p.terrainCells=t.terrainCells;p.terrainClasses=t.terrainClasses;p.terrainShadowOwner=t.terrainShadowOwner;
    std::memcpy(p.terrainDimensions,t.terrainDimensions,sizeof(p.terrainDimensions));p.terrainClimate=t.terrainClimate;
    p.terrain=t.terrain;p.groundOverlay=t.groundOverlay;p.groundExclusionOverlay=t.groundExclusionOverlay;
    p.heightTexture=t.heightTexture;p.emissiveTexture=t.emissiveTexture;p.wearTextures=t.wearTextures;
    p.pixelShader=t.pixelShader;p.vertexShader=t.vertexShader;p.pixelAssembly=t.pixelAssembly;p.vertexAssembly=t.vertexAssembly;
    if(t.vertexShader)p.vertexConstants=t.vertexConstants;
    p.outputOverlay=t.outputOverlay;
}
struct SortedRunTemplateCounts {std::uint64_t hits=0,misses=0,stores=0,bypass=0,verified=0,verifyMismatches=0;};
inline SortedRunTemplateCounts sortedRunTemplateCounts;
// Every field a template supplies, compared byte for byte (verification mode).
inline bool SameCapturedPacket(const DrawPacket &a,const DrawPacket &b){
    if(std::memcmp(&a.constants,&b.constants,sizeof(a.constants))||a.States().render!=b.States().render||a.States().stages!=b.States().stages||
       !SameMeshSource(a.nativeSource,b.nativeSource)||a.fvf!=b.fvf||a.primitive!=b.primitive||a.sourcePrimitive!=b.sourcePrimitive||
       a.pixelShader!=b.pixelShader||a.vertexShader!=b.vertexShader||AssemblyText(a.pixelAssembly)!=AssemblyText(b.pixelAssembly)||AssemblyText(a.vertexAssembly)!=AssemblyText(b.vertexAssembly)||
       a.vertexConstants!=b.vertexConstants||a.outputOverlay!=b.outputOverlay||
       a.terrain!=b.terrain||a.groundOverlay!=b.groundOverlay||a.groundExclusionOverlay!=b.groundExclusionOverlay||
       std::memcmp(a.terrainDimensions,b.terrainDimensions,sizeof(a.terrainDimensions))||
       a.terrainClimate.kind!=b.terrainClimate.kind||a.terrainClimate.snow!=b.terrainClimate.snow||
       a.terrainClimate.sand!=b.terrainClimate.sand||a.terrainClimate.grass!=b.terrainClimate.grass||
       a.terrainHeight.Get()!=b.terrainHeight.Get()||a.terrainClass.Get()!=b.terrainClass.Get()||a.terrainCells.Get()!=b.terrainCells.Get()||a.terrainClasses.Get()!=b.terrainClasses.Get()||
       a.terrainShadowOwner!=b.terrainShadowOwner||a.heightTexture.Get()!=b.heightTexture.Get()||a.emissiveTexture.Get()!=b.emissiveTexture.Get())return false;
    for(unsigned i=0;i<8;++i)if(a.textures[i]!=b.textures[i]||a.capturedTextures[i].Get()!=b.capturedTextures[i].Get())return false;
    for(unsigned i=0;i<2;++i)if(a.materialTextures[i].Get()!=b.materialTextures[i].Get())return false;
    for(unsigned i=0;i<3;++i)if(a.wearTextures[i].Get()!=b.wearTextures[i].Get())return false;
    return true;
}
}
