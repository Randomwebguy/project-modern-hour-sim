// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainShadowPolicy.h"
#include "DrawGrouping.h"
namespace GenZH::Modern {
inline bool CaptureShadowStamp(const DrawPacket &packet,ShadowDrawStamp &stamp,std::uint64_t exclusionRevision=0){
    // Terrain may reuse only revision-tagged source buffers, immutable map
    // leases and an exact exclusion revision. Untracked data stays fully live.
    // Skinned/custom draws and unowned SRVs always keep their complete live path.
    const bool terrain=TerrainShadowCaster(packet);
    const bool opaqueGeometry=!terrain&&ConservativeCameraBounds(packet)&&packet.constants.alphaStage[0][3]==0;
    if((packet.terrain&&!terrain)||packet.nativeSource.skinned||(packet.nativeSource.sorted&&!opaqueGeometry)||packet.vertexShader||
       (packet.pixelShader&&!terrain)||packet.Indices().empty())return false;
    if(!packet.geometry&&!(terrain&&packet.sourceVertex&&packet.sourceIndex))return false;
    if(terrain&&!packet.terrainShadowOwner)return false;
    stamp={};stamp.owners[0]=packet.geometry?std::static_pointer_cast<void>(packet.geometry):packet.sourceVertex;stamp.ownerMask=1;
    if(!packet.geometry){stamp.owners[9]=packet.sourceIndex;stamp.ownerMask|=1u<<9;}
    if(terrain){stamp.owners[10]=packet.terrainShadowOwner;stamp.ownerMask|=1u<<10;}
    std::size_t offset=0;
    auto append=[&](const void *data,std::size_t bytes){std::memcpy(stamp.bytes.data()+offset,data,bytes);offset+=bytes;};
    if(opaqueGeometry){
        // Solid fixed-function depth depends only on geometry and world pose.
        // Native light/exposure/camera and material-map readiness change color,
        // not this coverage. The cache separately compares the light matrix.
        // Alpha-tested, terrain, custom/skinned and unsafe stencil draws retain
        // the complete conservative stamp or live fallback below.
        append(packet.constants.world,64);append(packet.sourceIdentity.data(),sizeof(packet.sourceIdentity));
        const std::uint32_t coverage[]={packet.fvf,static_cast<std::uint32_t>(packet.primitive),static_cast<std::uint32_t>(packet.sourcePrimitive)};
        append(coverage,sizeof(coverage));return true;
    }
    if(terrain){
        // Opaque canonical terrain uses fixed-function XYZ transforms and the
        // shared HS/DS/GS. Its shadow PS returns before reading color/alpha/UV.
        // Native lighting, fog, material readiness, UV texture transforms and
        // surface wear therefore cannot change this depth coverage.
        // Preserve the *native* camera and viewport: they select tessellation
        // density and grass seeds even when rasterizing the light projection.
        // The light projection is compared separately by ShadowReuseCache.
        append(packet.constants.world,64);append(packet.constants.view,64);
        append(packet.constants.projection,64);append(packet.constants.viewport,16);
        append(packet.constants.modernMaterial,4);
        const auto materialFlags=packet.constants.modernMaterial[3];
        // accum105: terrain101+ sets pixel-only bits above 65535 (field, hex, wet ground,
        // accumulation); the former 16-bit limit made every library terrain stamp fail, so its
        // shadow was redrawn each frame. Coverage depends on the blades (32) and relief (8192).
        if(!std::isfinite(materialFlags)||materialFlags<0||materialFlags>16777215)return false;
        const std::uint32_t grass=static_cast<std::uint32_t>(materialFlags)&(32u|8192u);
        append(&grass,4);append(packet.sourceIdentity.data(),sizeof(packet.sourceIdentity));
        append(packet.terrainDimensions,16);
        const std::uint64_t views[]={reinterpret_cast<std::uintptr_t>(packet.terrainHeight.Get()),
            reinterpret_cast<std::uintptr_t>(packet.terrainClass.Get()),reinterpret_cast<std::uintptr_t>(packet.terrainCells.Get())};
        append(views,sizeof(views));append(&exclusionRevision,8);
        const std::uint32_t coverage[]={packet.fvf,static_cast<std::uint32_t>(packet.primitive),static_cast<std::uint32_t>(packet.sourcePrimitive)};
        append(coverage,sizeof(coverage));return true;
    }
    // Vehicle surface aging does not change opaque geometry/shadow coverage.
    // Exclude the appended appearance lanes to retain valid warm shadow stamps.
    static_assert(offsetof(LegacyConstants,vehicleAge)+sizeof(packet.States().stages)+sizeof(GeometryKey)+8*16+8+16+24+8+8==3552);
    append(&packet.constants,offsetof(LegacyConstants,vehicleAge));append(&packet.States().stages,sizeof(packet.States().stages));
    append(packet.sourceIdentity.data(),sizeof(packet.sourceIdentity));
    for(unsigned i=0;i<8;++i){
        if(packet.capturedTextures[i]&&!packet.textures[i])return false;
        const std::uint64_t srv=reinterpret_cast<std::uintptr_t>(packet.capturedTextures[i].Get());
        const std::uint64_t revision=packet.textures[i]?packet.textures[i]->revision:0;
        append(&srv,8);append(&revision,8);
        if(packet.textures[i]){stamp.owners[i+1]=packet.textures[i];stamp.ownerMask|=static_cast<std::uint16_t>(1u<<(i+1));}
    }
    const std::uint32_t fvf=packet.fvf,primitive=static_cast<std::uint32_t>(packet.primitive);
    append(&fvf,4);append(&primitive,4);
    append(packet.terrainDimensions,16);
    const std::uint64_t terrainSRVs[]={reinterpret_cast<std::uintptr_t>(packet.terrainHeight.Get()),reinterpret_cast<std::uintptr_t>(packet.terrainClass.Get()),reinterpret_cast<std::uintptr_t>(packet.terrainCells.Get())};
    append(terrainSRVs,24);const std::uint64_t revision=terrain?exclusionRevision:0;append(&revision,8);
    const std::uint32_t sourcePrimitive=static_cast<std::uint32_t>(packet.sourcePrimitive),pixelShader=packet.pixelShader;
    append(&sourcePrimitive,4);append(&pixelShader,4);return true;
}
}
