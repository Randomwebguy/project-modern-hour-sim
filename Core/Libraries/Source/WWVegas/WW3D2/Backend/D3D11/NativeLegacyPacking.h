// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeMeshGeometry.h"
#include "DeferredLegacyBuffer.h"
namespace GenZH::Modern {
inline std::uint32_t NativeLegacyStride(const NativeGeometryView &v){
    constexpr auto allowed=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEXCOUNT_MASK;
    if(!v.positions||!v.normals||!v.vertices||(v.fvf&~allowed)||(v.fvf&D3DFVF_POSITION_MASK)!=D3DFVF_XYZ||!(v.fvf&D3DFVF_NORMAL))return 0;
    const unsigned channels=(v.fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
    if(channels>8)return 0;for(unsigned i=0;i<channels;++i)if(!v.uv[i])return 0;
    return 24+(v.fvf&D3DFVF_DIFFUSE?4:0)+(v.fvf&D3DFVF_SPECULAR?4:0)+channels*8;
}
inline bool PackNativeLegacyVertices(const NativeGeometryView &v,std::uint8_t *out,std::size_t bytes){
    const auto stride=NativeLegacyStride(v);if(!out||!stride||std::uint64_t(stride)*v.vertices!=bytes)return false;
    const unsigned channels=(v.fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
    for(std::uint32_t i=0;i<v.vertices;++i){auto *p=out+std::size_t(i)*stride;
        std::memcpy(p,static_cast<const std::uint8_t*>(v.positions)+std::size_t(i)*12,12);
        std::memcpy(p+12,static_cast<const std::uint8_t*>(v.normals)+std::size_t(i)*12,12);unsigned offset=24;
        if(v.fvf&D3DFVF_DIFFUSE){const auto color=v.diffuse?v.diffuse[i]:0xffffffffu;std::memcpy(p+offset,&color,4);offset+=4;}
        if(v.fvf&D3DFVF_SPECULAR){const auto color=v.specular?v.specular[i]:0xffffffffu;std::memcpy(p+offset,&color,4);offset+=4;}
        for(unsigned channel=0;channel<channels;++channel){std::memcpy(p+offset,static_cast<const std::uint8_t*>(v.uv[channel])+std::size_t(i)*v.UVStride(channel),8);offset+=8;}
    }return true;
}
inline bool PackNativeLegacyIndices(const NativeGeometryView &v,std::uint32_t base,std::uint8_t *out,std::size_t bytes){
    if(!out||!v.recipe||!v.triangles||v.gapCount||v.recipe->polygons.size()*std::uint64_t(12)!=bytes)return false;
    // Validate the complete selection before writing any index.
    for(auto polygon:v.recipe->polygons){if(polygon>=v.triangleCount)return false;std::uint32_t tri[3];std::memcpy(tri,static_cast<const std::uint8_t*>(v.triangles)+std::size_t(polygon)*12,12);
        for(auto index:tri)if(index>=v.vertices||std::uint64_t(base)+index>UINT32_MAX)return false;}
    for(auto polygon:v.recipe->polygons){std::uint32_t tri[3];std::memcpy(tri,static_cast<const std::uint8_t*>(v.triangles)+std::size_t(polygon)*12,12);
        for(auto index:tri){index+=base;std::memcpy(out,&index,4);out+=4;}}
    return true;
}
}
