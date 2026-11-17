// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d8.h>
#include <array>
#include <cstring>
namespace GenZH::Modern {
struct NativeFixedState {
    std::array<D3DMATRIX,11> transforms{}; // world, view, projection, texture0..7
    D3DMATERIAL8 material{};
    D3DVIEWPORT8 viewport{};
    std::array<D3DLIGHT8,4> lights{};
    std::array<BOOL,4> enabled{};
    std::array<bool,4> lightValid{};
    DWORD vertexShader=0,pixelShader=0;
};
class NativeFixedStateMirror {
    NativeFixedState value_;
    bool ready_=false,recording_=false;
    static int Index(D3DTRANSFORMSTATETYPE state){
        if(state==D3DTS_WORLD)return 0;if(state==D3DTS_VIEW)return 1;if(state==D3DTS_PROJECTION)return 2;
        if(state>=D3DTS_TEXTURE0&&state<=D3DTS_TEXTURE7)return 3+static_cast<int>(state-D3DTS_TEXTURE0);
        return -1;
    }
public:
    bool Ready()const{return ready_&&!recording_;}
    bool Recording()const{return recording_;}
    bool SameTransform(D3DTRANSFORMSTATETYPE state,const D3DMATRIX *matrix)const{
        const auto i=Index(state);return Ready()&&matrix&&i>=0&&std::memcmp(&value_.transforms[static_cast<unsigned>(i)],matrix,sizeof(*matrix))==0;
    }
    bool SameMaterial(const D3DMATERIAL8 *material)const{return Ready()&&material&&std::memcmp(&value_.material,material,sizeof(*material))==0;}
    bool SameViewport(const D3DVIEWPORT8 *viewport)const{return Ready()&&viewport&&std::memcmp(&value_.viewport,viewport,sizeof(*viewport))==0;}
    bool SameLight(DWORD index,const D3DLIGHT8 *light)const{return Ready()&&light&&index<4&&value_.lightValid[index]&&std::memcmp(&value_.lights[index],light,sizeof(*light))==0;}
    bool SameLightEnable(DWORD index,BOOL enabled)const{return Ready()&&index<4&&value_.lightValid[index]&&value_.enabled[index]==enabled;}
    const NativeFixedState &Value()const{return value_;}
    void Invalidate(){ready_=false;}
    void Reset(){value_=NativeFixedState{};ready_=recording_=false;}
    void BeginBlock(HRESULT hr){if(SUCCEEDED(hr))recording_=true;}
    void EndBlock(HRESULT hr){if(SUCCEEDED(hr)){recording_=false;Invalidate();}}
    void TransformChanged(D3DTRANSFORMSTATETYPE state,const D3DMATRIX *matrix,HRESULT hr){
        const int index=Index(state);if(Ready()&&SUCCEEDED(hr)&&matrix&&index>=0)value_.transforms[static_cast<unsigned>(index)]=*matrix;
    }
    void MaterialChanged(const D3DMATERIAL8 *material,HRESULT hr){if(Ready()&&SUCCEEDED(hr)&&material)value_.material=*material;}
    void ViewportChanged(const D3DVIEWPORT8 *viewport,HRESULT hr){if(Ready()&&SUCCEEDED(hr)&&viewport)value_.viewport=*viewport;}
    void LightChanged(DWORD index,const D3DLIGHT8 *light,HRESULT hr){
        if(Ready()&&SUCCEEDED(hr)&&light&&index<4){value_.lights[index]=*light;value_.lightValid[index]=true;}
    }
    void LightEnabled(DWORD index,BOOL enabled,HRESULT hr){
        if(Ready()&&SUCCEEDED(hr)&&index<4){
            value_.enabled[index]=enabled;
            // A disabled light was not read during Capture. LightEnable can also
            // create a default native light; reseed before using its values.
            if(enabled&&!value_.lightValid[index])Invalidate();
        }
    }
    void VertexShaderChanged(DWORD shader,HRESULT hr){if(Ready()&&SUCCEEDED(hr))value_.vertexShader=shader;}
    void PixelShaderChanged(DWORD shader,HRESULT hr){if(Ready()&&SUCCEEDED(hr))value_.pixelShader=shader;}
    template<class Native>bool Capture(Native &native){
        if(Ready())return true;if(recording_)return false;
        NativeFixedState next;bool valid=true;
        for(unsigned i=0;i<11;++i){
            const auto state=i==0?D3DTS_WORLD:i==1?D3DTS_VIEW:i==2?D3DTS_PROJECTION:static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0+i-3);
            if(FAILED(native.GetTransform(state,&next.transforms[i])))valid=false;
        }
        if(FAILED(native.GetMaterial(&next.material))||FAILED(native.GetViewport(&next.viewport))||
           FAILED(native.GetVertexShader(&next.vertexShader))||FAILED(native.GetPixelShader(&next.pixelShader)))valid=false;
        for(unsigned i=0;i<4;++i){
            // Undefined lights behave like the existing zero/disabled snapshot.
            native.GetLightEnable(i,&next.enabled[i]);
            if(next.enabled[i])next.lightValid[i]=SUCCEEDED(native.GetLight(i,&next.lights[i]));
        }
        if(!valid){Invalidate();return false;}value_=next;ready_=true;return true;
    }
};
}
