// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
// Native vertex counts and triangle/index values are now 32-bit.
// Mesh algorithms use signed int counts; reject values outside that domain.
constexpr std::uint32_t W3DNativeMaxVertices=0x7fffffff;
inline bool W3DNativeMeshFits(std::uint32_t vertices){return vertices<=W3DNativeMaxVertices;}
inline bool W3DNativeTriangleFits(std::uint32_t vertices,std::uint32_t a,std::uint32_t b,std::uint32_t c){
    return W3DNativeMeshFits(vertices)&&a<vertices&&b<vertices&&c<vertices;
}
