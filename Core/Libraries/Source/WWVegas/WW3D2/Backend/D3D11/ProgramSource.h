// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LegacyDraw.h"
#include <sstream>
#include <cstring>
namespace GenZH::Modern {
inline void ReplaceProgramMarker(std::string &source,const char *marker,const std::string &value){source.replace(source.find(marker),std::strlen(marker),value);}
// Shared generator: offline prewarm keys exactly match runtime binding variants.
inline std::string BuildProgramSource(const char *base,bool hasVertex,bool hasPixel,
 const std::string &vertexAssembly,const std::string &pixelAssembly,const std::array<D3DRESOURCETYPE,8> &types,std::string &error){
 std::string source=(hasVertex?std::string("#define LEGACY_PROGRAMMABLE_INPUT 1\n"):std::string())+base;std::ostringstream textures;
 for(UINT i=0;i<8;++i){auto type=types[i]?types[i]:D3DRTYPE_TEXTURE;
  std::string dimension=type==D3DRTYPE_CUBETEXTURE?"Cube":type==D3DRTYPE_VOLUMETEXTURE?"3D":"2D";
  textures<<"Texture"<<dimension<<" Texture"<<i<<":register(t"<<i<<");SamplerState Sampler"<<i<<":register(s"<<i<<");\n";
  if(types[i])textures<<"float4 Sample"<<i<<"(float4 coord){return Texture"<<i<<".Sample(Sampler"<<i<<",coord."<<(dimension=="2D"?"xy":"xyz")<<");}\n";
  else textures<<"float4 Sample"<<i<<"(float4 coord){return 1;}\n";
 }
 textures<<"float4 SampleStage(int index,float4 coord){\n";
 for(UINT i=0;i<8;++i)textures<<"if(index=="<<i<<")return Sample"<<i<<"(coord);\n";
 textures<<"return 1;}\n";ReplaceProgramMarker(source,"/*TEXTURES*/",textures.str());
 std::string pixel=hasPixel?TranslatePixelShader(pixelAssembly,error):"float4 LegacyPixel(float4 diffuse,float4 specular,float4 coords[8]){return FixedPixel(diffuse,specular,coords);}";
 if(pixel.empty())return {};ReplaceProgramMarker(source,"/*PIXEL_PROGRAM*/",pixel);
 std::string vertex=hasVertex?TranslateVertexShader(vertexAssembly,error):"LegacyOutput LegacyVS(LegacyInput input){return FixedVS(input);}";
 if(vertex.empty())return {};ReplaceProgramMarker(source,"/*VERTEX_PROGRAM*/",vertex);
 return source;
}
}
