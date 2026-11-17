#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace W3DGeometry {
struct PositionKey {
    std::uint32_t x, y, z;
    bool operator==(const PositionKey &b) const { return x==b.x && y==b.y && z==b.z; }
};
struct PositionHash {
    std::size_t operator()(const PositionKey &p) const {
        std::size_t h=p.x;
        h^=p.y+std::size_t(0x9e3779b9)+(h<<6)+(h>>2);
        h^=p.z+std::size_t(0x9e3779b9)+(h<<6)+(h>>2);
        return h;
    }
};
inline std::uint32_t PositionBits(float f) {
    if(f==0.0f)return 0; // Preserve numeric equality for signed zero.
    std::uint32_t bits;std::memcpy(&bits,&f,sizeof(bits));return bits;
}
// This is a shadow-only lookup: it never welds render vertices, normals or UVs.
template<class Vertex> unsigned BuildShadowVertexParents(const Vertex *vertices, unsigned count,
                                                        std::vector<unsigned> &parents) {
    parents.resize(count);
    std::unordered_map<PositionKey,unsigned,PositionHash> first;
    first.reserve(count);
    unsigned unique=0;
    for(unsigned i=0;i<count;++i){
        const auto &v=vertices[i];
        if(!std::isfinite(v.X)||!std::isfinite(v.Y)||!std::isfinite(v.Z)){
            parents[i]=i;++unique;continue;
        }
        auto result=first.emplace(PositionKey{PositionBits(v.X),PositionBits(v.Y),PositionBits(v.Z)},i);
        parents[i]=result.first->second;
        if(result.second)++unique;
    }
    return unique;
}
}
