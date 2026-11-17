// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
namespace GenZH::Modern {
// A deliberately exact subset of affine poses. Pure translations leave the
// normal basis and all passthrough UVs unchanged. Scale, rotation, projective,
// non-finite and mapper/custom-program cases retain the original path.
template<class Matrix> bool SortedWorldTranslation(const Matrix &m,float (&t)[3]){
    if(m._11!=1||m._22!=1||m._33!=1||m._44!=1||
        m._12!=0||m._13!=0||m._14!=0||m._21!=0||m._23!=0||m._24!=0||
        m._31!=0||m._32!=0||m._34!=0||
        !std::isfinite(m._41)||!std::isfinite(m._42)||!std::isfinite(m._43))return false;
    t[0]=m._41;t[1]=m._42;t[2]=m._43;return true;
}
template<class Vertex> void TranslateSortedVertices(Vertex *v,unsigned count,const float (&t)[3]){
    for(unsigned i=0;i<count;++i){v[i].x+=t[0];v[i].y+=t[1];v[i].z+=t[2];}
}
}
