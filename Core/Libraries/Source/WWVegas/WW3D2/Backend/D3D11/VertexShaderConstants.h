// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstring>
#include <memory>
namespace GenZH::Modern {
// perf90: the 1.5 KB vertex-shader constant block of a draw packet exists only
// for packets with a vertex shader. Fixed-function packets (nearly all) no
// longer zero, move and destroy it; a packet without a block reads as all
// zeros, exactly as the former zero-initialized float[96][4] member.
class VertexShaderConstants {
    std::unique_ptr<float[]> data_;
public:
    static constexpr std::size_t Floats=96*4,Bytes=Floats*sizeof(float);
    VertexShaderConstants()=default;
    VertexShaderConstants(const VertexShaderConstants &other){if(other.data_){data_=std::make_unique<float[]>(Floats);std::memcpy(data_.get(),other.data_.get(),Bytes);}}
    VertexShaderConstants &operator=(const VertexShaderConstants &other){
        if(this!=&other){if(other.data_){if(!data_)data_=std::make_unique<float[]>(Floats);std::memcpy(data_.get(),other.data_.get(),Bytes);}else data_.reset();}
        return *this;
    }
    VertexShaderConstants(VertexShaderConstants&&)noexcept=default;
    VertexShaderConstants &operator=(VertexShaderConstants&&)noexcept=default;
    float *Write(){if(!data_)data_=std::make_unique<float[]>(Floats);return data_.get();} // zeroed on creation
    const float *Read()const{static const float zeros[Floats]{};return data_?data_.get():zeros;}
    bool operator==(const VertexShaderConstants &other)const{return !std::memcmp(Read(),other.Read(),Bytes);}
    bool operator!=(const VertexShaderConstants &other)const{return !(*this==other);}
};
}
