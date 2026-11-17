// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <d3d8.h>
// perf92: W3DShroud tags its projection texture with the exact world mapping
// that ShroudTextureShader writes into D3DTS_TEXTUREn (camera-space position
// texgen times inv(view)*offset*scale). The D3D11 renderer then evaluates the
// shroud from scene depth in one volumetric pass instead of the per-object
// ZFUNC=EQUAL multiplicative passes, which cannot match tessellated terrain.
struct D3D11ShroudMapping {
    std::uint32_t version=1;
    float cellWidth=0,cellHeight=0;      // world units per shroud texel
    float originX=0,originY=0;           // W3DShroud draw origin (texel 1,1 starts here)
    std::uint32_t textureWidth=0,textureHeight=0;
    std::uint32_t cellsX=0,cellsY=0;
    float clearLevel=1,fogLevel=.5f,shroudLevel=0; // GlobalData Clear/Fog/ShroudAlpha / 255
    float color[3]{1,1,1};               // GlobalData ShroudColor / 255
};
static_assert(sizeof(D3D11ShroudMapping)==60);
namespace GenZH::Modern {
// GENZH_SHROUD_FOG=off keeps the native passes; GENZH_SHROUD_FOG_FLAT=on draws
// the screen-space native-equivalent (multiplicative) shroud instead of clouds.
inline std::atomic<bool> shroudFogEnabled{true};
inline std::atomic<bool> shroudFogFlat{false};
// Texture coordinate of a world position, as the native texture matrix computes it.
inline void ShroudUV(const D3D11ShroudMapping &m,float x,float y,float &u,float &v){
    u=(x-m.originX+m.cellWidth)/(m.cellWidth*static_cast<float>(m.textureWidth));
    v=(y-m.originY+m.cellHeight)/(m.cellHeight*static_cast<float>(m.textureHeight));
}
inline bool ValidShroudMapping(const D3D11ShroudMapping &m){
    return m.version==1&&m.cellWidth>0&&m.cellHeight>0&&m.textureWidth&&m.textureHeight&&m.textureWidth<=8192&&m.textureHeight<=8192&&
           m.cellsX&&m.cellsY&&m.cellsX+2<=m.textureWidth&&m.cellsY+2<=m.textureHeight&&m.clearLevel>m.shroudLevel;
}
// Fade of the vision texture toward the latest cell states (exponential, per frame).
struct ShroudFadeConstants {float rates[4]{};}; // clearing blend, fogging blend, snap (1 = copy target), unused
static_assert(sizeof(ShroudFadeConstants)==16);
inline ShroudFadeConstants MakeShroudFadeConstants(double seconds,bool snap){
    // Fog clears within ~0.4 s of a cell coming into sight and returns over ~1.5 s.
    const double dt=seconds<0?0:(seconds>.25?.25:seconds);ShroudFadeConstants c;
    c.rates[0]=static_cast<float>(1-std::exp(-dt/.4));c.rates[1]=static_cast<float>(1-std::exp(-dt/1.5));c.rates[2]=snap?1.f:0.f;return c;
}
// Shader constants: world->uv (scale, offset), level decode and the native colour.
struct ShroudFogConstants {
    float mapping[4]{};   // 1/(cellW*texW), 1/(cellH*texH), cellW-originX, cellH-originY
    float levels[4]{};    // clear, fog, shroud, fog cover = (clear-fog)/(clear-shroud)
    float color[4]{};     // shroud colour rgb, 1/dot(colour,colour)
    float viewport[4]{};  // top-left xy, size zw (pixels)
    float settings[4]{};  // min depth, max depth, mode (0 volume, 1 flat), cell size (frontier warp)
    float vision[4]{};    // perf93: x = 1 when the faded vision texture supplies the cover
};
static_assert(sizeof(ShroudFogConstants)==96);
// vision: the cover comes from the faded per-cell vision (0 visible, .5 out of
// sight, 1 unexplored) instead of the native shroud levels.
inline ShroudFogConstants MakeShroudFogConstants(const D3D11ShroudMapping &m,const float viewport[4],float minDepth,float maxDepth,bool flat,bool vision=false){
    ShroudFogConstants c;
    c.mapping[0]=1/(m.cellWidth*static_cast<float>(m.textureWidth));c.mapping[1]=1/(m.cellHeight*static_cast<float>(m.textureHeight));
    c.mapping[2]=m.cellWidth-m.originX;c.mapping[3]=m.cellHeight-m.originY;
    c.levels[0]=m.clearLevel;c.levels[1]=m.fogLevel;c.levels[2]=m.shroudLevel;
    c.levels[3]=vision?.5f:(m.clearLevel-m.fogLevel)/(m.clearLevel-m.shroudLevel);c.vision[0]=vision?1.f:0.f;
    const float squared=m.color[0]*m.color[0]+m.color[1]*m.color[1]+m.color[2]*m.color[2];
    for(unsigned i=0;i<3;++i)c.color[i]=m.color[i];c.color[3]=squared>1e-6f?1/squared:0;
    for(unsigned i=0;i<4;++i)c.viewport[i]=viewport[i];
    c.settings[0]=minDepth;c.settings[1]=maxDepth;c.settings[2]=flat?1.f:0.f;c.settings[3]=(m.cellWidth+m.cellHeight)*.5f;
    return c;
}
// The native shroud material pass: projected shroud texture at stage 0,
// multiplicative sprite blend (ZERO, SRCCOLOR) and depth EQUAL, no programs.
template<class States>bool NativeShroudPassStates(const States &r,bool vertexShader,bool pixelShader){
    return !vertexShader&&!pixelShader&&r[D3DRS_ZENABLE]&&r[D3DRS_ZFUNC]==D3DCMP_EQUAL&&r[D3DRS_ALPHABLENDENABLE]&&
           r[D3DRS_SRCBLEND]==D3DBLEND_ZERO&&r[D3DRS_DESTBLEND]==D3DBLEND_SRCCOLOR;
}
}
