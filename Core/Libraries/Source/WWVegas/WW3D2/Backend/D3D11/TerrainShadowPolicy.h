// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CompatibilityRenderer.h"
namespace GenZH::Modern {
inline void BindSceneShadowConstants(ID3D11DeviceContext *context,ID3D11Buffer *buffer,bool terrain){
    context->VSSetConstantBuffers(2,1,&buffer);context->PSSetConstantBuffers(2,1,&buffer);
    if(terrain){context->HSSetConstantBuffers(2,1,&buffer);context->DSSetConstantBuffers(2,1,&buffer);context->GSSetConstantBuffers(2,1,&buffer);}
}
inline bool TerrainShadowCaster(const DrawPacket &p){
    // Primary opaque terrain only. Blend/fog/water/roads and custom vertex
    // programs cannot accidentally grow grass or write an extra depth pass.
    if(!p.terrain||p.vertexShader||p.outputOverlay||p.primitive!=D3DPT_TRIANGLELIST||
       p.constants.geometryFlags[0]>.5f||!p.States().render[D3DRS_ZENABLE]||!p.States().render[D3DRS_ZWRITEENABLE]||
       p.States().render[D3DRS_ALPHABLENDENABLE]||p.constants.alphaStage[0][3]!=0||
       !p.terrainHeight||!p.terrainClass||!p.terrainCells)return false;
    for(float value:p.terrainDimensions)if(!std::isfinite(value))return false;
    return p.terrainDimensions[0]>0&&p.terrainDimensions[1]>0&&p.terrainDimensions[3]>0;
}
inline void ExpandTerrainShadowBounds(std::array<DirectX::XMFLOAT3,8> &bounds){
    DirectX::XMFLOAT3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for(const auto &p:bounds){lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);
        hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);}
    // Shared shader limits: relief +/-0.175; covered soil -0.06; blade height
    // <=1.5, side <=0.48, bent tip <=0.456. Padding includes safe margin.
    for(unsigned i=0;i<8;++i)bounds[i]={i&1?hi.x+.65f:lo.x-.65f,i&2?hi.y+.65f:lo.y-.65f,i&4?hi.z+1.75f:lo.z-.25f};
}
}
