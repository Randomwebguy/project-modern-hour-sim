// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d8.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GenZH::Modern {
struct LegacyVertex {
    float position[4]={0,0,0,1};
    float normal[4]={0,0,1,0};
    float diffuse[4]={1,1,1,1};
    float specular[4]={0,0,0,0};
    float uv[8][4]{};
    float registers[16][4]{};
};
static_assert(sizeof(LegacyVertex)==448);
struct FixedVertex {
    float position[4]={0,0,0,1};
    float normal[4]={0,0,1,0};
    float diffuse[4]={1,1,1,1};
    float specular[4]={0,0,0,0};
    float uv[8][4]{};
};
static_assert(sizeof(FixedVertex)==192);
// Exact float32 prefix for the native sorted XYZ/N/D/TEX2 layout. The absent
// UV2..7 and specular are all zero; the input layout aliases the zero specular
// vector for those channels. No quantization, attribute removal or shader change.
struct TwoUVFixedVertex {
    float position[4]={0,0,0,1};
    float normal[4]={0,0,1,0};
    float diffuse[4]={1,1,1,1};
    float specular[4]{};
    float uv[2][4]{};
};
static_assert(sizeof(TwoUVFixedVertex)==96);
inline bool TwoUVFixedLayout(DWORD fvf,UINT stride){
    return fvf==(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2)&&stride==44;
}
HRESULT DecodeTwoUVFixedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,
    UINT count,std::vector<TwoUVFixedVertex> &result,std::string &error);
// Checked CPU conversion, shared by static/dynamic W3D and direct UP draws.
HRESULT DecodeVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,
                       UINT first,UINT count,std::vector<LegacyVertex> &result,std::string &error);
HRESULT DecodeFixedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,
                           UINT first,UINT count,std::vector<FixedVertex> &result,std::string &error);
// Indexed compatibility draws retain only vertices actually addressed by the
// index stream. All shader registers/UVs are decoded unchanged; index order,
// repeated corners and strip degenerates remain intact.
HRESULT DecodeReferencedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,
 UINT first,UINT count,std::vector<std::uint32_t> &indices,std::vector<LegacyVertex> &vertices,std::string &error);
// Same checked FVF conversion and index order; fixed-function draws do not
// allocate the unused programmable-shader register payload.
HRESULT DecodeReferencedFixedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,
 UINT first,UINT count,std::vector<std::uint32_t> &indices,std::vector<FixedVertex> &vertices,std::string &error);
UINT PrimitiveVertexCount(D3DPRIMITIVETYPE type,UINT primitives);
HRESULT DecodeIndices(const void *data,std::size_t bytes,D3DFORMAT format,UINT first,UINT count,
                      UINT base,UINT vertexCount,std::vector<std::uint32_t> &result);
HRESULT DecodeBumpTexture(const void *data,std::size_t bytes,UINT pitch,UINT width,UINT height,
                          D3DFORMAT format,std::vector<float> &rgba);
struct VertexStreamSpan {const void *data=nullptr;std::size_t bytes=0;UINT stride=0;};
HRESULT DecodeDeclaration(const std::vector<DWORD> &declaration,const std::array<VertexStreamSpan,16> &streams,
                          UINT first,UINT count,std::vector<LegacyVertex> &vertices,std::string &error);
std::string TranslateVertexShader(const std::string &assembly,std::string &error);
std::string TranslatePixelShader(const std::string &assembly,std::string &error);
}

namespace GenZH::Modern {
HRESULT DecodeReferencedDeclaration(const std::vector<DWORD> &declaration,const std::array<VertexStreamSpan,16> &streams,
 UINT first,UINT count,std::vector<std::uint32_t> &indices,std::vector<LegacyVertex> &vertices,std::string &error);
}
