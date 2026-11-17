// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

// This is supplied by TerrainTex, after WorldHeightMap has packed its actual
// map materials. Never infer terrain identity from the atlas's pixel colors.
enum class D3D11Ground : std::uint32_t {
    Unknown, Grass, Dirt, Soil, Stone, Sand, Snow, Asphalt, Concrete, Wood
};
struct D3D11TerrainRegion {
    std::uint32_t x=0,y=0,extent=0;
    D3D11Ground ground=D3D11Ground::Unknown;
    char texture[128]{};
};
struct D3D11TerrainAtlas {
    std::uint32_t version=2,width=0,height=0,count=0;
    std::uint32_t cellsX=0,cellsY=0;
    float borderWorld=0;
    float diffuse[3][4]{},ambient[3][4]{},direction[3][4]{};
    D3D11TerrainRegion regions[200]{};
};
// Canonical base/blend UVs per native world cell. All render passes sample the
// same surface height, even when their current texture coordinates differ.
struct D3D11TerrainCell {
    float coordinates[4][4]{}; // xy base UV, zw secondary UV; corners 00,10,11,01
    float alpha[4]{};
};
static_assert(sizeof(D3D11TerrainCell)==80);
// terrain97: native terrain elevation, private data after the cells. Header, then
// (cellsX+1)*(cellsY+1) float vertex heights (world units, row-major, map index
// order) and cellsX*cellsY bytes: 1 where the cell is split along 10-01.
struct D3D11TerrainElevationHeader {std::uint32_t version=1,cellsX=0,cellsY=0,reserved=0;};
static_assert(sizeof(D3D11TerrainElevationHeader)==16);
inline std::size_t D3D11TerrainElevationBytes(std::uint32_t cellsX,std::uint32_t cellsY){
 return sizeof(D3D11TerrainElevationHeader)+std::size_t(cellsX+1)*(cellsY+1)*sizeof(float)+std::size_t(cellsX)*cellsY;
}
struct D3D11GroundBlock {
    float x=0,y=0,major=0,minor=0,angle=0;
};
