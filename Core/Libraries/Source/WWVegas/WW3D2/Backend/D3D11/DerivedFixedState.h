// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <DirectXMath.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
namespace GenZH::Modern {
// Reforged's unchanged-input/dirty-state principle applied to our legacy/PBR
// snapshot. Only pure derived values are memoized: never texture ownership,
// native validation, per-object wear, shader constants or render eligibility.
class DerivedFixedState {
    template<unsigned N,unsigned M> struct Memo {
        bool valid=false;std::array<float,N> input{};std::array<float,M> output{};
        bool Same(const float *value)const{return valid&&std::memcmp(input.data(),value,N*sizeof(float))==0;}
        void Store(const float *value){std::memcpy(input.data(),value,N*sizeof(float));valid=true;}
    };
    Memo<16,16> world_;Memo<16,3> view_;Memo<1,1> roughness_;
    std::array<Memo<2,2>,4> spots_;
public:
    bool enabled=true;
    std::uint64_t calculations=0,reuses=0;
    void NormalWorld(const float *world,float *out){
        if(enabled&&world_.Same(world)){++reuses;std::memcpy(out,world_.output.data(),64);return;}
        DirectX::XMFLOAT4X4 value;std::memcpy(&value,world,64);
        const auto normal=DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr,DirectX::XMLoadFloat4x4(&value)));
        DirectX::XMStoreFloat4x4(&value,normal);std::memcpy(out,&value,64);++calculations;
        if(enabled){world_.Store(world);std::memcpy(world_.output.data(),out,64);}
    }
    void Camera(const float *view,float *out){
        if(enabled&&view_.Same(view)){++reuses;std::memcpy(out,view_.output.data(),12);return;}
        DirectX::XMFLOAT4X4 value;std::memcpy(&value,view,64);
        DirectX::XMStoreFloat4x4(&value,DirectX::XMMatrixInverse(nullptr,DirectX::XMLoadFloat4x4(&value)));
        out[0]=value._41;out[1]=value._42;out[2]=value._43;++calculations;
        if(enabled){view_.Store(view);std::memcpy(view_.output.data(),out,12);}
    }
    float Roughness(float power){
        if(enabled&&roughness_.Same(&power)){++reuses;return roughness_.output[0];}
        const float value=std::clamp(std::sqrt(2.0f/(std::max(power,0.0f)+2)),.25f,.95f);++calculations;
        if(enabled){roughness_.Store(&power);roughness_.output[0]=value;}return value;
    }
    void Spot(unsigned index,float theta,float phi,float *out){
        const float input[]={theta,phi};auto &memo=spots_[index];
        if(enabled&&memo.Same(input)){++reuses;std::memcpy(out,memo.output.data(),8);return;}
        out[0]=std::cos(theta*.5f);out[1]=std::cos(phi*.5f);++calculations;
        if(enabled){memo.Store(input);std::memcpy(memo.output.data(),out,8);}
    }
};
}
