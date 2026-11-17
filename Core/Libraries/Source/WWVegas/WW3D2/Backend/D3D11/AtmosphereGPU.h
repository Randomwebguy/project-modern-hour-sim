// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ShadowCascades.h"
#include "WeatherState.h"
#include "WeatherProbeState.h"
#include "CloudHistory.h"
#include "LocalVolumetricLights.h"
#include "LocalLightShadows.h"
#include "GpuFrameTimer.h"
#include "ShroudMapping.h"
#include "ScenePipelineBindings.h"
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>
namespace GenZH::Modern {
using Microsoft::WRL::ComPtr;
struct AtmosphereConstants {
 float inverseVP[16]{},viewProjection[16]{};
 float camera[4]{},sun[4]{},sunColor[4]{},weather[4]{},wind[4]{},range[4]{},shadowArea[4]{},viewDepth[4]{},ambient[4]{},flashPosition[4]{},flashRadiance[4]{};
};
static_assert(sizeof(AtmosphereConstants)==304);
class AtmosphereGPU {
 ComPtr<ID3D11Buffer> constants_,shadows_,rainDepthConstants_;
 ComPtr<ID3D11ShaderResourceView> noise_,volume_,cloudShadow_,sky_;
 ComPtr<ID3D11UnorderedAccessView> volumeUAV_,cloudUAV_;
 ComPtr<ID3D11RenderTargetView> skyTarget_;
 struct SkyBank {ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11RenderTargetView> target;} skyHistory_[2];
 ComPtr<ID3D11Buffer> skyHistoryConstants_;
 ComPtr<ID3D11PixelShader> temporalPS_;
 CloudHistoryState skyHistoryState_;unsigned currentSky_=0;
 ComPtr<ID3D11Buffer> localLightConstants_;
 ComPtr<ID3D11ShaderResourceView> localClusters_;
 ComPtr<ID3D11UnorderedAccessView> localClustersUAV_;
 ComPtr<ID3D11ComputeShader> localClusterCS_;
 LocalVolumeConstants localLights_{};
 ComPtr<ID3D11Buffer> localShadowConstants_;
 ComPtr<ID3D11ShaderResourceView> localShadowAtlas_;
 LocalShadowConstants localShadowData_{};
 ComPtr<ID3D11ComputeShader> volumeCS_,cloudCS_;
 ComPtr<ID3D11VertexShader> quadVS_,rainVS_;
 ComPtr<ID3D11PixelShader> skyPS_,resolvePS_,rainPS_;
 ComPtr<ID3D11SamplerState> wrap_,clamp_,comparison_;
 ComPtr<ID3D11DepthStencilState> noDepth_,rainDepth_;
 ComPtr<ID3D11RasterizerState> raster_;
 ComPtr<ID3D11BlendState> opaque_,rainBlend_;
 struct ProbeBank {
  ComPtr<ID3D11ShaderResourceView> reflection,diffuse;
  std::array<ComPtr<ID3D11UnorderedAccessView>,7> mips;
  ComPtr<ID3D11UnorderedAccessView> diffuseUAV;
 } probes_[2];
 ComPtr<ID3D11ShaderResourceView> rawSky_;
 ComPtr<ID3D11UnorderedAccessView> rawSkyUAV_;
 ComPtr<ID3D11ComputeShader> skyProbeCS_,specularCS_,diffuseCS_;
 ComPtr<ID3D11Buffer> probeFilter_;
 WeatherProbeState probeState_;unsigned currentProbe_=0,previousProbe_=0;
 GpuFrameTimer probeTimer_;
 HRESULT CreateProbes(ID3D11Device *device);
 void PrepareProbes(ID3D11DeviceContext *context);
 WeatherController controller_;AtmosphereConstants data_{};
 float view_[16]{},projection_[16]{};
 D3D11_VIEWPORT viewport_{};
 UINT skyWidth_=0,skyHeight_=0;
 bool ready_=false,ticked_=false,skyDrawn_=false,diagnosticCloudSky_=false;
 std::uint64_t mapEpoch_=0;
 std::uint64_t flashEvent_=0;float flashAnchor_[3]{};bool skyFlash_=false;
 GpuFrameTimer timer_;
 // perf92 volumetric fog of war (PSShroudFog).
 ComPtr<ID3D11Buffer> shroudConstants_;ComPtr<ID3D11PixelShader> shroudPS_;ComPtr<ID3D11BlendState> shroudBlend_,shroudFlatBlend_;
 GpuFrameTimer shroudTimer_;
 // perf93 faded per-cell vision, in the native shroud texture layout (cell i at texel i+1).
 ComPtr<ID3D11Texture2D> visionTarget_;ComPtr<ID3D11ShaderResourceView> visionTargetView_;
 struct VisionBank {ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11RenderTargetView> target;} vision_[2];
 ComPtr<ID3D11PixelShader> shroudFadePS_;ComPtr<ID3D11Buffer> shroudFadeConstants_;
 std::vector<std::uint8_t> visionUpload_;UINT visionWidth_=0,visionHeight_=0;unsigned visionCurrent_=0;std::uint64_t visionRevision_=0;bool visionValid_=false;
public:
 struct Stats {std::uint64_t preparations=0,cloudPasses=0,volumePasses=0,skyPasses=0,rainPasses=0,binds=0,gpuSamples=0,probeUpdates=0,probeGpuSamples=0,cloudHistoryPasses=0,cloudHistoryResets=0,localLightSources=0,localClusterPasses=0,localSourceRejected=0,localSourceOverflow=0,localSourceDuplicates=0,flashFrames=0,flashEvents=0,shroudPasses=0,shroudGpuSamples=0;double cpuMs=0,gpuMs=0,probeGpuMs=0,shroudGpuMs=0;} stats;
 void Reset();
 HRESULT Create(ID3D11Device *device,UINT width,UINT height,WeatherMode mode,bool diagnosticCloudSky=false);
 void BeginFrame(){if(!skyDrawn_)skyHistoryState_.Invalidate();localLights_={};localShadowData_={};localShadowAtlas_.Reset();ready_=false;ticked_=false;skyDrawn_=false;}
 void SetLocalShadows(const LocalShadowConstants &data,ID3D11ShaderResourceView *atlas){localShadowData_=data;localShadowAtlas_=atlas;if(!atlas)localShadowData_={};}
 void SetLocalSources(const LocalVolumeSources &sources){localLights_=sources.data;localLights_.metadata[0]=(std::min)(localLights_.metadata[0],LocalVolumeLightLimit);stats.localSourceRejected+=sources.rejected;stats.localSourceOverflow+=sources.overflow;stats.localSourceDuplicates+=sources.duplicates;}
 void SetClimate(const WeatherClimate &climate,bool nativeSnow,std::uint64_t mapEpoch){
  if(mapEpoch_!=mapEpoch){controller_.ResetWorld();probeState_=WeatherProbeState{};skyHistoryState_.Invalidate();mapEpoch_=mapEpoch;flashEvent_=0;skyFlash_=false;ready_=false;ticked_=false;skyDrawn_=false;}
  controller_.SetClimate(climate.kind,nativeSnow);
 }
 HRESULT Prepare(ID3D11DeviceContext *context,const float *view,const float *projection,const float *sun,const float *sunColor,
                 const ShadowConstants &shadows,ID3D11ShaderResourceView *shadowMap,const D3D11_VIEWPORT &viewport,double delta,const float *ambient=nullptr);
 bool Matches(const float *view,const float *projection,const D3D11_VIEWPORT &viewport)const;
 void Bind(ID3D11DeviceContext *context,bool enabled,ScenePipelineBindings *cache=nullptr);
 void DrawSky(ID3D11DeviceContext *context,ID3D11RenderTargetView *scene);
 void DrawRain(ID3D11DeviceContext *context,ID3D11RenderTargetView *scene,ID3D11DepthStencilView *depth,
               ID3D11ShaderResourceView *sceneDepth=nullptr,ID3D11DepthStencilView *readOnlyDepth=nullptr);
 // Full-viewport fog of war over the prepared world view; scene depth is read
 // as a texture (no depth target bound). false: nothing drawn.
 // vision (optional): the faded vision texture replaces the native levels as cover.
 bool DrawShroudFog(ID3D11DeviceContext *context,ID3D11RenderTargetView *scene,ID3D11ShaderResourceView *sceneDepth,
                    ID3D11ShaderResourceView *shroud,const D3D11ShroudMapping &mapping,bool flat,ID3D11ShaderResourceView *vision=nullptr);
 // Uploads changed cell states (0 visible, 128 out of sight, 255 unexplored) and
 // moves the vision texture toward them: fog clears in ~0.4 s, returns in ~1.5 s.
 bool UpdateShroudVision(ID3D11DeviceContext *context,const std::uint8_t *cells,UINT cellsX,UINT cellsY,std::uint8_t border,std::uint64_t revision,
                         UINT textureWidth,UINT textureHeight,double seconds);
 void ResetShroudVision(){visionValid_=false;}
 ID3D11ShaderResourceView *ShroudVision()const{return visionValid_?vision_[visionCurrent_].view.Get():nullptr;}
 bool Ready()const{return ready_;}
 const AtmosphereConstants &Data()const{return data_;}
 ID3D11ShaderResourceView *Volume()const{return volume_.Get();}
 ID3D11ShaderResourceView *NoiseVolume()const{return noise_.Get();}
 ID3D11ShaderResourceView *SkyHistory()const{return skyHistory_[currentSky_].view.Get();}
 ID3D11ShaderResourceView *RawSky()const{return sky_.Get();}
 ID3D11ShaderResourceView *LocalClusters()const{return localClusters_.Get();}
 ID3D11ShaderResourceView *CloudShadow()const{return cloudShadow_.Get();}
 ID3D11ShaderResourceView *Reflection()const{return probes_[currentProbe_].reflection.Get();}
 ID3D11ShaderResourceView *Diffuse()const{return probes_[currentProbe_].diffuse.Get();}
 bool CloudDiagnostic()const{return diagnosticCloudSky_;}
 bool ProbeReady()const{return probeState_.Ready();}
 bool Available()const{return bool(constants_);}
 std::uint64_t PayloadBytes()const{return Available()?FixedBytes+std::uint64_t(skyWidth_)*skyHeight_*8*3:0;}
 static constexpr std::uint64_t ProbeBytes=64ull*64*6*8+5461ull*6*8*2+16ull*16*6*8*2;
 static constexpr std::uint64_t FixedBytes=160ull*90*64*8+256ull*256*2+4793490ull+ProbeBytes+LocalVolumeClusters*16ull+sizeof(LocalVolumeConstants)+sizeof(LocalShadowConstants);
};
}
