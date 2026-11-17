// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cassert>
#include "XeGTAO.h"
#include "ScreenMaterialGPU.h"
#include "GpuFrameTimer.h"
#include <vector>
namespace GenZH::Modern {
class GTAOGPU {
 template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
 Ptr<ID3D11Device> device_;
 Ptr<ID3D11Buffer> constants_;
 Ptr<ID3D11ComputeShader> depthCS_,reduceCS_,mainCS_,denoiseCS_;
 Ptr<ID3D11VertexShader> compositeVS_;
 Ptr<ID3D11PixelShader> compositePS_;
 Ptr<ID3D11SamplerState> sampler_;
 Ptr<ID3D11BlendState> blend_;
 Ptr<ID3D11DepthStencilState> noDepth_;
 Ptr<ID3D11RasterizerState> raster_;
 Ptr<ID3D11ShaderResourceView> depthView_,edgesView_,colorView_;
 Ptr<ID3D11UnorderedAccessView> edgesOutput_;
 Ptr<ID3D11RenderTargetView> colorTarget_;
 std::vector<Ptr<ID3D11ShaderResourceView>> depthMips_;
 std::vector<Ptr<ID3D11UnorderedAccessView>> depthOutputs_;
 std::array<Ptr<ID3D11ShaderResourceView>,2> aoViews_;
 std::array<Ptr<ID3D11UnorderedAccessView>,2> aoOutputs_;
 UINT width_=0,height_=0;bool valid_=false,colorOwned_=false;float radius_=8,strength_=1,contactStrength_=0;
 GpuFrameTimer timer_;
public:
 struct Constants {
  XeGTAO::GTAOConstants ao{};
  ScreenMaterialGPU::Camera camera{};
  UINT filterDimensions[4]{};
  float tuning[4]{};
  float contact[4]{}; // strength, maximum ray distance, base depth thickness, normal bias
 };
 struct Stats {std::uint64_t frames=0,dispatches=0,unsupported=0,gpuSamples=0,contactFrames=0;double cpuMs=0,gpuMs=0;} stats;
 HRESULT Create(ID3D11Device *device,UINT width,UINT height,float radius=8,float strength=1,float contactStrength=0);
 HRESULT Apply(ID3D11DeviceContext *context,const ScreenMaterialGPU &materials,ID3D11ShaderResourceView *depth,ID3D11ShaderResourceView *color);
 static bool PrepareConstants(const ScreenMaterialGPU::Camera &camera,UINT width,UINT height,float radius,float strength,Constants &out);
 void Reset(){*this=GTAOGPU{};}
 bool Available()const{return bool(constants_);}
 bool Valid()const{return valid_;}
 void BeginFrame(){valid_=colorOwned_=false;}
 std::uint64_t Bytes()const;
 ID3D11ShaderResourceView *AOView()const{return valid_&&strength_>0?aoViews_[0].Get():nullptr;}
 ID3D11ShaderResourceView *DepthView()const{return valid_&&strength_>0?depthView_.Get():nullptr;}
 ID3D11ShaderResourceView *ColorView()const{return valid_&&colorOwned_?colorView_.Get():nullptr;}
 void SwapColor(Ptr<ID3D11RenderTargetView> &target,Ptr<ID3D11ShaderResourceView> &view){if(valid_&&colorOwned_){std::swap(target,colorTarget_);std::swap(view,colorView_);colorOwned_=false;}}
};
static_assert(sizeof(XeGTAO::GTAOConstants)==96);
static_assert(sizeof(ScreenMaterialGPU::Camera)==256);
static_assert(sizeof(GTAOGPU::Constants)==400);
}
