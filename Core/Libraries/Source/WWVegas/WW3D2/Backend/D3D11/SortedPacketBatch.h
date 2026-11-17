// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CompatibilityRenderer.h"
#include <cstring>
namespace GenZH::Modern {
inline bool SameSortedSource(const NativeMeshSource &a,const NativeMeshSource &b){
 return a.root==b.root&&a.mesh==b.mesh&&a.resource==b.resource&&a.parent==b.parent&&a.drawable==b.drawable&&
  a.pass==b.pass&&a.lod==b.lod&&a.bone==b.bone&&a.skinned==b.skinned&&a.sorted==b.sorted&&
  a.vehicleWear.age==b.vehicleWear.age&&a.vehicleWear.profile==b.vehicleWear.profile&&a.sortedVertexCount==b.sortedVertexCount;
}
// Concatenate only neighboring fixed-function sorted triangles with exactly
// the same draw state. This is not instancing or reordering: transparent and
// cutout primitive order, all UVs, normals and material leases stay unchanged.
inline bool TryAppendSortedPacket(DrawPacket &a,const DrawPacket &b){
 const auto eligible=[](const DrawPacket &p){return p.nativeSource.sorted&&!p.nativeSource.skinned&&
  !p.geometry&&!p.vertexShader&&!p.pixelShader&&!p.terrain&&!p.groundOverlay&&!p.groundExclusionOverlay&&
  !p.outputOverlay&&p.primitive==D3DPT_TRIANGLELIST&&!p.fixedVertices.empty()&&!p.indices.empty()&&p.indices.size()%3==0;};
 if(!eligible(a)||!eligible(b)||a.fvf!=b.fvf||a.sourcePrimitive!=b.sourcePrimitive||
  !SameStateArrays(a,b)||
  !BytesEqual(&a.constants,&b.constants,sizeof(a.constants))||
  !SameSortedSource(a.nativeSource,b.nativeSource))return false;
 for(unsigned i=0;i<8;++i)if(a.textures[i]!=b.textures[i]||a.capturedTextures[i].Get()!=b.capturedTextures[i].Get())return false;
 for(unsigned i=0;i<2;++i)if(a.materialTextures[i].Get()!=b.materialTextures[i].Get())return false;
 for(unsigned i=0;i<3;++i)if(a.wearTextures[i].Get()!=b.wearTextures[i].Get())return false;
 if(a.heightTexture.Get()!=b.heightTexture.Get()||a.emissiveTexture.Get()!=b.emissiveTexture.Get())return false;
 // A transient upload byte budget, never a mesh/index precision limit.
 constexpr std::size_t budget=8u*1024u*1024u;
 const auto vertices=a.fixedVertices.size()+b.fixedVertices.size(),indices=a.indices.size()+b.indices.size();
 if(vertices*sizeof(FixedVertex)+indices*sizeof(std::uint32_t)>budget)return false;
 for(auto index:b.indices)if(index>=b.fixedVertices.size())return false;
 const auto base=static_cast<std::uint32_t>(a.fixedVertices.size());
 a.fixedVertices.insert(a.fixedVertices.end(),b.fixedVertices.begin(),b.fixedVertices.end());
 for(auto index:b.indices)a.indices.push_back(base+index);
 // Merged dynamic geometry cannot masquerade as an immutable source range.
 a.sourceIdentity={};a.sourceVertex.reset();a.sourceIndex.reset();return true;
}
}
