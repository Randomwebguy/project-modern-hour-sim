// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace GenZH::Modern {
// Appended only for the paired compact raster entries. The original pixel
// entries and arbitrary legacy shader input signatures remain available.
inline constexpr char TerrainCompactShaderSuffix[]=R"GENZH(
LegacyOutput UnpackTerrainRaster(TerrainRasterOutput input){
 LegacyOutput output=(LegacyOutput)0;
 output.position=input.position;output.diffuse=input.diffuse;output.specular=input.specular;
 output.uv0=input.uv0;output.uv1=input.uv1;output.uv2=input.uv2;output.uv3=input.uv3;
 output.fog=input.fog;output.worldPosition=input.worldPosition;output.worldNormal=input.worldNormal;output.blade=input.blade;
 return output;
}
LegacyLightingOutput TerrainCompactPS(TerrainRasterOutput input){return LegacyPS(UnpackTerrainRaster(input));}
void TerrainCompactShadowPS(TerrainRasterOutput input){LegacyShadowPS(UnpackTerrainRaster(input));}
)GENZH";
}
