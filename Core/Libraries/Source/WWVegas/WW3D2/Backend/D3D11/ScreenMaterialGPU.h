// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdint>
namespace GenZH::Modern {
// Current visible primary-camera data only. No hidden objects, CPU texture
// sampling, reduced authored maps or extra geometry passes.
class ScreenMaterialGPU {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    std::array<Ptr<ID3D11RenderTargetView>,7> targets_;
    std::array<Ptr<ID3D11ShaderResourceView>,7> views_;
    Ptr<ID3D11Buffer> control_;
    UINT width_=0,height_=0;bool cameraReady_=false,cleared_=false;
    int lastControl_=-1;
    float view_[16]{},projection_[16]{},depthRange_[2]{};
public:
    struct Camera {float inverseVP[16]{},viewMatrix[16]{},projectionMatrix[16]{},viewport[4]{},depthRange[4]{},eye[4]{},sunDirection[4]{};} camera;
    struct Stats {std::uint64_t primaryDraws=0,excludedDraws=0,uploads=0,skips=0,clears=0;} stats;
    enum Output:unsigned {DiffuseIndirect,SpecularIndirect,NormalOcclusion,DirectionalDirect,SpecularResponse,MotionVector,Albedo};
    void Reset(){*this=ScreenMaterialGPU{};}
    bool Available()const{return bool(control_);}
    void BeginFrame(){cameraReady_=cleared_=false;lastControl_=-1;std::memset(camera.sunDirection,0,sizeof(camera.sunDirection));}
    bool CameraReady()const{return cameraReady_;}
    std::uint64_t Bytes()const{return Available()?std::uint64_t(width_)*height_*56+16:0;}
    ID3D11ShaderResourceView *View(Output output)const{return Available()&&cleared_?views_[output].Get():nullptr;}
    static void UnbindInputs(ID3D11DeviceContext *c){if(c){ID3D11ShaderResourceView *empty[5]{};c->PSSetShaderResources(39,5,empty);c->CSSetShaderResources(39,5,empty);}}
    static UINT AuxiliaryWriteMask(UINT colorMask,bool depthWrite){return ((colorMask&7u)||depthWrite)?D3D11_COLOR_WRITE_ENABLE_ALL:0;}
    HRESULT Create(ID3D11Device *d,UINT width,UINT height){
        if(!d||!width||!height||width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)return E_INVALIDARG;
        ScreenMaterialGPU next;next.width_=width;next.height_=height;HRESULT hr;
        D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        for(unsigned i=0;i<7;++i){Ptr<ID3D11Texture2D> texture;if(FAILED(hr=d->CreateTexture2D(&td,nullptr,texture.GetAddressOf()))||FAILED(hr=d->CreateRenderTargetView(texture.Get(),nullptr,next.targets_[i].GetAddressOf()))||FAILED(hr=d->CreateShaderResourceView(texture.Get(),nullptr,next.views_[i].GetAddressOf())))return hr;}
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;const float zero[4]{};D3D11_SUBRESOURCE_DATA data{zero,0,0};
        if(FAILED(hr=d->CreateBuffer(&bd,&data,next.control_.GetAddressOf())))return hr;
        *this=std::move(next);return S_OK;
    }
    bool SelectCamera(const float *view,const float *projection,const float *viewport,const float *depthRange){
        if(!Available()||!view||!projection||!viewport||!depthRange)return false;
        if(cameraReady_)return Matches(view,projection,viewport,depthRange);
        for(unsigned i=0;i<16;++i)if(!std::isfinite(view[i])||!std::isfinite(projection[i]))return false;
        for(unsigned i=0;i<4;++i)if(!std::isfinite(viewport[i]))return false;
        if(viewport[0]<0||viewport[1]<0||viewport[2]<=0||viewport[3]<=0||viewport[0]+viewport[2]>width_||viewport[1]+viewport[3]>height_||!std::isfinite(depthRange[0])||!std::isfinite(depthRange[1])||depthRange[0]<0||depthRange[1]>1||depthRange[1]<=depthRange[0])return false;
        DirectX::XMFLOAT4X4 v,p;std::memcpy(&v,view,64);std::memcpy(&p,projection,64);DirectX::XMVECTOR determinant;const auto inverse=DirectX::XMMatrixInverse(&determinant,DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&v),DirectX::XMLoadFloat4x4(&p)));
        if(std::abs(DirectX::XMVectorGetX(determinant))<1e-12f)return false;DirectX::XMFLOAT4X4 iv;DirectX::XMStoreFloat4x4(&iv,inverse);std::memcpy(camera.inverseVP,&iv,64);
        for(float value:camera.inverseVP)if(!std::isfinite(value))return false;
        DirectX::XMFLOAT4X4 inverseView;DirectX::XMStoreFloat4x4(&inverseView,DirectX::XMMatrixInverse(nullptr,DirectX::XMLoadFloat4x4(&v)));
        std::memcpy(camera.viewMatrix,view,64);std::memcpy(camera.projectionMatrix,projection,64);camera.eye[0]=inverseView._41;camera.eye[1]=inverseView._42;camera.eye[2]=inverseView._43;camera.eye[3]=1;
        std::memcpy(view_,view,64);std::memcpy(projection_,projection,64);std::memcpy(camera.viewport,viewport,16);std::memcpy(depthRange_,depthRange,8);std::memcpy(camera.depthRange,depthRange,8);cameraReady_=true;return true;
    }
    bool Matches(const float *view,const float *projection,const float *viewport,const float *depthRange)const{return cameraReady_&&view&&projection&&viewport&&depthRange&&!std::memcmp(view_,view,64)&&!std::memcmp(projection_,projection,64)&&!std::memcmp(camera.viewport,viewport,16)&&!std::memcmp(depthRange_,depthRange,8);}
    void BindTargets(ID3D11DeviceContext *c,ID3D11RenderTargetView *scene,ID3D11DepthStencilView *depth){
        if(!Available()||!c||!scene||!depth)return;UnbindInputs(c);
        if(!cleared_){const float zero[4]{};for(const auto &target:targets_)c->ClearRenderTargetView(target.Get(),zero);cleared_=true;++stats.clears;}
        ID3D11RenderTargetView *targets[8]={scene,targets_[0].Get(),targets_[1].Get(),targets_[2].Get(),targets_[3].Get(),targets_[4].Get(),targets_[5].Get(),targets_[6].Get()};c->OMSetRenderTargets(8,targets,depth);
    }
    bool SelectSun(const float positions[4][4],const float directions[4][4]){
        if(!cameraReady_||!positions||!directions)return false;
        for(unsigned i=0;i<4;++i)if(positions[i][3]>2.5f){
            float length=0;for(unsigned k=0;k<3;++k)length+=directions[i][k]*directions[i][k];
            if(!std::isfinite(length)||length<1e-8f)continue;
            for(unsigned k=0;k<3;++k)camera.sunDirection[k]=-directions[i][k]/std::sqrt(length);
            camera.sunDirection[3]=1;return true;
        }
        return false;
    }
    unsigned SunMask(const float positions[4][4],const float directions[4][4])const{
        if(!cameraReady_||camera.sunDirection[3]!=1||!positions||!directions)return 0;
        unsigned mask=0;
        for(unsigned i=0;i<4;++i)if(positions[i][3]>2.5f){
            float length=0,dot=0;for(unsigned k=0;k<3;++k){length+=directions[i][k]*directions[i][k];dot-=directions[i][k]*camera.sunDirection[k];}
            if(std::isfinite(length)&&length>1e-8f&&dot/std::sqrt(length)>.99999f)mask|=1u<<i;
        }
        return mask;
    }
    void BindControl(ID3D11DeviceContext *c,bool enabled,unsigned sunMask=15){
        if(!c)return;enabled=enabled&&Available()&&cameraReady_;
        if(Available()){const int flag=enabled?1+int((sunMask&15u)<<1):0;if(lastControl_!=flag){const float values[4]={enabled?1.f:0.f,enabled?float(sunMask&15u):0.f,0,0};c->UpdateSubresource(control_.Get(),0,nullptr,values,0,0);lastControl_=flag;++stats.uploads;}else ++stats.skips;}
        auto *buffer=Available()?control_.Get():nullptr;c->PSSetConstantBuffers(13,1,&buffer);if(enabled)++stats.primaryDraws;else ++stats.excludedDraws;
    }
};
}
