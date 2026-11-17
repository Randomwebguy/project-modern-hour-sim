// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GpuFrameTimer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
namespace GenZH::Modern {
// The original D24S8 precision/stencil remain unchanged. Views share one
// allocation; the optional hierarchy is created only when a world frame needs it.
class SceneDepthGPU {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11Texture2D> depth_,hierarchy_;
    Ptr<ID3D11DepthStencilView> writable_,readOnly_;
    Ptr<ID3D11ShaderResourceView> depthView_,hierarchyView_;
    std::vector<Ptr<ID3D11ShaderResourceView>> mipViews_;
    std::vector<Ptr<ID3D11UnorderedAccessView>> mipOutputs_;
    Ptr<ID3D11ComputeShader> copy_,reduce_;
    Ptr<ID3D11Buffer> constants_;
    UINT width_=0,height_=0;
    bool valid_=false;
    GpuFrameTimer timer_;
    HRESULT CreateHierarchy();
public:
    struct Stats {std::uint64_t builds=0,dispatches=0,gpuSamples=0;double cpuMs=0,gpuMs=0;} stats;
    HRESULT Create(ID3D11Device *device,UINT width,UINT height);
    HRESULT Build(ID3D11DeviceContext *context);
    void Reset();
    void Invalidate(){valid_=false;}
    // Reserved post-effects slots. Unbind before restoring writable depth.
    static void Unbind(ID3D11DeviceContext *context);
    ID3D11DepthStencilView *Writable()const{return writable_.Get();}
    ID3D11DepthStencilView *ReadOnly()const{return readOnly_.Get();}
    ID3D11ShaderResourceView *DepthView()const{return depthView_.Get();}
    ID3D11ShaderResourceView *HierarchyView()const{return valid_?hierarchyView_.Get():nullptr;}
    UINT Width()const{return width_;} UINT Height()const{return height_;}
    UINT Mips()const{return static_cast<UINT>(mipViews_.size());}
    std::uint64_t HierarchyBytes()const;
};
}
