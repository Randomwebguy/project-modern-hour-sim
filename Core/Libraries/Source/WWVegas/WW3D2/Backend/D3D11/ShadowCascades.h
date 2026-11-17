// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <limits>

namespace GenZH::Modern {
// b2 retains the first 80 bytes of the previous single-map interface.
struct ShadowConstants {
    float nearVP[16]{},nearSettings[4]{};
    float farVP[16]{},farSettings[4]{};
    float cameraDepth[4]{},split[4]{},render[4]{},sun[4]{};
};
static_assert(sizeof(ShadowConstants)==224);
inline ShadowConstants ShadowConstantsForPass(const ShadowConstants &scene,bool shadowPass,bool overlay,bool offscreen){
    auto result=scene;result.render[1]=shadowPass?1.0f:0.0f; // Shared terrain shaders select their output projection.
    if(!shadowPass&&(overlay||offscreen))result.nearSettings[3]=0;
    return result;
}
struct ShadowCascadePlan {
    ShadowConstants constants;
    std::array<float,2> radius{},texel{};
    bool valid=false;
};
inline bool ShadowFinite(DirectX::FXMVECTOR v){
    return !DirectX::XMVector4IsNaN(v)&&!DirectX::XMVector4IsInfinite(v);
}
inline ShadowCascadePlan PlanShadowCascades(const float *cameraView,const float *cameraProjection,const float *sun, unsigned size=2048,float splitFraction=.35f){
    using namespace DirectX;
    ShadowCascadePlan result;
    if(!size)return result;
    XMFLOAT4X4 v,p;std::memcpy(&v,cameraView,64);std::memcpy(&p,cameraProjection,64);
    auto view=XMLoadFloat4x4(&v),projection=XMLoadFloat4x4(&p);
    XMVECTOR determinant;auto inverse=XMMatrixInverse(&determinant,view*projection);
    if(!ShadowFinite(determinant)||std::abs(XMVectorGetX(determinant))<1e-12f)return result;
    std::array<XMVECTOR,4> nearCorners,farCorners;
    for(unsigned i=0;i<4;++i){
        const float x=i&1?1.0f:-1.0f,y=i&2?1.0f:-1.0f;
        nearCorners[i]=XMVector3TransformCoord(XMVectorSet(x,y,0,1),inverse);
        farCorners[i]=XMVector3TransformCoord(XMVectorSet(x,y,1,1),inverse);
        if(!ShadowFinite(nearCorners[i])||!ShadowFinite(farCorners[i]))return result;
    }
    const float nativeNear=XMVectorGetZ(XMVector3TransformCoord(nearCorners[0],view));
    const float nativeFar=XMVectorGetZ(XMVector3TransformCoord(farCorners[0],view));
    // Native Zero Hour uses RH view depth. Cascades use positive forward
    // distance for splits, but interpolate the unchanged world-space corners.
    const float forward=nativeFar>=nativeNear?1.0f:-1.0f;
    const float nearZ=nativeNear*forward,farZ=nativeFar*forward;
    if(!std::isfinite(nearZ)||!std::isfinite(farZ)||nearZ<0||farZ<=nearZ+.01f)return result;
    // Bounded sun-shadow distance; do not fetch undisclosed native objects.
    // Camera changes, never caster births/motion, determine cascade footprints.
    auto eye=XMVector3TransformCoord(XMVectorZero(),XMMatrixInverse(nullptr,view));
    if(!ShadowFinite(eye))return result;
    const float distance=std::min(farZ,std::max(2048.0f,std::min(4096.0f,std::abs(XMVectorGetZ(eye))*4)));
    if(distance<=nearZ+.01f)return result;
    const float split=nearZ+(distance-nearZ)*splitFraction,blend=(split-nearZ)*.08f;
    auto direction=XMVectorSet(sun[0],sun[1],sun[2],0);
    if(!ShadowFinite(direction)||XMVectorGetX(XMVector3LengthSq(direction))<1e-8f)return result;
    direction=XMVector3Normalize(direction);
    const auto up=std::abs(XMVectorGetZ(direction))>.95f?XMVectorSet(0,1,0,0):XMVectorSet(0,0,1,0);
    // A world-fixed light basis is essential: snapping in a light frame that
    // translates with the caster AABB did not stabilize world-space shadows.
    const auto lightView=XMMatrixLookToLH(XMVectorZero(),XMVectorNegate(direction),up);
    for(unsigned cascade=0;cascade<2;++cascade){
        const float start=cascade?split-blend:nearZ,end=cascade?distance:split+blend;
        std::array<XMVECTOR,8> corners;auto center=XMVectorZero();
        for(unsigned i=0;i<4;++i){
            corners[i]=XMVectorLerp(nearCorners[i],farCorners[i],(start-nearZ)/(farZ-nearZ));
            corners[i+4]=XMVectorLerp(nearCorners[i],farCorners[i],(end-nearZ)/(farZ-nearZ));
            center=XMVectorAdd(center,XMVectorAdd(corners[i],corners[i+4]));
        }
        center=XMVectorScale(center,.125f);float radius=0;
        for(auto corner:corners)radius=std::max(radius,XMVectorGetX(XMVector3Length(XMVectorSubtract(corner,center))));
        if(!std::isfinite(radius)||radius>16384)return result;
        radius=std::ceil(std::max(radius,32.0f)/32)*32+16;
        const float texel=radius*2/static_cast<float>(size);
        XMFLOAT3 lightCenter;XMStoreFloat3(&lightCenter,XMVector3TransformCoord(center,lightView));
        const float cx=std::round(lightCenter.x/texel)*texel,cy=std::round(lightCenter.y/texel)*texel;
        const float cz=std::round(lightCenter.z/1024)*1024;
        const float depth=std::ceil(std::max(radius+2048,4096.0f)/512)*512;
        const auto lightProjection=XMMatrixOrthographicOffCenterLH(cx-radius,cx+radius,cy-radius,cy+radius,cz-depth,cz+depth);
        XMFLOAT4X4 vp;XMStoreFloat4x4(&vp,lightView*lightProjection);
        float *matrix=cascade?result.constants.farVP:result.constants.nearVP;
        float *settings=cascade?result.constants.farSettings:result.constants.nearSettings;
        std::memcpy(matrix,&vp,64);settings[0]=.35f/(depth*2);settings[1]=settings[2]=1.0f/static_cast<float>(size);settings[3]=1;
        result.radius[cascade]=radius;result.texel[cascade]=texel;
    }
    result.constants.cameraDepth[0]=v._13*forward;result.constants.cameraDepth[1]=v._23*forward;
    result.constants.cameraDepth[2]=v._33*forward;result.constants.cameraDepth[3]=v._43*forward;
    result.constants.split[0]=split;result.constants.split[1]=blend;result.constants.split[2]=distance;result.constants.split[3]=2;
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(result.constants.sun),direction);
    result.valid=true;return result;
}
inline bool ShadowBoundsIntersect(const std::array<DirectX::XMFLOAT3,8> &corners,const float *vp){
    using namespace DirectX;XMFLOAT4X4 m;std::memcpy(&m,vp,64);
    const auto matrix=XMLoadFloat4x4(&m);XMFLOAT3 low{FLT_MAX,FLT_MAX,FLT_MAX},high{-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for(const auto &corner:corners){auto p=XMVector3TransformCoord(XMLoadFloat3(&corner),matrix);
        if(!ShadowFinite(p))return true; // Invalid bounds retain the safe draw path.
        XMFLOAT3 f;XMStoreFloat3(&f,p);low.x=std::min(low.x,f.x);low.y=std::min(low.y,f.y);low.z=std::min(low.z,f.z);
        high.x=std::max(high.x,f.x);high.y=std::max(high.y,f.y);high.z=std::max(high.z,f.z);
    }
    return high.x>=-1&&low.x<=1&&high.y>=-1&&low.y<=1&&high.z>=0&&low.z<=1;
}
}
