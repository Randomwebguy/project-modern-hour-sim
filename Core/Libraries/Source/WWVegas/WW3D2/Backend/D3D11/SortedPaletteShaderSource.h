// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <stdexcept>
#include <utility>
namespace GenZH::Modern {
// A separate canonical VS variant. Pixel/geometry/shadow shader sources and
// every original UV/color attribute remain untouched. Palette draws use zero
// BaseVertexLocation and an equivalent IA byte offset; the original base is
// added explicitly to the raw SV_VertexID for its immutable selector table.
inline std::string SortedPaletteVertexSource(std::string source){
    const auto replace=[&](const std::string &from,const std::string &to){
        const auto at=source.find(from);
        if(at==std::string::npos||source.find(from,at+from.size())!=std::string::npos)
            throw std::runtime_error("sorted palette VS source anchor changed");
        source.replace(at,from.size(),to);
    };
    replace("StructuredBuffer<InstanceMatrices> DrawInstances:register(t15);",
        "StructuredBuffer<InstanceMatrices> DrawInstances:register(t15);\n"
        "struct SortedMaterialMatrices { row_major float4x4 world; row_major float4x4 normal; float4 diffuse; float4 ambient; float4 specular; };\nStructuredBuffer<SortedMaterialMatrices> SortedWorldMatrices:register(t120);\n"
        "StructuredBuffer<uint> SortedVertexWorld:register(t121);");
    replace("uint instanceID:SV_InstanceID;","uint instanceID:SV_InstanceID;uint vertexID:SV_VertexID;");
    replace("row_major float4x4 worldMatrix=World,normalMatrix=NormalWorld;",
        "uint sortedWorld=SortedVertexWorld[input.vertexID+InstanceSettings.y];\n"
        "row_major float4x4 worldMatrix=SortedWorldMatrices[sortedWorld].world,normalMatrix=SortedWorldMatrices[sortedWorld].normal;");
    // Replace only the fixed VS body: declarations and every pixel shader
    // retain their original uniform constants. No new raster interpolators.
    const auto begin=source.find("LegacyOutput FixedVS(");
    const auto end=source.find("float4 coords[8]",begin);
    if(begin==std::string::npos||end==std::string::npos)throw std::runtime_error("material palette VS body anchor changed");
    auto body=source.substr(begin,end-begin);
    for(const auto &field:{std::pair<std::string,std::string>{"MaterialDiffuse","diffuse"},{"MaterialAmbient","ambient"},{"MaterialSpecular","specular"}}){
        std::size_t at=0;while((at=body.find(field.first,at))!=std::string::npos){
            const auto value="SortedWorldMatrices[sortedWorld]."+field.second;
            body.replace(at,field.first.size(),value);at+=value.size();
        }
    }
    source.replace(begin,end-begin,body);
    return source;
}
}
