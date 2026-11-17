// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../GenZHRenderStates.h"
#include "ScreenMaterialGate.h"
#include "ShaderStageLinkage.h"
#include "TerrainCompactShaderSource.h"
#include "OpaqueShadowShaderSource.h"
#include "OpaqueShadowPolicy.h"
#include "CompatibilityRenderer.h"
#include "SortedColorPalette.h"
#include "SortedRunTemplates.h"
#include "SortedPaletteShaderSource.h"
#include "LegacyShaderSource.h"
#include "DeviceBridge.h"
#include "NativeImage.h"
#include "NativeDDS.h"
#include "DrawGrouping.h"
#include "SceneVisibility.h"
#include "SortedPacketBatch.h"
#include "ShadowReuseState.h"
#include "ShaderBytecodeCache.h"
#include "ProgramSource.h"
#include "AtmosphereDrawPolicy.h"
#include "CaptureWorkTelemetry.h"
#include "StableDepthSort.h"
#include <d3dx8.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <DirectXPackedVector.h>
#include <filesystem>
#include <limits>
#include <cfloat>
#include <fstream>
#include <iomanip>
#include <set>
#include <cctype>

namespace GenZH::Modern {
namespace {
float AsFloat(DWORD bits){float value;std::memcpy(&value,&bits,4);return value;}
void PackedColor(DWORD value,float *rgba){
 rgba[0]=static_cast<float>((value>>16)&255)/255;rgba[1]=static_cast<float>((value>>8)&255)/255;
 rgba[2]=static_cast<float>(value&255)/255;rgba[3]=static_cast<float>((value>>24)&255)/255;
}
D3D11_BLEND Blend(DWORD value){
 switch(value){
 case D3DBLEND_ZERO:return D3D11_BLEND_ZERO;case D3DBLEND_ONE:return D3D11_BLEND_ONE;
 case D3DBLEND_SRCCOLOR:return D3D11_BLEND_SRC_COLOR;case D3DBLEND_INVSRCCOLOR:return D3D11_BLEND_INV_SRC_COLOR;
 case D3DBLEND_SRCALPHA:return D3D11_BLEND_SRC_ALPHA;case D3DBLEND_INVSRCALPHA:return D3D11_BLEND_INV_SRC_ALPHA;
 case D3DBLEND_DESTALPHA:return D3D11_BLEND_DEST_ALPHA;case D3DBLEND_INVDESTALPHA:return D3D11_BLEND_INV_DEST_ALPHA;
 case D3DBLEND_DESTCOLOR:return D3D11_BLEND_DEST_COLOR;case D3DBLEND_INVDESTCOLOR:return D3D11_BLEND_INV_DEST_COLOR;
 case D3DBLEND_SRCALPHASAT:return D3D11_BLEND_SRC_ALPHA_SAT;default:return D3D11_BLEND_ONE;
 }
}
D3D11_BLEND AlphaBlend(D3D11_BLEND value){
 switch(value){
 case D3D11_BLEND_SRC_COLOR:return D3D11_BLEND_SRC_ALPHA;
 case D3D11_BLEND_INV_SRC_COLOR:return D3D11_BLEND_INV_SRC_ALPHA;
 case D3D11_BLEND_DEST_COLOR:return D3D11_BLEND_DEST_ALPHA;
 case D3D11_BLEND_INV_DEST_COLOR:return D3D11_BLEND_INV_DEST_ALPHA;
 case D3D11_BLEND_SRC_ALPHA_SAT:return D3D11_BLEND_ONE;
 default:return value;
 }
}
D3D11_TEXTURE_ADDRESS_MODE Address(DWORD value){
 switch(value){case D3DTADDRESS_WRAP:return D3D11_TEXTURE_ADDRESS_WRAP;case D3DTADDRESS_MIRROR:return D3D11_TEXTURE_ADDRESS_MIRROR;
 case D3DTADDRESS_CLAMP:return D3D11_TEXTURE_ADDRESS_CLAMP;case D3DTADDRESS_BORDER:return D3D11_TEXTURE_ADDRESS_BORDER;
 case D3DTADDRESS_MIRRORONCE:return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;default:return D3D11_TEXTURE_ADDRESS_WRAP;}
}
HRESULT DynamicBuffer(ID3D11Device *device,UINT bytes,UINT bind,ComPtr<ID3D11Buffer> &buffer){
 D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.BindFlags=bind;desc.Usage=D3D11_USAGE_DYNAMIC;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
 return device->CreateBuffer(&desc,nullptr,buffer.ReleaseAndGetAddressOf());
}
HRESULT WriteBuffer(ID3D11DeviceContext *context,ID3D11Buffer *buffer,const void *data,std::size_t bytes){
 D3D11_MAPPED_SUBRESOURCE mapped{};HRESULT hr=context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
 if(FAILED(hr))return hr;std::memcpy(mapped.pData,data,bytes);context->Unmap(buffer,0);return S_OK;
}
}

HRESULT CompatibilityRenderer::Fail(HRESULT code,const std::string &message){
 std::ostringstream detail;detail<<message<<" (HRESULT 0x"<<std::hex<<code<<")";
 error_=detail.str();D3D11Diagnostic(("D3D11 bridge: "+error_).c_str());return code;
}
HRESULT CompatibilityRenderer::Initialize(IDirect3DDevice8 *native,HWND window,UINT width,UINT height){
 char bindings[32]{};GetEnvironmentVariableA("GENZH_SCENE_BINDINGS",bindings,sizeof(bindings));sceneBindingsEnabled_=std::strcmp(bindings,"off")!=0;
 char resolved[32]{};GetEnvironmentVariableA("GENZH_RESOLVED_STATES",resolved,sizeof(resolved));resolvedStatesEnabled_=std::strcmp(resolved,"off")!=0;
 char palette[32]{};GetEnvironmentVariableA("GENZH_SORTED_COLOR_PALETTE",palette,sizeof(palette));sortedColorPaletteEnabled_=std::strcmp(palette,"off")!=0;
 char mirrored[32]{};GetEnvironmentVariableA("GENZH_MIRRORED_LOCKS",mirrored,sizeof(mirrored));mirroredBufferLocks.store(std::strcmp(mirrored,"off")!=0,std::memory_order_relaxed);
 resolvedRaster_.Clear();resolvedBlend_.Clear();resolvedDepth_.Clear();resolvedSampler_.Clear();
 native_=native;HRESULT hr=renderer_.Initialize(window,width,height);
 if(FAILED(hr))return Fail(hr,renderer_.LastError());
 ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterInfo{};
 if(SUCCEEDED(renderer_.Device()->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())))&&
    SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf()))&&SUCCEEDED(adapter->GetDesc(&adapterInfo))){
  char description[512]{};WideCharToMultiByte(CP_UTF8,0,adapterInfo.Description,-1,description,sizeof(description),nullptr,nullptr);
  D3D11Diagnostic((std::string("D3D11 active adapter: ")+description).c_str());
 }
 D3D11Diagnostic("Static geometry cache enabled: adaptive CPU/GPU budget; revision/range invalidation");
 if(FAILED(hr=native_->GetRenderTarget(nativeDefault_.ReleaseAndGetAddressOf())))return Fail(hr,"query native default surface");
 if(FAILED(hr=calibratedLights_.Create(renderer_.Device())))return Fail(hr,"native light calibration buffer");
 if(FAILED(hr=DynamicBuffer(renderer_.Device(),sizeof(LegacyConstants),D3D11_BIND_CONSTANT_BUFFER,constantBuffer_)))return Fail(hr,"legacy constant buffer");
 char constantChoice[32]{};GetEnvironmentVariableA("GENZH_BATCH_CONSTANTS",constantChoice,sizeof(constantChoice));
 sceneConstantArena_.Initialize(renderer_.Device(),renderer_.Context(),std::strcmp(constantChoice,"off")!=0);sceneConstantIndex_=UINT_MAX;
 if(FAILED(hr=DynamicBuffer(renderer_.Device(),1536,D3D11_BIND_CONSTANT_BUFFER,vertexConstants_)))return hr;
 if(FAILED(hr=DynamicBuffer(renderer_.Device(),sizeof(shadowData_),D3D11_BIND_CONSTANT_BUFFER,shadowConstants_)))return hr;
 if(FAILED(hr=DynamicBuffer(renderer_.Device(),16,D3D11_BIND_CONSTANT_BUFFER,terrainConstants_)))return hr;
 {const float zero[4]{};if(FAILED(hr=DynamicBuffer(renderer_.Device(),16,D3D11_BIND_CONSTANT_BUFFER,taaVertexJitter_))||FAILED(hr=DynamicBuffer(renderer_.Device(),16,D3D11_BIND_CONSTANT_BUFFER,taaVertexZero_))||
   FAILED(hr=WriteBuffer(renderer_.Context(),taaVertexZero_.Get(),zero,16))||FAILED(hr=WriteBuffer(renderer_.Context(),taaVertexJitter_.Get(),zero,16)))return Fail(hr,"TAA vertex jitter buffers");}
 char weatherChoice[32]{};GetEnvironmentVariableA("GENZH_WEATHER",weatherChoice,sizeof(weatherChoice));
 WeatherMode weatherMode=WeatherMode::Dynamic;
 if(!std::strcmp(weatherChoice,"clear"))weatherMode=WeatherMode::Clear;
 else if(!std::strcmp(weatherChoice,"overcast"))weatherMode=WeatherMode::Overcast;
 else if(!std::strcmp(weatherChoice,"rain"))weatherMode=WeatherMode::Rain;
 else if(!std::strcmp(weatherChoice,"off"))weatherMode=WeatherMode::Off;
 hr=atmosphere_.Create(renderer_.Device(),width,height,weatherMode);
 if(FAILED(hr)){atmosphere_.Reset();D3D11Diagnostic("Renderer32 atmosphere allocation/compile failed; native surface quality retained");}
 else D3D11Diagnostic(hr==S_OK?"Renderer32 GPU volumetric lights, 3D clouds and shared dynamic weather enabled":"Renderer32 atmosphere explicitly disabled");
 atmosphereTick_=std::chrono::steady_clock::now();
 char choice[32]{};GetEnvironmentVariableA("GENZH_SHADOWS",choice,sizeof(choice));modernShadows_=std::strcmp(choice,"modern")==0;
 char culling[32]{};GetEnvironmentVariableA("GENZH_CAMERA_CULLING",culling,sizeof(culling));cameraCulling_=std::strcmp(culling,"off")!=0;
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;raster.DepthBias=100;raster.SlopeScaledDepthBias=1.5f;
 if(FAILED(hr=renderer_.Device()->CreateRasterizerState(&raster,shadowRaster_.ReleaseAndGetAddressOf())))return hr;
 D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
 if(FAILED(hr=renderer_.Device()->CreateDepthStencilState(&depth,shadowState_.ReleaseAndGetAddressOf())))return hr;
 D3D11_SAMPLER_DESC terrain{};terrain.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
 terrain.AddressU=terrain.AddressV=terrain.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;terrain.MaxLOD=D3D11_FLOAT32_MAX;
 if(FAILED(hr=renderer_.Device()->CreateSamplerState(&terrain,terrainSampler_.ReleaseAndGetAddressOf())))return hr;
 gpuTimer_.Initialize(renderer_.Device());
 passTimer_.Initialize(renderer_.Device());
 shadowDrawTimer_.Initialize(renderer_.Device());lightPreparationTimer_.Initialize(renderer_.Device());
 char opaqueChoice[32]{};GetEnvironmentVariableA("GENZH_OPAQUE_SHADOW_DEPTH",opaqueChoice,sizeof(opaqueChoice));opaqueShadowDepth_=std::strcmp(opaqueChoice,"off")!=0;
 pipelineStats_.Initialize(renderer_.Device());
 drawConstantsCache_.Reset();vertexConstantsCache_.Reset();shadowConstantsCache_.Reset();terrainConstantsCache_.Reset();instanceSettingsCache_.Reset();
 shadowData_={};
 localShadowAtlas_.Reset();for(auto &cache:localShadowCache_)cache.Reset();localShadowStats_={};localShadowFrameTriangles_=0;
  char cascadeChoice[32]{};GetEnvironmentVariableA("GENZH_CASCADE_SIZE",cascadeChoice,sizeof(cascadeChoice));cascadeSize_=std::strcmp(cascadeChoice,"1024")==0?1024u:std::strcmp(cascadeChoice,"4096")==0?4096u:2048u;
 char splitChoice[32]{};GetEnvironmentVariableA("GENZH_CASCADE_SPLIT",splitChoice,sizeof(splitChoice));const float splitValue=static_cast<float>(std::atof(splitChoice));cascadeSplit_=splitValue>=.15f&&splitValue<=.6f?splitValue:.35f;
 if(modernShadows_&&FAILED(hr=shadowCascades_.Create(renderer_.Device(),cascadeSize_)))return Fail(hr,"directional cascade array");
 localSurfaceLighting_.Reset();localSurfaceStats_={};if(modernShadows_&&FAILED(hr=localSurfaceLighting_.Create(renderer_.Device())))return Fail(hr,"independent local surface lighting");
 staticShadowCascades_.Reset();for(auto &cache:shadowReuse_)cache.Reset();
 char terrainShadowChoice[32]{};GetEnvironmentVariableA("GENZH_TERRAIN_SHADOWS",terrainShadowChoice,sizeof(terrainShadowChoice));terrainShadows_=std::strcmp(terrainShadowChoice,"off")!=0;
 terrainShadowStats_={};
 char cacheChoice[32]{};GetEnvironmentVariableA("GENZH_SHADOW_CACHE",cacheChoice,sizeof(cacheChoice));
 shadowCache_=modernShadows_&&std::strcmp(cacheChoice,"off")!=0&&ShadowCacheMemoryAvailable(renderer_.Device());
 if(shadowCache_&&FAILED(staticShadowCascades_.Create(renderer_.Device(),cascadeSize_)))shadowCache_=false;
 D3D11Diagnostic(shadowCache_?"Renderer30: bounded stationary shadow depth cache enabled; mutable casters remain live":"Renderer30: full live shadow rendering retained; cache disabled/unavailable");
 D3D11Diagnostic(modernShadows_?"Renderer29: two world-stabilized 2048 directional cascades with conservative caster culling":"Renderer29: cascades disabled by legacy shadow mode");
 if(modernShadows_)D3D11Diagnostic(("Renderer29b: cascade size "+std::to_string(cascadeSize_)+" split "+std::to_string(cascadeSplit_)).c_str());
 D3D11Diagnostic(modernShadows_?"Modern directional scene shadows enabled":"Legacy scene shadows retained");
 D3D11Diagnostic(cameraCulling_?"Conservative mesh camera culling enabled; shadows retain original submitted casters":"Mesh camera culling disabled");
 return S_OK;
}
void CompatibilityRenderer::PrepareReset(){
 calibratedLights_.Reset();
 atmosphere_.Reset();
 presentedFrames_.Reset();
 shadowCascades_.Reset();shadowStats_={};
 localShadowAtlas_.Reset();for(auto &cache:localShadowCache_)cache.Reset();localShadowStats_={};
 localSurfaceLighting_.Reset();localSurfaceStats_={};
 staticShadowCascades_.Reset();for(auto &cache:shadowReuse_)cache.Reset();shadowCache_=false;shadowCacheStats_={};
 materialStreaming_.Reset();terrainLibrary_.Stop();grass_.Reset();
 terrainUpload_.reset();terrainPreparation_.Cancel();terrainPendingOwner_.reset();terrainPendingRevision_=0;
 terrainMaps_.clear();terrainSampler_.Reset();terrainExclusionTexture_.Reset();terrainExclusionView_.Reset();terrainExclusionWidth_=terrainExclusionHeight_=0;
 terrainExclusionInputs_.Reset();
 gpuTimer_.Reset();
 passTimer_.Reset();pipelineStats_.Reset();shadowDrawTimer_.Reset();lightPreparationTimer_.Reset();
 instanceBuffer_.Reset();instanceSettings_.Reset();instanceView_.Reset();instanceCapacity_=0;
 sceneConstantArena_.Reset();sceneConstantIndex_=UINT_MAX;
 renderer_.Context()->ClearState();nativeDefault_.Reset();overlays_.clear();scenePackets_.clear();surfaces_.clear();textures_.clear();resolvedRaster_.Clear();resolvedBlend_.Clear();resolvedDepth_.Clear();resolvedSampler_.Clear();rasterStates_.clear();blendStates_.clear();depthStates_.clear();samplerStates_.clear();
}
HRESULT CompatibilityRenderer::Reset(HWND window,UINT width,UINT height){
 overlays_.clear();ForgetShaders();textures_.clear();vertexBuffer_.Reset();indexBuffer_.Reset();constantBuffer_.Reset();vertexConstants_.Reset();taaVertexJitter_.Reset();taaVertexZero_.Reset();
 vertexCapacity_=indexCapacity_=0;inScene_=ready_=offscreen_=false;
 return Initialize(native_.Get(),window,width,height);
}
void CompatibilityRenderer::ForgetShaders(){programs_.clear();pixelAssembly_.clear();vertexAssembly_.clear();}
void CompatibilityRenderer::TextureChanged(const std::shared_ptr<TextureData> &texture){if(texture)++texture->revision;}

void CompatibilityRenderer::RefreshCaptureConfiguration(){
 runtimeSwitches_.Poll();
 char palette[32]{};GetEnvironmentVariableA("GENZH_SORTED_COLOR_PALETTE",palette,sizeof(palette));sortedColorPaletteEnabled_=std::strcmp(palette,"off")!=0;
 wchar_t folder[32768];const DWORD length=GetEnvironmentVariableW(L"GENZH_MATERIAL_DIR",folder,32768);
 const std::filesystem::path root=length&&length<32768?std::filesystem::path(folder):std::filesystem::path{};
 if(root!=materialRoot_){materialRoot_=root;++materialRootRevision_;}
 char choice[32]{};GetEnvironmentVariableA("GENZH_MATERIAL_LIGHTING",choice,sizeof(choice));materialCompatibility_=std::strcmp(choice,"compatibility")==0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_TERRAIN",choice,sizeof(choice));terrainEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_FOLIAGE",choice,sizeof(choice));foliageEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_DERIVED_FIXED",choice,sizeof(choice));derivedFixed_.enabled=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_DEPTH_RADIX",choice,sizeof(choice));stableDepthSortReference=std::strcmp(choice,"off")==0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_CAPTURE_PROFILE",choice,sizeof(choice));captureWorkEnabled=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_MIRRORED_LOCKS",choice,sizeof(choice));mirroredBufferLocks.store(std::strcmp(choice,"off")!=0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_SORTED_RUN_TEMPLATES",choice,sizeof(choice));sortedRunTemplatesEnabled.store(std::strcmp(choice,"on")==0,std::memory_order_relaxed); // opt-in (perf85)
 choice[0]=0;GetEnvironmentVariableA("GENZH_SORTED_RUN_TEMPLATE_VERIFY",choice,sizeof(choice));sortedRunTemplateVerify.store(std::strcmp(choice,"on")==0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_COPY_RECTS_REPEAT",choice,sizeof(choice));int copyMode=std::strcmp(choice,"off")!=0?1:0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_COPY_RECTS_VERIFY",choice,sizeof(choice));if(!std::strcmp(choice,"on"))copyMode=2;
 copyRectsRepeatMode.store(copyMode,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_SNAPSHOT_RECYCLE",choice,sizeof(choice));capturedSnapshotRecycle.store(std::strcmp(choice,"off")!=0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_STAGED_PACKETS",choice,sizeof(choice));stagedScenePackets.store(std::strcmp(choice,"off")!=0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_STATE_REVISION",choice,sizeof(choice));stateRevisionCompare.store(std::strcmp(choice,"off")!=0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_SIMD_EQUAL",choice,sizeof(choice));simdByteEquality.store(std::strcmp(choice,"off")!=0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_ASYNC_TELEMETRY",choice,sizeof(choice));GenZHLoad::asyncTelemetry.store(std::strcmp(choice,"off")!=0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_SHROUD_FOG",choice,sizeof(choice));shroudFogEnabled.store(std::strcmp(choice,"off")!=0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_SHROUD_FOG_FLAT",choice,sizeof(choice));shroudFogFlat.store(std::strcmp(choice,"on")==0,std::memory_order_relaxed);
 choice[0]=0;GetEnvironmentVariableA("GENZH_TERRAIN_LIBRARY",choice,sizeof(choice));terrainLibraryEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_TERRAIN_RELIEF",choice,sizeof(choice));terrainReliefEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_GRASS",choice,sizeof(choice));grassEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_GRASS_SHADOWS",choice,sizeof(choice));grassShadowsEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_GRASS_INTERACTION",choice,sizeof(choice));grassInteractionEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_TERRAIN_FIELD",choice,sizeof(choice));terrainFieldEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_TERRAIN_HEX",choice,sizeof(choice));terrainHexEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_DECAL",choice,sizeof(choice));decalEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_TERRAIN_WET",choice,sizeof(choice));terrainWetEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_ACCUMULATION",choice,sizeof(choice));accumulationEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_ROCKS",choice,sizeof(choice));terrainRocksEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_MOTION",choice,sizeof(choice));motionEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_WATER_SHORE",choice,sizeof(choice));waterShoreEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_WATER_REFLECTION",choice,sizeof(choice));waterReflectionEnabled_=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_SCENE_EXTRA_BINDINGS",choice,sizeof(choice));sceneBindings_.extraEnabled=std::strcmp(choice,"off")!=0;
 choice[0]=0;GetEnvironmentVariableA("GENZH_BLOOM",choice,sizeof(choice));renderer_.SetBloomMode(std::strcmp(choice,"off")==0?0:std::strcmp(choice,"legacy")==0?1:2);
 choice[0]=0;GetEnvironmentVariableA("GENZH_TAA",choice,sizeof(choice));taaEnabled_=std::strcmp(choice,"off")!=0;
 // post102: contrast-adaptive sharpening of the TAA output (GENZH_CAS=off: plain copy).
 choice[0]=0;GetEnvironmentVariableA("GENZH_CAS",choice,sizeof(choice));renderer_.TemporalAA().settings.sharpen=std::strcmp(choice,"off")!=0?.35f:0.0f;
 // Decoded once per material root on a worker; switching off only stops binding.
 if(terrainLibraryEnabled_&&!materialRoot_.empty())terrainLibrary_.Start(materialRoot_/L"terrain-library-v1");
}
HRESULT CompatibilityRenderer::BeginScene(){
 if(inScene_)return Fail(E_UNEXPECTED,"nested BeginScene");
 RefreshCaptureConfiguration();
 if(ready_)return Fail(E_UNEXPECTED,"new scene before presentation of previous frame");
 for(auto it=terrainMaps_.begin();it!=terrainMaps_.end();)it=it->second.owner.expired()?terrainMaps_.erase(it):std::next(it);
 for(auto it=textures_.begin();it!=textures_.end();)it=it->second.owner.expired()?textures_.erase(it):std::next(it);
 const auto atmosphereNow=std::chrono::steady_clock::now();
 atmosphereDelta_=std::chrono::duration<double>(atmosphereNow-atmosphereTick_).count();atmosphereTick_=atmosphereNow;
 atmosphere_.BeginFrame();localSurfaceLighting_.BeginFrame();worldFrameDrawn_=false;grassDrawn_=false;
 taaActive_=false;++taaFrame_;taaJitter_=TemporalJitterPixels(taaFrame_);
 // photo107: motion: this frame's objects and camera become the previous ones.
 motionPrevious_.swap(motionCurrent_);motionCurrent_.clear();
 std::memcpy(motionPreviousVP_,motionCurrentVP_,sizeof(motionPreviousVP_));motionPreviousValid_=motionCurrentValid_;motionCurrentValid_=false;
 
 // taa101: how many game vertex-program draws took the jitter (in-game check that trees/water match the camera).
 if(taaFrame_%600==0)GenZHLoad::Record("taa_vertex_v101",std::to_string(taaVertexDraws_)+" jittered game vertex-program draws (cumulative)",0);
 if(taaFrame_%600==0&&(waterShoreDraws_||waterShoreFallbacks_))GenZHLoad::Record("water_shore_v107",std::to_string(waterShoreDraws_)+" per-pixel shore draws, "+std::to_string(waterShoreFallbacks_)+" without scene depth (cumulative)",0);frameHasWorldDepth_=false;screenTargetsBound_=false;primaryDepthTarget_=true;localShadowFrameTriangles_=0;localShadowBindings_.Clear();
 FrameConstants frame{};frame.cameraExposure[3]=1;
 HRESULT hr=renderer_.SetFrame(frame);if(SUCCEEDED(hr))hr=renderer_.BeginFrame(clear_);
 if(FAILED(hr))return Fail(hr,renderer_.LastError());
 renderer_.Context()->ClearDepthStencilView(renderer_.SceneDepth(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,clearDepth_,static_cast<UINT8>(clearStencil_));
 gpuTimer_.Poll(renderer_.Context(),[&](double ms){stats_.gpuMs+=ms;++stats_.gpuSamples;});
 passTimer_.PollDetailed(renderer_.Context(),[&](const auto &times,const auto &cpu){for(unsigned i=0;i<times.size();++i){workStats_.passMs[i]+=times[i];workStats_.passCpuMs[i]+=cpu[i];}++workStats_.passSamples;});
 shadowDrawTimer_.Poll(renderer_.Context(),[&](double ms){workStats_.shadowDrawMs+=ms;++workStats_.shadowDrawSamples;});
 lightPreparationTimer_.Poll(renderer_.Context(),[&](double ms){workStats_.lightPreparationMs+=ms;++workStats_.lightPreparationSamples;});
 gpuTimer_.Begin(renderer_.Context());frameStart_=std::chrono::steady_clock::now();
 pipelineStats_.Poll(renderer_.Context(),[&](const D3D11_QUERY_DATA_PIPELINE_STATISTICS &data){
  const std::uint64_t counts[]={data.IAPrimitives,data.VSInvocations,data.GSInvocations,data.GSPrimitives,data.HSInvocations,data.DSInvocations,data.PSInvocations};
  for(unsigned i=0;i<7;++i)workStats_.pipelineCounts[i]+=counts[i];++workStats_.pipelineSamples;
 });
 pipelineStats_.Begin(renderer_.Context());passTimer_.Begin(renderer_.Context());
 // Terrain and authored maps share one upload slot: total 4 MiB/frame, not
 // separate budgets. CPU preparation is also serialized between the queues.
 materialStreaming_.BeginFrame();
 materialStreaming_.UpdateBudget(renderer_.Device());
 if(terrainPendingRevision_)PumpTerrainLoading();
 else if(terrainLibrary_.Pending()){if(FAILED(hr=terrainLibrary_.Pump(renderer_.Device(),renderer_.Context())))return Fail(hr,"terrain library upload");}
 else materialStreaming_.Pump(renderer_.Device(),renderer_.Context(),terrainPreparation_.Available());
 passTimer_.Mark(renderer_.Context(),GpuPhase::Capture);
 overlays_.clear();scenePackets_.clear();shadowData_.nearSettings[3]=0;inScene_=true;offscreen_=false;shroudFrameTexture_.reset();worldPassesDone_=false;return S_OK;
}
HRESULT CompatibilityRenderer::EndScene(){
 if(!inScene_)return Fail(E_UNEXPECTED,"EndScene without BeginScene");
 if(offscreen_)return Fail(E_UNEXPECTED,"offscreen target was not restored before EndScene");
 HRESULT hr=FlushScene();if(FAILED(hr))return hr;
 if(FAILED(hr=FinishWorldPasses()))return hr;
 atmosphere_.DrawRain(renderer_.Context(),renderer_.SceneTarget(),renderer_.SceneDepth(),renderer_.SceneDepthView(),renderer_.SceneReadOnlyDepth());
 passTimer_.Mark(renderer_.Context(),GpuPhase::Post);
 hr=renderer_.ToneMapFrame();if(FAILED(hr))return Fail(hr,renderer_.LastError());
 passTimer_.Mark(renderer_.Context(),GpuPhase::HUD);
 for(const auto &packet:overlays_)if(FAILED(hr=Execute(packet)))return hr;
 pipelineStats_.End(renderer_.Context());passTimer_.End(renderer_.Context());gpuTimer_.End(renderer_.Context());overlays_.clear();inScene_=false;ready_=true;
 completedSceneCpuMs_=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frameStart_).count();
 stats_.cpuMs+=completedSceneCpuMs_;
 ++stats_.frames;if(stats_.frames>=120||std::chrono::steady_clock::now()-lastStatsWrite_>=std::chrono::seconds(1))WritePerformanceStats();return S_OK;
}
void CompatibilityRenderer::WritePerformanceStats(){
 if(!stats_.frames)return;
 lastStatsWrite_=std::chrono::steady_clock::now();
 if(sortedPacketMerges_){GenZHLoad::Record("scene_queue_batch_window",std::to_string(stats_.frames)+" frames; identical adjacent sorted draws merged="+std::to_string(sortedPacketMerges_),0,sortedPacketMerges_*sizeof(DrawPacket));sortedPacketMerges_=0;}
 if(!textureUploads_.empty()){
  std::vector<std::pair<const void*,TextureUploadStat>> uploads(textureUploads_.begin(),textureUploads_.end());
  std::sort(uploads.begin(),uploads.end(),[](const auto &a,const auto &b){return a.second.count>b.second.count;});
  std::uint64_t total=0;for(const auto &u:uploads)total+=u.second.count;
  std::string detail="frames="+std::to_string(stats_.frames)+" uploads="+std::to_string(total)+" textures="+std::to_string(uploads.size());
  for(std::size_t i=0;i<uploads.size()&&i<5;++i)detail+=" | count="+std::to_string(uploads[i].second.count)+" fresh="+std::to_string(uploads[i].second.fresh)+" "+uploads[i].second.detail;
  GenZHLoad::Record("texture_upload_v86",detail,0);textureUploads_.clear();
 }
 if(copyRectsStats_.copies){const auto &c=copyRectsStats_;
  GenZHLoad::Record("copy_rects_v87","frames="+std::to_string(stats_.frames)+" copies="+std::to_string(c.copies)+" tracked="+std::to_string(c.tracked)+" repeats="+std::to_string(c.repeats)+
   " verified="+std::to_string(c.verified)+" mismatches="+std::to_string(c.mismatches)+" mode="+std::to_string(copyRectsRepeatMode.load(std::memory_order_relaxed)),0);
  copyRectsStats_={};
 }
 {const auto &s=shroudFogStats_;auto &a=atmosphere_.stats;
  if(s.replaced||s.neutralized||s.missed||s.invalid){
   GenZHLoad::Record("shroud_fog_v92","frames="+std::to_string(stats_.frames)+" replaced="+std::to_string(s.replaced)+" neutralized="+std::to_string(s.neutralized)+
    " fog="+std::to_string(s.fogPasses)+" flat="+std::to_string(s.flatPasses)+" missed="+std::to_string(s.missed)+" invalid="+std::to_string(s.invalid)+
    " vision="+std::to_string(s.vision)+" before_hud="+std::to_string(s.beforeHud)+" vision_revision="+std::to_string(GetD3D11ShroudVisibility().revision)+
    " gpu_samples="+std::to_string(a.shroudGpuSamples)+" mean_gpu_ms="+std::to_string(a.shroudGpuSamples?a.shroudGpuMs/static_cast<double>(a.shroudGpuSamples):-1.0)+
    " mode="+(shroudFogEnabled.load(std::memory_order_relaxed)?(shroudFogFlat.load(std::memory_order_relaxed)?"flat":"volume"):"native"),0);
   shroudFogStats_={};a.shroudGpuMs=0;a.shroudGpuSamples=0;
  }
 }
 {
  wchar_t directory[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",directory,32768);
  if(length&&length<32768){const auto path=std::filesystem::path(directory)/L"renderer53-surface-light.csv";const bool header=!std::filesystem::exists(path);GenZHLoad::AppendLog log(path,std::ios::app);
   if(log){if(header)log<<"pid,utc_epoch_ms,available,cumulative_frames,cumulative_dispatches,cumulative_unsupported,GPU_bytes,cumulative_gpu_samples,mean_GPU_ms,mean_CPU_ms,cumulative_contact_frames\n";
    const auto &ao=renderer_.ScreenAO();const auto &a=ao.stats;const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();log<<GetCurrentProcessId()<<','<<epoch<<','<<ao.Available()<<','<<a.frames<<','<<a.dispatches<<','<<a.unsupported<<','<<ao.Bytes()<<','<<a.gpuSamples<<','<<(a.gpuSamples?a.gpuMs/static_cast<double>(a.gpuSamples):-1)<<','<<(a.frames?a.cpuMs/static_cast<double>(a.frames):0)<<','<<a.contactFrames<<'\n';
   }
  }
 }

 {
  wchar_t directory[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",directory,32768);
  if(length&&length<32768){
   const auto path=std::filesystem::path(directory)/L"renderer51-material-capture.csv";const bool header=!std::filesystem::exists(path);GenZHLoad::AppendLog log(path,std::ios::app);
   if(log){if(header)log<<"pid,utc_epoch_ms,cumulative_primary_draws,cumulative_excluded_draws,cumulative_uploads,cumulative_upload_skips,cumulative_clears,GPU_bytes,camera_ready\n";
    const auto &m=renderer_.ScreenMaterials();const auto &a=m.stats;const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    log<<GetCurrentProcessId()<<','<<epoch<<','<<a.primaryDraws<<','<<a.excludedDraws<<','<<a.uploads<<','<<a.skips<<','<<a.clears<<','<<m.Bytes()<<','<<m.CameraReady()<<'\n';
   }
  }
 }

 {
  wchar_t directory[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",directory,32768);
  if(length&&length<32768){
   const auto path=std::filesystem::path(directory)/L"renderer50-scene-depth.csv";const bool header=!std::filesystem::exists(path);GenZHLoad::AppendLog log(path,std::ios::app);
   if(log){if(header)log<<"pid,utc_epoch_ms,cumulative_builds,cumulative_dispatches,hierarchy_bytes,cumulative_gpu_samples,mean_gpu_ms,mean_cpu_ms\n";
    const auto &d=renderer_.SceneDepthData();const auto &a=d.stats;const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    log<<GetCurrentProcessId()<<','<<epoch<<','<<a.builds<<','<<a.dispatches<<','<<d.HierarchyBytes()<<','<<a.gpuSamples<<','<<(a.gpuSamples?a.gpuMs/static_cast<double>(a.gpuSamples):-1)<<','<<(a.builds?a.cpuMs/static_cast<double>(a.builds):0)<<'\n';
   }
  }
 }

 wchar_t profile[32768]{};const DWORD size=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",profile,32768);
 if(size&&size<32768){
  WriteCaptureWorkTelemetry(std::filesystem::path(profile),stats_.frames,derivedFixed_.calculations,derivedFixed_.reuses);
  const auto path=std::filesystem::path(profile)/L"renderer-performance-v16.csv";
  std::error_code error;const bool header=!std::filesystem::exists(path,error)||std::filesystem::file_size(path,error)==0;
  GenZHLoad::AppendLog log(path,std::ios::app);
  if(log){
   if(header)log<<"pid,utc_epoch_ms,frames,cpu_frame_ms,exclusion_ms,shadow_submit_ms,scene_submit_ms,cache_hits,cache_misses,decoded_vertices,decoded_indices,geometry_upload_bytes,draws,cache_bytes,cache_entries,cache_evictions,native_draws,grouped_instances,gpu_frame_ms,gpu_samples,shader_cache_hits,shader_compiles\n";
   const double frames=static_cast<double>(stats_.frames);
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   log<<GetCurrentProcessId()<<','<<epoch<<','<<std::fixed<<std::setprecision(3)<<stats_.frames<<','<<stats_.cpuMs/frames<<','<<stats_.exclusionMs/frames<<','<<stats_.shadowMs/frames<<','<<stats_.sceneMs/frames<<','<<stats_.cacheHits<<','<<stats_.cacheMisses<<','<<stats_.decodedVertices<<','<<stats_.decodedIndices<<','<<stats_.uploadBytes<<','<<stats_.draws<<','<<stats_.cacheBytes<<','<<stats_.cacheEntries<<','<<stats_.cacheEvictions<<','<<stats_.nativeDraws<<','<<stats_.groupedInstances<<','<<(stats_.gpuSamples?stats_.gpuMs/static_cast<double>(stats_.gpuSamples):-1)<<','<<stats_.gpuSamples<<','<<stats_.shaderCacheHits<<','<<stats_.shaderCompiles<<'\n';
  }
  // Separate schema keeps existing v16 performance logs readable. Cache bytes
  // exclude driver overhead and COM references retained by submitted packets.
  const auto shadowPath=std::filesystem::path(profile)/L"renderer-shadows-v29.csv";
  const bool shadowHeader=!std::filesystem::exists(shadowPath,error)||std::filesystem::file_size(shadowPath,error)==0;
  GenZHLoad::AppendLog shadowLog(shadowPath,std::ios::app);
  if(shadowLog){
   if(shadowHeader)shadowLog<<"pid,utc_epoch_ms,shadow_frames,cascade_frames,fallback_frames,input_draws,near_retained,near_culled,near_groups,near_submit_CPU_ms,near_world_texel,far_retained,far_culled,far_groups,far_submit_CPU_ms,far_world_texel,depth_array_bytes\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   shadowLog<<GetCurrentProcessId()<<','<<epoch<<','<<shadowStats_.frames<<','<<shadowStats_.cascadeFrames<<','<<shadowStats_.fallbackFrames<<','<<shadowStats_.inputs;
   for(unsigned i=0;i<2;++i)shadowLog<<','<<shadowStats_.retained[i]<<','<<shadowStats_.culled[i]<<','<<shadowStats_.groups[i]<<','<<(shadowStats_.frames?shadowStats_.submitMs[i]/shadowStats_.frames:0)<<','<<(shadowStats_.cascadeFrames?shadowStats_.texelSum[i]/shadowStats_.cascadeFrames:0);
   shadowLog<<','<<(modernShadows_?33554432:0)<<'\n';
  }
  shadowStats_={};
  const auto terrainShadowPath=std::filesystem::path(profile)/L"renderer-terrain-shadows-v31.csv";
  const bool terrainShadowHeader=!std::filesystem::exists(terrainShadowPath,error)||std::filesystem::file_size(terrainShadowPath,error)==0;
  GenZHLoad::AppendLog terrainShadow(terrainShadowPath,std::ios::app);
  if(terrainShadow){
   if(terrainShadowHeader)terrainShadow<<"pid,utc_epoch_ms,terrain_shadow_requested,input_draws,near_retained,near_stationary,near_live,far_retained,far_stationary,far_live,exclusion_revision\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   terrainShadow<<GetCurrentProcessId()<<','<<epoch<<','<<terrainShadows_<<','<<terrainShadowStats_.inputs<<','<<terrainShadowStats_.retained[0]<<','<<terrainShadowStats_.stationary[0]<<','<<terrainShadowStats_.live[0]<<','<<terrainShadowStats_.retained[1]<<','<<terrainShadowStats_.stationary[1]<<','<<terrainShadowStats_.live[1]<<','<<terrainExclusionRevision_<<'\n';
  }
  terrainShadowStats_={};
  const auto atmospherePath=std::filesystem::path(profile)/L"renderer-atmosphere-v32.csv";
  const bool atmosphereHeader=!std::filesystem::exists(atmospherePath,error)||std::filesystem::file_size(atmospherePath,error)==0;
  GenZHLoad::AppendLog atmosphereLog(atmospherePath,std::ios::app);
  if(atmosphereLog){
   if(atmosphereHeader)atmosphereLog<<"pid,utc_epoch_ms,available,preparations,cloud_shadow_passes,volume_passes,sky_passes,rain_passes,world_binds,preparation_CPU_ms,coverage,rain,wetness,extinction,fixed_GPU_bytes,volume_cloud_GPU_ms,gpu_samples\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   const auto &a=atmosphere_.stats;const auto &d=atmosphere_.Data();
   atmosphereLog<<GetCurrentProcessId()<<','<<epoch<<','<<atmosphere_.Available()<<','<<a.preparations<<','<<a.cloudPasses<<','<<a.volumePasses<<','<<a.skyPasses<<','<<a.rainPasses<<','<<a.binds<<','<<a.cpuMs;
   for(float weather:d.weather)atmosphereLog<<','<<weather;
   atmosphereLog<<','<<atmosphere_.PayloadBytes()<<','<<(a.gpuSamples?a.gpuMs/static_cast<double>(a.gpuSamples):-1)<<','<<a.gpuSamples<<'\n';
  }
  const auto probePath=std::filesystem::path(profile)/L"renderer-weather-probes-v33.csv";
  const bool probeHeader=!std::filesystem::exists(probePath,error)||std::filesystem::file_size(probePath,error)==0;
  GenZHLoad::AppendLog probeLog(probePath,std::ios::app);
  if(probeLog){
   if(probeHeader)probeLog<<"pid,utc_epoch_ms,available,updates,GPU_ms,gpu_samples,history_blend,fixed_GPU_bytes\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   const auto &a=atmosphere_.stats;
   probeLog<<GetCurrentProcessId()<<','<<epoch<<','<<atmosphere_.ProbeReady()<<','<<a.probeUpdates<<','<<(a.probeGpuSamples?a.probeGpuMs/static_cast<double>(a.probeGpuSamples):-1)<<','<<a.probeGpuSamples<<','<<atmosphere_.Data().sunColor[3]<<','<<(atmosphere_.Available()?AtmosphereGPU::ProbeBytes:0)<<'\n';
  }
  const auto cloudHistoryPath=std::filesystem::path(profile)/L"renderer-cloud-history-v37.csv";
  const bool cloudHistoryHeader=!std::filesystem::exists(cloudHistoryPath,error)||std::filesystem::file_size(cloudHistoryPath,error)==0;
  GenZHLoad::AppendLog cloudHistoryLog(cloudHistoryPath,std::ios::app);
  if(cloudHistoryLog){
   if(cloudHistoryHeader)cloudHistoryLog<<"pid,utc_epoch_ms,available,sky_passes,history_passes,history_resets,total_atmosphere_GPU_bytes\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   const auto &a=atmosphere_.stats;
   cloudHistoryLog<<GetCurrentProcessId()<<','<<epoch<<','<<atmosphere_.Available()<<','<<a.skyPasses<<','<<a.cloudHistoryPasses<<','<<a.cloudHistoryResets<<','<<atmosphere_.PayloadBytes()<<'\n';
  }
  const auto localPath=std::filesystem::path(profile)/L"renderer-local-volume-v38.csv";
  const bool localHeader=!std::filesystem::exists(localPath,error)||std::filesystem::file_size(localPath,error)==0;
  GenZHLoad::AppendLog localLog(localPath,std::ios::app);
  if(localLog){
   if(localHeader)localLog<<"pid,utc_epoch_ms,available,sources_seen,cluster_passes,duplicates,filtered,overflow,cluster_bytes,source_constant_bytes\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto &a=atmosphere_.stats;
   localLog<<GetCurrentProcessId()<<','<<epoch<<','<<atmosphere_.Available()<<','<<a.localLightSources<<','<<a.localClusterPasses<<','<<a.localSourceDuplicates<<','<<a.localSourceRejected<<','<<a.localSourceOverflow<<','<<LocalVolumeClusters*16<<','<<sizeof(LocalVolumeConstants)<<'\n';
  }
  atmosphere_.stats={};
  const auto localShadowPath=std::filesystem::path(profile)/L"renderer-local-shadows-v39.csv";
  const bool localShadowHeader=!std::filesystem::exists(localShadowPath,error)||std::filesystem::file_size(localShadowPath,error)==0;
  GenZHLoad::AppendLog localShadowLog(localShadowPath,std::ios::app);
  if(localShadowLog){
   if(localShadowHeader)localShadowLog<<"pid,utc_epoch_ms,selected,shadowed,faces_rendered,faces_reused,draws,triangles,budget_bypass,unshadowed_sources,preparation_CPU_ms,atlas_GPU_bytes,cache_CPU_bytes\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();std::size_t cacheBytes=0;for(const auto &cache:localShadowCache_)cacheBytes+=cache.Bytes();
   const auto &s=localShadowStats_;localShadowLog<<GetCurrentProcessId()<<','<<epoch<<','<<s.selected<<','<<s.shadowed<<','<<s.faces<<','<<s.hits<<','<<s.draws<<','<<s.triangles<<','<<s.budgetBypass<<','<<s.unshadowed<<','<<s.cpuMs<<','<<(localShadowAtlas_.Available()?LocalShadowGPU::Bytes:0)<<','<<cacheBytes<<'\n';
  }
  localShadowStats_={};
  const auto localSurfacePath=std::filesystem::path(profile)/L"renderer-local-surface-v41.csv";const bool localSurfaceHeader=!std::filesystem::exists(localSurfacePath,error)||std::filesystem::file_size(localSurfacePath,error)==0;GenZHLoad::AppendLog localSurfaceLog(localSurfacePath,std::ios::app);
  if(localSurfaceLog){if(localSurfaceHeader)localSurfaceLog<<"pid,utc_epoch_ms,prepared_passes,tagged_draws,tagged_lights,binds,independent_constant_bytes,weather_available\n";const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto &s=localSurfaceStats_;localSurfaceLog<<GetCurrentProcessId()<<','<<epoch<<','<<s.prepared<<','<<s.taggedDraws<<','<<s.taggedLights<<','<<s.binds<<','<<localSurfaceLighting_.PayloadBytes()<<','<<atmosphere_.Available()<<'\n';}localSurfaceStats_={};
  const auto lightCalibrationPath=std::filesystem::path(profile)/L"renderer-light-calibration-v47.csv";
  const bool lightCalibrationHeader=!std::filesystem::exists(lightCalibrationPath,error)||std::filesystem::file_size(lightCalibrationPath,error)==0;
  GenZHLoad::AppendLog lightCalibrationLog(lightCalibrationPath,std::ios::app);
  if(lightCalibrationLog){if(lightCalibrationHeader)lightCalibrationLog<<"pid,utc_epoch_ms,requests,conversions,cache_hits,evictions,invalid,uploads,upload_skips,excluded,GPU_bytes,CPU_cache_bytes\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto &s=calibratedLights_.Stats();
   lightCalibrationLog<<GetCurrentProcessId()<<','<<epoch<<','<<s.requests<<','<<s.conversions<<','<<s.hits<<','<<s.evictions<<','<<s.invalid<<','<<s.uploads<<','<<s.skips<<','<<s.excluded<<','<<calibratedLights_.GPUPayloadBytes()<<','<<calibratedLights_.CPUPayloadBytes()<<'\n';}
  calibratedLights_.ClearStats();
  const auto stormPath=std::filesystem::path(profile)/L"renderer-storm-light-v42.csv";const bool stormHeader=!std::filesystem::exists(stormPath,error)||std::filesystem::file_size(stormPath,error)==0;GenZHLoad::AppendLog stormLog(stormPath,std::ios::app);
  if(stormLog){if(stormHeader)stormLog<<"pid,utc_epoch_ms,flash_frames,flash_events,envelope,source_x,source_y,source_z,constant_bytes\n";const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto &d=atmosphere_.Data();stormLog<<GetCurrentProcessId()<<','<<epoch<<','<<atmosphere_.stats.flashFrames<<','<<atmosphere_.stats.flashEvents<<','<<d.flashRadiance[3]<<','<<d.flashPosition[0]<<','<<d.flashPosition[1]<<','<<d.flashPosition[2]<<','<<sizeof(AtmosphereConstants)<<'\n';}atmosphere_.stats.flashFrames=atmosphere_.stats.flashEvents=0;
  const auto reusePath=std::filesystem::path(profile)/L"renderer-shadow-cache-v30.csv";
  const bool reuseHeader=!std::filesystem::exists(reusePath,error)||std::filesystem::file_size(reusePath,error)==0;
  GenZHLoad::AppendLog reuse(reusePath,std::ios::app);
  if(reuse){
   if(reuseHeader)reuse<<"pid,utc_epoch_ms,cascade_passes,cache_hits,cache_rebuilds,static_draws,live_draws,saved_draws,depth_copies,bypassed,preparation_CPU_ms,state_bytes,static_depth_bytes\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   reuse<<GetCurrentProcessId()<<','<<epoch<<','<<shadowCacheStats_.passes<<','<<shadowCacheStats_.hits<<','<<shadowCacheStats_.rebuilds<<','<<shadowCacheStats_.staticDraws<<','<<shadowCacheStats_.liveDraws<<','<<shadowCacheStats_.savedDraws<<','<<shadowCacheStats_.copies<<','<<shadowCacheStats_.bypassed<<','<<shadowCacheStats_.preparationMs<<','<<(shadowReuse_[0].StateBytes()+shadowReuse_[1].StateBytes())<<','<<(shadowCache_?33554432:0)<<'\n';
  }
  shadowCacheStats_={};
  if(shadowCache_&&!ShadowCacheMemoryAvailable(renderer_.Device())){
   staticShadowCascades_.Reset();for(auto &cache:shadowReuse_)cache.Reset();shadowCache_=false;
   D3D11Diagnostic("Renderer30 shadow cache released under VRAM budget pressure; full live shadows retained");
  }
  const auto streamingPath=std::filesystem::path(profile)/L"material-streaming-v17c.csv";
  const bool streamingHeader=!std::filesystem::exists(streamingPath,error)||std::filesystem::file_size(streamingPath,error)==0;
  GenZHLoad::AppendLog streaming(streamingPath,std::ios::app);
  if(streaming){
   if(streamingHeader)streaming<<"pid,utc_epoch_ms,cache_owned_bytes,entries,queued,active,evictions_total,submitted_bytes_total,upload_CPU_bytes,terrain_pending\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   streaming<<GetCurrentProcessId()<<','<<epoch<<','<<materialStreaming_.ResidentBytes()<<','<<materialStreaming_.Entries()<<','<<materialStreaming_.Queued()<<','<<materialStreaming_.Active()<<','<<materialStreaming_.Evictions()<<','<<materialStreaming_.SubmittedBytes()<<','<<materialStreaming_.UploadCPUBytes()<<','<<(terrainPendingRevision_!=0)<<'\n';
  }
  const auto workPath=std::filesystem::path(profile)/L"renderer-work-v18.csv";
  const bool workHeader=!std::filesystem::exists(workPath,error)||std::filesystem::file_size(workPath,error)==0;
  GenZHLoad::AppendLog work(workPath,std::ios::app);
  if(work){
   if(workHeader)work<<"pid,utc_epoch_ms,frames,constant_uploads,constant_skips,dynamic_vertex_bytes,dynamic_index_bytes,compacted_draws,scene_seen,scene_eligible,scene_groups,reordered_instances,eligible,no_geometry,topology,special,shader,layout,depth,blend_stencil,depth_func,pass_samples,gpu_upload_ms,gpu_capture_ms,gpu_shadow_ms,gpu_terrain_ms,gpu_scene_ms,gpu_post_ms,gpu_HUD_ms,gpu_offscreen_ms,pass_skipped_total,pass_discarded_total\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   work<<GetCurrentProcessId()<<','<<epoch<<','<<stats_.frames<<','<<workStats_.constantUploads<<','<<workStats_.constantSkips<<','<<workStats_.dynamicVertexBytes<<','<<workStats_.dynamicIndexBytes<<','<<workStats_.compactedDraws<<','<<workStats_.sceneSeen<<','<<workStats_.sceneEligible<<','<<workStats_.sceneGroups<<','<<workStats_.reorderedInstances;
   for(auto n:workStats_.eligibility)work<<','<<n;
   work<<','<<workStats_.passSamples;
   for(auto ms:workStats_.passMs)work<<','<<(workStats_.passSamples?ms/static_cast<double>(workStats_.passSamples):-1);
   work<<','<<passTimer_.Skipped()<<','<<passTimer_.Discarded()<<'\n';
  }
  const auto phasePath=std::filesystem::path(profile)/L"renderer-phase-v21.csv";
  const bool phaseHeader=!std::filesystem::exists(phasePath,error)||std::filesystem::file_size(phasePath,error)==0;
  GenZHLoad::AppendLog phase(phasePath,std::ios::app);
  if(phase){
   if(phaseHeader)phase<<"pid,utc_epoch_ms,frames,samples,grouping_cpu_ms,cpu_upload_ms,cpu_capture_ms,cpu_shadow_ms,cpu_terrain_ms,cpu_scene_ms,cpu_post_ms,cpu_HUD_ms,cpu_offscreen_ms,cpu_grass_ms,cpu_grass_draw_ms,gpu_upload_ms,gpu_capture_ms,gpu_shadow_ms,gpu_terrain_ms,gpu_scene_ms,gpu_post_ms,gpu_HUD_ms,gpu_offscreen_ms,gpu_grass_ms,gpu_grass_draw_ms,gpu_scene_blend_ms,gpu_scene_water_ms\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   phase<<GetCurrentProcessId()<<','<<epoch<<','<<stats_.frames<<','<<workStats_.passSamples<<','<<workStats_.groupingCpuMs/static_cast<double>(stats_.frames);
   for(auto ms:workStats_.passCpuMs)phase<<','<<(workStats_.passSamples?ms/static_cast<double>(workStats_.passSamples):-1);
   for(auto ms:workStats_.passMs)phase<<','<<(workStats_.passSamples?ms/static_cast<double>(workStats_.passSamples):-1);
   phase<<'\n';
  }
  const auto terrainCachePath=std::filesystem::path(profile)/L"renderer-terrain-cache-v69.csv";
  const bool terrainCacheHeader=!std::filesystem::exists(terrainCachePath,error)||std::filesystem::file_size(terrainCachePath,error)==0;
  GenZHLoad::AppendLog terrainCacheLog(terrainCachePath,std::ios::app);
  if(terrainCacheLog){
   if(terrainCacheHeader)terrainCacheLog<<"pid,utc_epoch_ms,tracked,untracked,first_seen,world_changed,camera_changed,shape_changed,source_changed,map_changed,owner_changed\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   terrainCacheLog<<GetCurrentProcessId()<<','<<epoch<<','<<workStats_.terrainTracked<<','<<workStats_.terrainUntracked<<','<<workStats_.terrainFirst<<','<<workStats_.terrainWorldChanged<<','<<workStats_.terrainCameraChanged<<','<<workStats_.terrainShapeChanged<<','<<workStats_.terrainSourceChanged<<','<<workStats_.terrainMapChanged<<','<<workStats_.terrainOwnerChanged<<'\n';
  }
  const auto depthPath=std::filesystem::path(profile)/L"renderer-shadow-depth-v68.csv";
  const bool depthHeader=!std::filesystem::exists(depthPath,error)||std::filesystem::file_size(depthPath,error)==0;
  GenZHLoad::AppendLog depthLog(depthPath,std::ios::app);
  if(depthLog){
   if(depthHeader)depthLog<<"pid,utc_epoch_ms,opaque_depth_requested,opaque_fast_draws,fallback_shadow_draws,shadow_GPU_ms,shadow_samples,light_prepare_GPU_ms,light_prepare_samples\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   depthLog<<GetCurrentProcessId()<<','<<epoch<<','<<opaqueShadowDepth_<<','<<workStats_.opaqueShadowDraws<<','<<workStats_.fallbackShadowDraws<<','<<(workStats_.shadowDrawSamples?workStats_.shadowDrawMs/workStats_.shadowDrawSamples:-1)<<','<<workStats_.shadowDrawSamples<<','<<(workStats_.lightPreparationSamples?workStats_.lightPreparationMs/workStats_.lightPreparationSamples:-1)<<','<<workStats_.lightPreparationSamples<<'\n';
  }
  const auto terrainPath=std::filesystem::path(profile)/L"renderer-terrain-v65.csv";
  const bool terrainHeader=!std::filesystem::exists(terrainPath,error)||std::filesystem::file_size(terrainPath,error)==0;
  GenZHLoad::AppendLog terrain(terrainPath,std::ios::app);
  if(terrain){
   if(terrainHeader)terrain<<"pid,utc_epoch_ms,frames,compact_terrain_draws,full_terrain_draws,pipeline_samples,IA_primitives,VS_invocations,GS_invocations,GS_primitives,HS_invocations,DS_invocations,PS_invocations\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   terrain<<GetCurrentProcessId()<<','<<epoch<<','<<stats_.frames<<','<<workStats_.terrainCompactDraws<<','<<workStats_.terrainFullDraws<<','<<workStats_.pipelineSamples;
   for(auto count:workStats_.pipelineCounts)terrain<<','<<(workStats_.pipelineSamples?double(count)/double(workStats_.pipelineSamples):-1);terrain<<'\n';
  }
  const auto nativePath=std::filesystem::path(profile)/L"renderer-native-v23.csv";
  const bool nativeHeader=!std::filesystem::exists(nativePath,error)||std::filesystem::file_size(nativePath,error)==0;
  GenZHLoad::AppendLog native(nativePath,std::ios::app);
  if(native){
   if(nativeHeader)native<<"pid,utc_epoch_ms,frames,tagged_packets,drawable_packets,rigid_packets,skinned_packets,sorted_packets,opaque_packets,cutout_packets,transparent_packets,overlay_packets,shader_packets,untagged_packets\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   const auto &n=nativeStats_;
   native<<GetCurrentProcessId()<<','<<epoch<<','<<stats_.frames<<','<<n.tagged<<','<<n.drawable<<','<<n.rigid<<','<<n.skinned<<','<<n.sorted<<','<<n.opaque<<','<<n.cutout<<','<<n.transparent<<','<<n.overlay<<','<<n.shader<<','<<n.untagged<<'\n';
  }
  const auto scenePath=std::filesystem::path(profile)/L"renderer-scene-v22.csv";
  const bool sceneHeader=!std::filesystem::exists(scenePath,error)||std::filesystem::file_size(scenePath,error)==0;
  GenZHLoad::AppendLog scene(scenePath,std::ios::app);
  if(scene){
   if(sceneHeader)scene<<"pid,utc_epoch_ms,frames,camera_culling_enabled,scene_seen,bounds_tested,camera_culled,fallback_retained,shadow_inputs,preparation_cpu_ms,exclusion_reuse,exclusion_builds,exclusion_uploads,road_vertices_transformed\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   scene<<GetCurrentProcessId()<<','<<epoch<<','<<stats_.frames<<','<<cameraCulling_<<','<<sceneStats_.seen<<','<<sceneStats_.boundsTested<<','<<sceneStats_.culled<<','<<sceneStats_.fallback<<','<<sceneStats_.shadowInputs<<','<<sceneStats_.preparationMs/static_cast<double>(stats_.frames)<<','<<sceneStats_.exclusionReuse<<','<<sceneStats_.exclusionBuilds<<','<<sceneStats_.exclusionUploads<<','<<sceneStats_.roadVerticesTransformed<<'\n';
  }
 }
 stats_=FrameStats{};workStats_=WorkStats{};sceneStats_=SceneStats{};nativeStats_=NativeSceneCounts{};
}
HRESULT CompatibilityRenderer::Present(){
 if(inScene_)return Fail(E_UNEXPECTED,"Present during scene");
 if(!ready_)return S_OK; // Native flip-to-primary may call Present repeatedly.
 const auto begin=std::chrono::steady_clock::now();HRESULT hr=renderer_.PresentFrame(false);const auto end=std::chrono::steady_clock::now();ready_=false;
 if(hr==S_OK)presentedFrames_.Add(completedSceneCpuMs_,std::chrono::duration<double,std::milli>(end-begin).count(),end);
 else {
  presentedFrames_.Reset();shadowStats_={};shadowCacheStats_={};terrainShadowStats_={};
  for(auto &cache:shadowReuse_)cache.Reset(); // Occlusion/device loss invalidates history.
 }
 // Successful presentation preserves both depth arrays. PrepareReset owns
 // resource destruction; an interval barrier must never destroy live DSVs.
 return FAILED(hr)?Fail(hr,renderer_.LastError()):hr;
}
HRESULT CompatibilityRenderer::Clear(DWORD flags,D3DCOLOR color,float depth,DWORD stencil){
 HRESULT hr=FlushScene();if(FAILED(hr))return hr;
 float rgba[4];PackedColor(color,rgba);
 if(!offscreen_)for(UINT i=0;i<3;++i)rgba[i]=std::pow(rgba[i],2.2f);
 if(!offscreen_&&(flags&D3DCLEAR_TARGET))std::copy_n(rgba,4,clear_);
 if(flags&D3DCLEAR_ZBUFFER)clearDepth_=depth;
 if(flags&D3DCLEAR_STENCIL)clearStencil_=stencil;
 if(!inScene_&&!offscreen_)return S_OK;
 ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> ds;
 renderer_.Context()->OMGetRenderTargets(1,target.GetAddressOf(),ds.GetAddressOf());
 if((flags&D3DCLEAR_TARGET)&&target)renderer_.Context()->ClearRenderTargetView(target.Get(),rgba);
 if(!offscreen_&&(flags&(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER))){renderer_.ScreenMaterials().BeginFrame();screenTargetsBound_=false;}
 UINT mask=((flags&D3DCLEAR_ZBUFFER)?D3D11_CLEAR_DEPTH:0)|((flags&D3DCLEAR_STENCIL)?D3D11_CLEAR_STENCIL:0);
 if(mask&&ds)renderer_.Context()->ClearDepthStencilView(ds.Get(),mask,depth,static_cast<UINT8>(stencil));
 return S_OK;
}

HRESULT CompatibilityRenderer::Snapshot(IDirect3DDevice8 *native,DrawPacket &packet,bool statesCaptured,const NativeFixedState *fixedState){
 CaptureWorkScope captureScope(CaptureWork::Snapshot,67);
 packet.nativeSource=GetNativeMeshSource();
 std::copy(packet.nativeSource.vehicleWear.age.begin(),packet.nativeSource.vehicleWear.age.end(),packet.constants.vehicleAge);
 std::copy(packet.nativeSource.vehicleWear.profile.begin(),packet.nativeSource.vehicleWear.profile.end(),packet.constants.vehicleProfile);
 auto &c=packet.constants;
 if(!statesCaptured){
  // perf112: the native reads fill a private copy that starts from the packet's current bytes (as the former in-place reads did), then the packet shares it.
  auto states=std::make_shared<StateSnapshot>(packet.States());
  for(UINT i=0;i<256;++i)if(i!=GENZH_D3DRS_DECAL)native->GetRenderState(static_cast<D3DRENDERSTATETYPE>(i),&states->render[i]);
  for(UINT stage=0;stage<8;++stage)for(UINT state=1;state<32;++state)
   native->GetTextureStageState(stage,static_cast<D3DTEXTURESTAGESTATETYPE>(state),&states->stages[stage][state]);
  packet.stateSnapshot=std::move(states);
 }
 auto rs=[&](D3DRENDERSTATETYPE key){return packet.States().render[static_cast<UINT>(key)];};
 D3DMATRIX matrix{};
 if(fixedState)matrix=fixedState->transforms[0];else native->GetTransform(D3DTS_WORLD,&matrix);std::memcpy(c.world,&matrix,64);
 if(fixedState)matrix=fixedState->transforms[1];else native->GetTransform(D3DTS_VIEW,&matrix);std::memcpy(c.view,&matrix,64);
 if(fixedState)matrix=fixedState->transforms[2];else native->GetTransform(D3DTS_PROJECTION,&matrix);std::memcpy(c.projection,&matrix,64);
 derivedFixed_.NormalWorld(c.world,c.normalWorld);
 D3DMATERIAL8 material{};if(fixedState)material=fixedState->material;else native->GetMaterial(&material);
 std::memcpy(c.materialDiffuse,&material.Diffuse,16);std::memcpy(c.materialAmbient,&material.Ambient,16);
 std::memcpy(c.materialSpecular,&material.Specular,16);std::memcpy(c.materialEmissive,&material.Emissive,16);
 PackedColor(rs(D3DRS_AMBIENT),c.globalAmbient);PackedColor(rs(D3DRS_TEXTUREFACTOR),c.textureFactor);
 PackedColor(rs(D3DRS_FOGCOLOR),c.fogColor);c.fogColor[3]=AsFloat(rs(D3DRS_FOGSTART));
 DWORD fog=rs(D3DRS_FOGTABLEMODE);if(!fog)fog=rs(D3DRS_FOGVERTEXMODE);
 c.fogSettings[0]=rs(D3DRS_FOGENABLE)?fog==D3DFOG_EXP?2.0f:fog==D3DFOG_EXP2?3.0f:1.0f:0;
 c.fogSettings[1]=AsFloat(rs(D3DRS_FOGDENSITY));c.fogSettings[2]=AsFloat(rs(D3DRS_FOGEND));c.fogSettings[3]=rs(D3DRS_RANGEFOGENABLE)?1.0f:0;
 D3DVIEWPORT8 viewport{};if(fixedState)viewport=fixedState->viewport;else native->GetViewport(&viewport);
 c.viewport[0]=static_cast<float>(viewport.X);c.viewport[1]=static_cast<float>(viewport.Y);
 c.viewport[2]=static_cast<float>(viewport.Width);c.viewport[3]=static_cast<float>(viewport.Height);
 c.geometryFlags[0]=(packet.fvf&D3DFVF_POSITION_MASK)==D3DFVF_XYZRHW?1.0f:0;
 c.geometryFlags[1]=rs(D3DRS_LIGHTING)?1.0f:0;c.geometryFlags[2]=rs(D3DRS_LOCALVIEWER)?1.0f:0;c.geometryFlags[3]=rs(D3DRS_SPECULARENABLE)?1.0f:0;
 c.materialFlags[0]=rs(D3DRS_COLORVERTEX)?static_cast<float>(rs(D3DRS_DIFFUSEMATERIALSOURCE)):0;
 c.materialFlags[1]=rs(D3DRS_COLORVERTEX)?static_cast<float>(rs(D3DRS_AMBIENTMATERIALSOURCE)):0;
 c.materialFlags[2]=rs(D3DRS_COLORVERTEX)?static_cast<float>(rs(D3DRS_SPECULARMATERIALSOURCE)):0;c.materialFlags[3]=material.Power;
 c.pointSettings[0]=AsFloat(rs(D3DRS_POINTSIZE));c.pointSettings[1]=AsFloat(rs(D3DRS_POINTSIZE_MIN));
 c.pointSettings[2]=AsFloat(rs(D3DRS_POINTSIZE_MAX));c.pointSettings[3]=rs(D3DRS_POINTSCALEENABLE)?1.0f:0;
 for(UINT i=0;i<4;++i){BOOL enabled=FALSE;D3DLIGHT8 light{};
  if(fixedState){enabled=fixedState->enabled[i];if(!enabled||!fixedState->lightValid[i])continue;light=fixedState->lights[i];}
  else {native->GetLightEnable(i,&enabled);if(!enabled||FAILED(native->GetLight(i,&light)))continue;}
  std::memcpy(c.lightDiffuse[i],&light.Diffuse,16);std::memcpy(c.lightAmbient[i],&light.Ambient,16);std::memcpy(c.lightSpecular[i],&light.Specular,16);
  std::memcpy(c.lightPosition[i],&light.Position,12);c.lightPosition[i][3]=static_cast<float>(light.Type);
  std::memcpy(c.lightDirection[i],&light.Direction,12);
  c.lightAttenuation[i][0]=light.Attenuation0;c.lightAttenuation[i][1]=light.Attenuation1;c.lightAttenuation[i][2]=light.Attenuation2;c.lightAttenuation[i][3]=light.Range;
  derivedFixed_.Spot(i,light.Theta,light.Phi,c.lightSpot[i]);c.lightSpot[i][2]=light.Falloff;
 }
 for(UINT i=0;i<8;++i){const auto &t=packet.States().stages[i];
  if(fixedState)matrix=fixedState->transforms[3+i];else native->GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0+i),&matrix);std::memcpy(c.textureMatrices[i],&matrix,64);
  c.colorStage[i][0]=t[D3DTSS_COLOROP];c.colorStage[i][1]=t[D3DTSS_COLORARG1];c.colorStage[i][2]=t[D3DTSS_COLORARG2];c.colorStage[i][3]=t[D3DTSS_RESULTARG];
  c.alphaStage[i][0]=t[D3DTSS_ALPHAOP];c.alphaStage[i][1]=t[D3DTSS_ALPHAARG1];c.alphaStage[i][2]=t[D3DTSS_ALPHAARG2];
  c.coordinateStage[i][0]=t[D3DTSS_TEXCOORDINDEX];c.coordinateStage[i][1]=t[D3DTSS_TEXTURETRANSFORMFLAGS];
  c.coordinateStage[i][2]=t[D3DTSS_COLORARG0];c.coordinateStage[i][3]=t[D3DTSS_ALPHAARG0];
  c.bump[i][0]=AsFloat(t[D3DTSS_BUMPENVMAT00]);c.bump[i][1]=AsFloat(t[D3DTSS_BUMPENVMAT01]);
  c.bump[i][2]=AsFloat(t[D3DTSS_BUMPENVMAT10]);c.bump[i][3]=AsFloat(t[D3DTSS_BUMPENVMAT11]);
  c.bumpLuma[i][0]=AsFloat(t[D3DTSS_BUMPENVLSCALE]);c.bumpLuma[i][1]=AsFloat(t[D3DTSS_BUMPENVLOFFSET]);
 }
 c.alphaStage[0][3]=rs(D3DRS_ALPHATESTENABLE)?rs(D3DRS_ALPHAFUNC):0;
 c.bridgeFlags[1]=static_cast<float>(rs(D3DRS_ALPHAREF)&255)/255;
 c.pointScale[0]=AsFloat(rs(D3DRS_POINTSCALE_A));c.pointScale[1]=AsFloat(rs(D3DRS_POINTSCALE_B));c.pointScale[2]=AsFloat(rs(D3DRS_POINTSCALE_C));
 c.pointScale[3]=rs(D3DRS_POINTSPRITEENABLE)?1.0f:0;
 c.bridgeFlags[0]=offscreen_?1.0f:0;c.bridgeFlags[2]=viewport.MinZ;c.bridgeFlags[3]=viewport.MaxZ;
 
 bool triangles=packet.primitive==D3DPT_TRIANGLELIST||packet.primitive==D3DPT_TRIANGLESTRIP;
 DWORD shader=0;if(fixedState)shader=fixedState->pixelShader;else native->GetPixelShader(&shader);
 c.modernMaterial[0]=(!materialCompatibility_&&!offscreen_&&!packet.vertexShader&&!shader&&triangles&&
  (packet.fvf&D3DFVF_NORMAL)&&c.geometryFlags[0]<.5f&&rs(D3DRS_LIGHTING)&&!rs(D3DRS_ALPHABLENDENABLE)&&(!rs(D3DRS_STENCILENABLE)||rs(D3DRS_ZWRITEENABLE)))?1.0f:0;
 c.modernMaterial[1]=derivedFixed_.Roughness(material.Power);
 c.modernMaterial[2]=0; // Legacy diffuse surfaces default to dielectric; authored ORM binding follows.
 derivedFixed_.Camera(c.view,c.cameraWorld);
 // Photoreal decal (track marks, building aprons): the native overlay sets GENZH_D3DRS_DECAL. Such a draw darkens the lit ground
 // multiplicatively (SRC=ZERO, DEST=SRCCOLOR) and cameraWorld.w tells LegacyPS to write the stain (Backend/D3D11/LegacyScene.hlsl).
 const bool decal=decalEnabled_&&packet.States().render[GENZH_D3DRS_DECAL]!=0;
 if(decal){auto &states=packet.MutableStates();states.render[D3DRS_SRCBLEND]=D3DBLEND_ZERO;states.render[D3DRS_DESTBLEND]=D3DBLEND_SRCCOLOR;}
 c.cameraWorld[3]=decal?1.0f:0;

 if(packet.vertexShader)native->GetVertexShaderConstant(0,packet.vertexConstants.Write(),96);
 if(packet.vertexShader&&vertexAssembly_.count(packet.vertexShader)){packet.vertexAssembly=vertexAssembly_[packet.vertexShader];}
 else if(packet.vertexShader){DWORD size=0;HRESULT hr=native->GetVertexShaderFunction(packet.vertexShader,nullptr,&size);
  if(FAILED(hr)||!size||size>65536)return Fail(E_FAIL,"invalid vertex program");
  std::vector<DWORD> code((size+3)/4);if(FAILED(hr=native->GetVertexShaderFunction(packet.vertexShader,code.data(),&size)))return hr;
  ComPtr<ID3DBlob> assembly;if(FAILED(hr=D3DDisassemble(code.data(),size,0,nullptr,assembly.GetAddressOf())))return hr;
  packet.vertexAssembly=std::make_shared<const std::string>(static_cast<const char*>(assembly->GetBufferPointer()));vertexAssembly_[packet.vertexShader]=packet.vertexAssembly;
 }
 packet.pixelShader=shader;if(packet.pixelShader)native->GetPixelShaderConstant(0,c.pixelConstants,8);
 if(packet.pixelShader&&pixelAssembly_.count(packet.pixelShader)){packet.pixelAssembly=pixelAssembly_[packet.pixelShader];}
 else if(packet.pixelShader){DWORD size=0;HRESULT hr=native->GetPixelShaderFunction(packet.pixelShader,nullptr,&size);
  if(FAILED(hr)||!size||size>65536)return Fail(E_FAIL,"invalid legacy pixel program");
  std::vector<DWORD> code((size+3)/4);if(FAILED(hr=native->GetPixelShaderFunction(packet.pixelShader,code.data(),&size)))return hr;
  ID3DBlob *assembly=nullptr;hr=D3DDisassemble(code.data(),size,0,nullptr,&assembly);
  if(FAILED(hr))return Fail(hr,"legacy pixel program disassembly");
  packet.pixelAssembly=std::make_shared<const std::string>(static_cast<const char*>(assembly->GetBufferPointer()),std::strlen(static_cast<const char*>(assembly->GetBufferPointer())));assembly->Release();pixelAssembly_[packet.pixelShader]=packet.pixelAssembly;
 }
 packet.outputOverlay=!offscreen_&&!packet.pixelShader&&c.geometryFlags[0]>.5&&!rs(D3DRS_ZENABLE)&&!rs(D3DRS_STENCILENABLE);
 // Terrain uses translated pixel programs with the fixed vertex transform, so
 // it can receive the same world-space shadow as fixed-function materials.
 if(modernShadows_&&!offscreen_&&!packet.vertexShader&&triangles&&c.geometryFlags[0]<.5f&&rs(D3DRS_ZENABLE)&&!rs(D3DRS_ALPHABLENDENABLE)&&(!rs(D3DRS_STENCILENABLE)||rs(D3DRS_ZWRITEENABLE)))c.modernMaterial[3]=static_cast<float>(4u|accumulationFlags_.load(std::memory_order_relaxed));
 // water107: the game's soft-edge water (its pixel programs, blended by destination alpha)
 // takes its shore opacity per pixel from the scene depth (ModernWater, LegacyScene.hlsl).
 // photo107: every opaque native water draw of the primary camera gets the screen-space reflection (8388608).
 if(waterReflectionEnabled_&&!offscreen_&&!packet.vertexShader&&shader&&triangles&&IsD3D11WaterProgram(shader)&&c.geometryFlags[0]<.5f)
  c.modernMaterial[3]=static_cast<float>(static_cast<UINT>(c.modernMaterial[3])|8388608u);
 packet.waterShore[0]=packet.waterShore[1]=packet.waterShore[2]=packet.waterShore[3]=0;
 if(waterShoreEnabled_&&!offscreen_&&shader&&triangles&&IsD3D11WaterProgram(shader)&&rs(D3DRS_ALPHABLENDENABLE)&&rs(D3DRS_SRCBLEND)==D3DBLEND_DESTALPHA&&
    rs(D3DRS_ZENABLE)&&!rs(D3DRS_ZWRITEENABLE)&&c.geometryFlags[0]<.5f){
  const auto &surface=GetD3D11WaterSurface();
  if(surface.softEdge&&!surface.additive&&surface.transparentDepth>0){
   c.modernMaterial[3]=static_cast<float>(static_cast<UINT>(c.modernMaterial[3])|2097152u);
   auto &states=packet.MutableStates();states.render[static_cast<UINT>(D3DRS_SRCBLEND)]=D3DBLEND_SRCALPHA;states.render[static_cast<UINT>(D3DRS_DESTBLEND)]=D3DBLEND_INVSRCALPHA;
   packet.stateRevision=0; // the states differ from the native mirror's revision now
   packet.waterShore[0]=surface.transparentDepth;packet.waterShore[1]=surface.minOpacity;packet.waterShore[2]=1;
  }
 }
 return S_OK;
}

HRESULT CompatibilityRenderer::GetProgram(const DrawPacket &packet,Program *&program,bool requireShadow,bool sortedPalette){
 const bool twoUV=packet.geometry&&packet.geometry->TwoUV();
 ProgramKey values{};values[0]=packet.pixelShader;values[1]=packet.vertexShader;values[10]=twoUV?1u:0u;values[11]=sortedPalette?1u:0u;
 for(unsigned i=0;i<8;++i)values[2+i]=packet.textures[i]?packet.textures[i]->native->GetType():0;
 const BinaryStateKey key(values);
 auto found=programs_.find(key);if(found!=programs_.end()&&(!requireShadow||found->second.shadowVertex)&&(!packet.terrain||found->second.terrainHull)){program=&found->second;return S_OK;}
 std::array<D3DRESOURCETYPE,8> types{};
 for(UINT i=0;i<8;++i)types[i]=packet.textures[i]?packet.textures[i]->native->GetType():static_cast<D3DRESOURCETYPE>(0);
 std::string source=BuildProgramSource(LegacyShaderSource,packet.vertexShader!=0,packet.pixelShader!=0,AssemblyText(packet.vertexAssembly),AssemblyText(packet.pixelAssembly),types,error_);
 if(source.empty())return Fail(E_NOTIMPL,error_);
 Program created;ComPtr<ID3DBlob> vs,ps,gs,shadowVS,shadowPS;
 if(found!=programs_.end())created=found->second;
 auto compile=[&](const char *entry,const char *profile,ComPtr<ID3DBlob> &blob){
  bool hit=false;std::string error;HRESULT hr=CompileCachedShader(source,entry,profile,blob,error,hit);
  if(hit)++stats_.shaderCacheHits;else ++stats_.shaderCompiles;
  if(FAILED(hr))Fail(hr,entry+std::string(": ")+error);return hr;};
 HRESULT hr;
 if(!created.vertex){
 if(sortedPalette){
  const std::array<D3DRESOURCETYPE,8> canonicalTypes{};std::string detail;
  const auto canonical=BuildProgramSource(LegacyShaderSource,false,false,{},{},canonicalTypes,detail);
  if(canonical.empty())return Fail(E_NOTIMPL,detail);
  bool hit=false;hr=CompileCachedShader(SortedPaletteVertexSource(canonical),"LegacyVS","vs_5_0",vs,detail,hit);
  if(hit)++stats_.shaderCacheHits;else ++stats_.shaderCompiles;
  if(FAILED(hr))return Fail(hr,"sorted palette VS: "+detail);
 }else if(FAILED(hr=compile("LegacyVS","vs_5_0",vs)))return hr;
 if(FAILED(hr=compile("LegacyPS","ps_5_0",ps))||FAILED(hr=compile("LegacyPointGS","gs_5_0",gs)))return hr;
 if(FAILED(hr=renderer_.Device()->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,created.vertex.GetAddressOf()))||
  FAILED(hr=renderer_.Device()->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,created.pixel.GetAddressOf()))||
  FAILED(hr=renderer_.Device()->CreateGeometryShader(gs->GetBufferPointer(),gs->GetBufferSize(),nullptr,created.point.GetAddressOf())))return Fail(hr,"legacy shader objects");
 std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
 layout.push_back({"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0});
 layout.push_back({"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0});
 layout.push_back({"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0});
 layout.push_back({"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0});
 for(UINT i=0;i<8;++i)layout.push_back({"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,twoUV&&i>=2?48:64+i*16,D3D11_INPUT_PER_VERTEX_DATA,0});
 if(packet.vertexShader)for(UINT i=0;i<16;++i)layout.push_back({"BLENDWEIGHT",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,192+i*16,D3D11_INPUT_PER_VERTEX_DATA,0});
 if(FAILED(hr=renderer_.Device()->CreateInputLayout(layout.data(),static_cast<UINT>(layout.size()),vs->GetBufferPointer(),vs->GetBufferSize(),created.layout.GetAddressOf())))return Fail(hr,"legacy vertex layout");
 }
 if(requireShadow){
  if(!created.opaqueShadowVertex){
   ComPtr<ID3DBlob> opaqueVS;bool hit=false;std::string error;
   hr=CompileCachedShader(source+OpaqueShadowShaderSuffix,"LegacyOpaqueShadowVS","vs_5_0",opaqueVS,error,hit);
   if(hit)++stats_.shaderCacheHits;else ++stats_.shaderCompiles;
   if(FAILED(hr))return Fail(hr,error);
   if(FAILED(hr=renderer_.Device()->CreateVertexShader(opaqueVS->GetBufferPointer(),opaqueVS->GetBufferSize(),nullptr,created.opaqueShadowVertex.GetAddressOf())))return Fail(hr,"opaque depth-only shadow shader");
  }
  if(FAILED(hr=compile("LegacyShadowVS","vs_5_0",shadowVS))||FAILED(hr=compile("LegacyShadowPS","ps_5_0",shadowPS)))return hr;
  if(FAILED(hr=renderer_.Device()->CreateVertexShader(shadowVS->GetBufferPointer(),shadowVS->GetBufferSize(),nullptr,created.shadowVertex.ReleaseAndGetAddressOf()))||
   FAILED(hr=renderer_.Device()->CreatePixelShader(shadowPS->GetBufferPointer(),shadowPS->GetBufferSize(),nullptr,created.shadowPixel.ReleaseAndGetAddressOf())))return Fail(hr,"scene shadow shaders");
 }
 if(packet.terrain&&!created.terrainHull){
  ComPtr<ID3DBlob> hull,domain,grass;
  if(FAILED(hr=compile("TerrainHS","hs_5_0",hull))||FAILED(hr=compile("TerrainDS","ds_5_0",domain))||FAILED(hr=compile("TerrainGrassGS","gs_5_0",grass)))return hr;
  if(FAILED(hr=renderer_.Device()->CreateHullShader(hull->GetBufferPointer(),hull->GetBufferSize(),nullptr,created.terrainHull.GetAddressOf()))||
     FAILED(hr=renderer_.Device()->CreateDomainShader(domain->GetBufferPointer(),domain->GetBufferSize(),nullptr,created.terrainDomain.GetAddressOf()))||
     FAILED(hr=renderer_.Device()->CreateGeometryShader(grass->GetBufferPointer(),grass->GetBufferSize(),nullptr,created.terrainGrass.GetAddressOf())))return Fail(hr,"terrain tessellation shaders");
  ComPtr<ID3DBlob> compact,colorInput,shadowInput;
  if(FAILED(hr=compile("TerrainGrassCompactGS","gs_5_0",compact))||FAILED(hr=compile("LegacyPS","ps_5_0",colorInput))||FAILED(hr=compile("LegacyShadowPS","ps_5_0",shadowInput)))return hr;
  const auto compactSource=source+TerrainCompactShaderSuffix;
  ComPtr<ID3DBlob> compactColor,compactShadow;
  auto compileCompact=[&](const char *entry,ComPtr<ID3DBlob> &blob){
   bool hit=false;std::string error;const HRESULT result=CompileCachedShader(compactSource,entry,"ps_5_0",blob,error,hit);
   if(hit)++stats_.shaderCacheHits;else ++stats_.shaderCompiles;
   if(FAILED(result))Fail(result,error);return result;
  };
  if(FAILED(hr=compileCompact("TerrainCompactPS",compactColor))||FAILED(hr=compileCompact("TerrainCompactShadowPS",compactShadow)))return hr;
  created.terrainCompactColor=CanLinkRasterStage(compact.Get(),colorInput.Get())&&CanLinkRasterStage(compact.Get(),compactColor.Get(),true);
  created.terrainCompactShadow=CanLinkRasterStage(compact.Get(),shadowInput.Get())&&CanLinkRasterStage(compact.Get(),compactShadow.Get(),true);
  if(created.terrainCompactColor&&FAILED(hr=renderer_.Device()->CreatePixelShader(compactColor->GetBufferPointer(),compactColor->GetBufferSize(),nullptr,created.terrainPixel.GetAddressOf())))return Fail(hr,"compact terrain material PS");
  if(created.terrainCompactShadow&&FAILED(hr=renderer_.Device()->CreatePixelShader(compactShadow->GetBufferPointer(),compactShadow->GetBufferSize(),nullptr,created.terrainShadowPixel.GetAddressOf())))return Fail(hr,"compact terrain shadow PS");
  if((created.terrainCompactColor||created.terrainCompactShadow)&&FAILED(hr=renderer_.Device()->CreateGeometryShader(compact->GetBufferPointer(),compact->GetBufferSize(),nullptr,created.terrainGrassCompact.GetAddressOf())))return Fail(hr,"compact terrain raster stage");
 }
 auto &cached=programs_[key];cached=std::move(created);program=&cached;return S_OK;
}

HRESULT CompatibilityRenderer::GetTexture(const std::shared_ptr<TextureData> &texture,GPUTexture *&result){
 if(!texture){result=nullptr;return S_OK;}
 auto &cached=textures_[texture->native.Get()];
 if(cached.owner.expired()){cached=GPUTexture{};cached.owner=texture;}
 if(cached.view&&cached.revision==texture->revision){result=&cached;return S_OK;}
 struct UploadNote{CompatibilityRenderer &renderer;IDirect3DBaseTexture8 *native;bool fresh;~UploadNote(){renderer.NoteTextureUpload(native,fresh);}} uploadNote{*this,texture->native.Get(),!cached.view};
 HRESULT hr=S_OK;auto *device=renderer_.Device();auto *context=renderer_.Context();
 auto read2D=[&](IDirect3DSurface8 *surface,UINT width,UINT height,std::vector<std::uint8_t> &rgba){
  ComPtr<IDirect3DSurface8> image;HRESULT status=native_->CreateImageSurface(width,height,D3DFMT_A8R8G8B8,image.GetAddressOf());if(FAILED(status))return status;
  if(FAILED(status=D3DXLoadSurfaceFromSurface(image.Get(),nullptr,nullptr,surface,nullptr,nullptr,D3DX_FILTER_NONE,0)))return status;
  D3DLOCKED_RECT locked{};if(FAILED(status=image->LockRect(&locked,nullptr,D3DLOCK_READONLY)))return status;
  rgba.resize(std::size_t(width)*height*4);
  for(UINT y=0;y<height;++y){auto *src=static_cast<const std::uint8_t*>(locked.pBits)+std::size_t(y)*locked.Pitch;
   auto *dst=rgba.data()+std::size_t(y)*width*4;for(UINT x=0;x<width;++x){dst[x*4]=src[x*4+2];dst[x*4+1]=src[x*4+1];dst[x*4+2]=src[x*4];dst[x*4+3]=src[x*4+3];}}
  return image->UnlockRect();
 };
 auto type=texture->native->GetType();
 if(type==D3DRTYPE_TEXTURE||type==D3DRTYPE_CUBETEXTURE){
  bool cube=type==D3DRTYPE_CUBETEXTURE;D3DSURFACE_DESC desc{};
  if(cube)hr=static_cast<IDirect3DCubeTexture8*>(texture->native.Get())->GetLevelDesc(0,&desc);
  else hr=static_cast<IDirect3DTexture8*>(texture->native.Get())->GetLevelDesc(0,&desc);
  if(FAILED(hr))return hr;
  bool signedBump=desc.Format==D3DFMT_V8U8||desc.Format==D3DFMT_L6V5U5||desc.Format==D3DFMT_X8L8V8U8||desc.Format==D3DFMT_Q8W8V8U8||desc.Format==D3DFMT_V16U16;
  if(signedBump){UINT levels=texture->native->GetLevelCount(),faces=cube?6:1;
   std::vector<std::vector<float>> pixels(std::size_t(levels)*faces);std::vector<D3D11_SUBRESOURCE_DATA> initial(pixels.size());
   for(UINT face=0;face<faces;++face)for(UINT mip=0;mip<levels;++mip){ComPtr<IDirect3DSurface8> surface;
    if(cube)hr=static_cast<IDirect3DCubeTexture8*>(texture->native.Get())->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face),mip,surface.GetAddressOf());
    else hr=static_cast<IDirect3DTexture8*>(texture->native.Get())->GetSurfaceLevel(mip,surface.GetAddressOf());
    if(FAILED(hr))return hr;D3DSURFACE_DESC level{};surface->GetDesc(&level);D3DLOCKED_RECT lock{};
    if(FAILED(hr=surface->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return Fail(hr,"signed bump map lock");UINT subresource=D3D11CalcSubresource(mip,face,levels);
    hr=DecodeBumpTexture(lock.pBits,std::size_t(lock.Pitch)*level.Height,static_cast<UINT>(lock.Pitch),level.Width,level.Height,level.Format,pixels[subresource]);surface->UnlockRect();if(FAILED(hr))return hr;
    initial[subresource].pSysMem=pixels[subresource].data();initial[subresource].SysMemPitch=level.Width*16;
   }
   D3D11_TEXTURE2D_DESC gpu{};gpu.Width=desc.Width;gpu.Height=desc.Height;gpu.MipLevels=levels;gpu.ArraySize=faces;gpu.SampleDesc.Count=1;
   gpu.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;gpu.Usage=D3D11_USAGE_IMMUTABLE;gpu.BindFlags=D3D11_BIND_SHADER_RESOURCE;gpu.MiscFlags=cube?D3D11_RESOURCE_MISC_TEXTURECUBE:0;
   ComPtr<ID3D11Texture2D> resource;if(FAILED(hr=device->CreateTexture2D(&gpu,initial.data(),resource.GetAddressOf())))return hr;
   if(FAILED(hr=device->CreateShaderResourceView(resource.Get(),nullptr,cached.view.ReleaseAndGetAddressOf())))return hr;
   cached.texture=resource;cached.revision=texture->revision;result=&cached;return S_OK;
  }
  bool target=(desc.Usage&D3DUSAGE_RENDERTARGET)!=0;
  if(target&&cached.target){cached.revision=texture->revision;result=&cached;return S_OK;}
  D3D11_TEXTURE2D_DESC gpu{};gpu.Width=desc.Width;gpu.Height=desc.Height;gpu.MipLevels=target?1:0;
  gpu.ArraySize=cube?6:1;gpu.Format=DXGI_FORMAT_R8G8B8A8_UNORM;gpu.SampleDesc.Count=1;gpu.Usage=D3D11_USAGE_DEFAULT;
  gpu.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
  gpu.MiscFlags=(target?0:D3D11_RESOURCE_MISC_GENERATE_MIPS)|(cube?D3D11_RESOURCE_MISC_TEXTURECUBE:0);
  ComPtr<ID3D11Texture2D> resource;if(FAILED(hr=device->CreateTexture2D(&gpu,nullptr,resource.GetAddressOf())))return hr;
  if(FAILED(hr=device->CreateShaderResourceView(resource.Get(),nullptr,cached.view.ReleaseAndGetAddressOf())))return hr;
  if(target){D3D11_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=gpu.Format;
   rtv.ViewDimension=cube?D3D11_RTV_DIMENSION_TEXTURE2DARRAY:D3D11_RTV_DIMENSION_TEXTURE2D;
   if(cube)rtv.Texture2DArray.ArraySize=1;
   if(FAILED(hr=device->CreateRenderTargetView(resource.Get(),&rtv,cached.target.ReleaseAndGetAddressOf())))return hr;
   float clear[4]{};context->ClearRenderTargetView(cached.target.Get(),clear);
  }else{
   resource->GetDesc(&gpu);
   for(UINT face=0;face<gpu.ArraySize;++face){ComPtr<IDirect3DSurface8> surface;
    if(cube)hr=static_cast<IDirect3DCubeTexture8*>(texture->native.Get())->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face),0,surface.GetAddressOf());
    else hr=static_cast<IDirect3DTexture8*>(texture->native.Get())->GetSurfaceLevel(0,surface.GetAddressOf());
    if(FAILED(hr))return hr;std::vector<std::uint8_t> rgba;if(FAILED(hr=read2D(surface.Get(),desc.Width,desc.Height,rgba)))return Fail(hr,"texture conversion");
    if(!cube&&texture->copyVerified){ // perf87 CopyRects verify mode
     if(texture->expectedRevision&&texture->expectedRevision==texture->revision&&cached.verifyRevision==texture->baseRevision&&!cached.verifyRGBA.empty()){
      ++copyRectsStats_.verified;if(cached.verifyRGBA!=rgba)++copyRectsStats_.mismatches;}
     texture->expectedRevision=0;cached.verifyRGBA=rgba;cached.verifyRevision=texture->revision;
    }
    context->UpdateSubresource(resource.Get(),D3D11CalcSubresource(0,face,gpu.MipLevels),nullptr,rgba.data(),desc.Width*4,0);
   }
   context->GenerateMips(cached.view.Get());
  }
  cached.texture=resource;
 }else if(type==D3DRTYPE_VOLUMETEXTURE){
  auto *volume=static_cast<IDirect3DVolumeTexture8*>(texture->native.Get());D3DVOLUME_DESC desc{};if(FAILED(hr=volume->GetLevelDesc(0,&desc)))return hr;
  ComPtr<IDirect3DVolumeTexture8> image;ComPtr<IDirect3DVolume8> src,dest;
  if(FAILED(hr=native_->CreateVolumeTexture(desc.Width,desc.Height,desc.Depth,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,image.GetAddressOf()))||
   FAILED(hr=volume->GetVolumeLevel(0,src.GetAddressOf()))||FAILED(hr=image->GetVolumeLevel(0,dest.GetAddressOf())))return hr;
  if(FAILED(hr=D3DXLoadVolumeFromVolume(dest.Get(),nullptr,nullptr,src.Get(),nullptr,nullptr,D3DX_FILTER_NONE,0)))return hr;
  D3DLOCKED_BOX lock{};if(FAILED(hr=image->LockBox(0,&lock,nullptr,D3DLOCK_READONLY)))return hr;
  std::vector<std::uint8_t> rgba(std::size_t(desc.Width)*desc.Height*desc.Depth*4);
  for(UINT z=0;z<desc.Depth;++z)for(UINT y=0;y<desc.Height;++y){auto *from=static_cast<const std::uint8_t*>(lock.pBits)+std::size_t(z)*lock.SlicePitch+std::size_t(y)*lock.RowPitch;
   auto *to=rgba.data()+(std::size_t(z)*desc.Height+y)*desc.Width*4;for(UINT x=0;x<desc.Width;++x){to[x*4]=from[x*4+2];to[x*4+1]=from[x*4+1];to[x*4+2]=from[x*4];to[x*4+3]=from[x*4+3];}}
  image->UnlockBox(0);D3D11_TEXTURE3D_DESC gpu{};gpu.Width=desc.Width;gpu.Height=desc.Height;gpu.Depth=desc.Depth;gpu.MipLevels=0;
  gpu.Format=DXGI_FORMAT_R8G8B8A8_UNORM;gpu.Usage=D3D11_USAGE_DEFAULT;gpu.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;gpu.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ComPtr<ID3D11Texture3D> resource;if(FAILED(hr=device->CreateTexture3D(&gpu,nullptr,resource.GetAddressOf())))return hr;
  if(FAILED(hr=device->CreateShaderResourceView(resource.Get(),nullptr,cached.view.ReleaseAndGetAddressOf())))return hr;
  context->UpdateSubresource(resource.Get(),0,nullptr,rgba.data(),desc.Width*4,desc.Width*desc.Height*4);context->GenerateMips(cached.view.Get());cached.texture=resource;
 }else return Fail(E_NOTIMPL,"unsupported texture resource type");
 cached.revision=texture->revision;result=&cached;return S_OK;
}

void CompatibilityRenderer::NoteTextureUpload(IDirect3DBaseTexture8 *native,bool fresh){
 auto &stat=textureUploads_[native];++stat.count;if(fresh)++stat.fresh;
 if(!stat.detail.empty()||!native)return;
 std::ostringstream detail;detail<<"type="<<native->GetType()<<" levels="<<native->GetLevelCount();
 if(native->GetType()==D3DRTYPE_TEXTURE){D3DSURFACE_DESC desc{};
  if(SUCCEEDED(static_cast<IDirect3DTexture8*>(native)->GetLevelDesc(0,&desc)))detail<<" format="<<desc.Format<<" size="<<desc.Width<<"x"<<desc.Height<<" usage="<<desc.Usage<<" pool="<<desc.Pool;}
 char name[256];DWORD size=sizeof(name);
 if(SUCCEEDED(native->GetPrivateData(D3D11TextureNameGUID,name,&size))&&size&&size<=sizeof(name)&&name[size-1]==0)detail<<" name="<<name;
 stat.detail=detail.str();
}

HRESULT CompatibilityRenderer::CaptureTextures(DrawPacket &packet){
 CaptureWorkScope captureScope(CaptureWork::TextureCapture,67);
 HRESULT hr=S_OK;
 for(UINT i=0;i<8;++i){GPUTexture *texture=nullptr;
  if(FAILED(hr=GetTexture(packet.textures[i],texture)))return Fail(hr,"capture draw texture");
  if(texture)packet.capturedTextures[i]=texture->view;
 }
 if(FAILED(hr=CaptureTerrain(packet)))return hr;
 if(packet.terrain)return S_OK;
 if(!packet.textures[0])return S_OK;
 char name[1024];DWORD bytes=sizeof(name);
 if(FAILED(packet.textures[0]->native->GetPrivateData(D3D11TextureNameGUID,name,&bytes))||!bytes||bytes>sizeof(name)||name[bytes-1]!=0)return S_OK;
 // The private name stays live on every draw. Cache only derived metadata;
 // registry changes and material-root changes invalidate their own domains.
 auto &metadata=textures_.at(packet.textures[0]->native.Get());
 const bool nameChanged=metadata.materialName!=name;
 const auto overlayRevision=GetD3D11GroundOverlayRevision();
 if(nameChanged||metadata.overlayRevision!=overlayRevision){
  metadata.groundOverlay=IsD3D11GroundOverlayTexture(name);
  std::string groundLabel(name);std::transform(groundLabel.begin(),groundLabel.end(),groundLabel.begin(),[](unsigned char v){return static_cast<char>(std::tolower(v));});
  metadata.groundExclusionOverlay=metadata.groundOverlay&&groundLabel.find("shadow")==std::string::npos&&groundLabel.find("water")==std::string::npos&&groundLabel.find("smudge")==std::string::npos;
  metadata.wearTexture=std::strstr(name,"_wear.tga")!=nullptr;
  metadata.overlayRevision=overlayRevision;
 }
 if(nameChanged||metadata.materialRootRevision!=materialRootRevision_){
  metadata.materialName=name;metadata.materialRootRevision=materialRootRevision_;
  metadata.materialLookup=MaterialLookup(materialRoot_,std::filesystem::path(name).filename().stem());
  metadata.materialMemo={};metadata.materialAudited=false;
 }
 packet.groundOverlay=metadata.groundOverlay;packet.groundExclusionOverlay=metadata.groundExclusionOverlay;
 if(packet.constants.vehicleAge[3]>.5f&&metadata.wearTexture)packet.constants.vehicleProfile[3]=1;
 static bool authoredAudited=false;
 if(!authoredAudited&&std::strstr(name,"Z26Armor")){
  std::ostringstream sample;sample<<"Authored draw input: "<<name<<" FVF="<<packet.fvf<<" VS="<<packet.vertexShader<<" PS="<<packet.pixelShader<<" lighting="<<packet.constants.geometryFlags[1]<<" blend="<<packet.States().render[D3DRS_ALPHABLENDENABLE]<<" GGX="<<packet.constants.modernMaterial[0];D3D11Diagnostic(sample.str().c_str());authoredAudited=true;
 }
 if(materialRoot_.empty())return S_OK;
 // GetPrivateData above remains live: renamed/reused textures are observed even
 // within a scene. Metadata never retains GPU views across eviction/reset.
 const auto &lookup=metadata.materialLookup;if(!lookup.Valid())return S_OK;
 const auto &stem=lookup.Stem();const auto &key=lookup.Key();
 // Queue metadata only. Disk I/O, DDS parsing and TGA decoding never occur in
 // this draw callback. A complete native material stays bound until readiness.
 const auto *ready=materialStreaming_.FindOrRequest(lookup,metadata.materialMemo);if(!ready)return S_OK;
 const auto &authored=ready->maps;
 if(authored[0])packet.capturedTextures[0]=authored[0];
 // Authored opaque world materials must not lose their normal/ORM maps merely
 // because a retail material disabled fixed-function vertex lighting.
 
 if(!materialCompatibility_&&(authored[1]||authored[2])&&!packet.vertexShader&&!packet.outputOverlay&&packet.constants.bridgeFlags[0]<.5f&&
    (packet.fvf&D3DFVF_NORMAL)&&packet.constants.geometryFlags[0]<.5f&&packet.States().render[D3DRS_ZENABLE]&&
    packet.States().render[D3DRS_ZWRITEENABLE]&&!packet.States().render[D3DRS_ALPHABLENDENABLE])packet.constants.modernMaterial[0]=1;
 if(packet.constants.vehicleAge[3]>.5f&&packet.constants.vehicleProfile[3]>.5f&&
    (authored[1]||authored[2])&&!packet.vertexShader&&!packet.outputOverlay&&
    (packet.fvf&D3DFVF_NORMAL)&&!materialCompatibility_)
    packet.constants.modernMaterial[0]=1;
 if(packet.constants.modernMaterial[0]<.5f)return S_OK;
 ComPtr<ID3D11ShaderResourceView> maps[]={authored[1],authored[2]};UINT flags=0;
 for(UINT i=0;i<2;++i){if(maps[i]){packet.materialTextures[i]=maps[i];flags|=1u<<i;}}
 packet.heightTexture=authored[3];packet.emissiveTexture=authored[4];
 for(unsigned i=0;i<3;++i)packet.wearTextures[i]=authored[5+i];
 if(packet.wearTextures[0])flags|=256;if(packet.wearTextures[1])flags|=512;if(packet.wearTextures[2])flags|=1024;
 if(packet.heightTexture)flags|=64;if(packet.emissiveTexture)flags|=128;
 packet.constants.modernMaterial[3]=static_cast<float>(static_cast<UINT>(packet.constants.modernMaterial[3])|flags);
 static std::set<std::wstring> audited;
 // One diagnostic per key, as before; the per-texture flag skips the set search on later draws.
 if(!metadata.materialAudited){metadata.materialAudited=true;if(audited.find(key)==audited.end()){audited.insert(key);D3D11Diagnostic(("Material v16 active: "+stem.string()+" maps="+std::to_string(flags)+" FVF="+std::to_string(packet.fvf)).c_str());}}
 return S_OK;
}


HRESULT CompatibilityRenderer::BindStatesOriginal(const DrawPacket &packet){
 auto *device=renderer_.Device();auto *context=renderer_.Context();const auto &r=packet.States().render;
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=r[D3DRS_FILLMODE]==D3DFILL_WIREFRAME?D3D11_FILL_WIREFRAME:D3D11_FILL_SOLID;
 raster.CullMode=r[D3DRS_CULLMODE]==D3DCULL_NONE?D3D11_CULL_NONE:D3D11_CULL_BACK;raster.FrontCounterClockwise=r[D3DRS_CULLMODE]==D3DCULL_CW;
 if(packet.terrain&&(static_cast<UINT>(packet.constants.modernMaterial[3])&32))raster.CullMode=D3D11_CULL_NONE;
 raster.DepthClipEnable=TRUE;raster.DepthBias=-static_cast<int>(r[D3DRS_ZBIAS]);
 auto &rasterState=rasterStates_.Get(raster);HRESULT hr=S_OK;
 if(!rasterState&&FAILED(hr=device->CreateRasterizerState(&raster,rasterState.GetAddressOf())))return Fail(hr,"raster state");
 sceneBindings_.Single(ScenePipelineBindings::Raster,rasterState.Get(),[&]{context->RSSetState(rasterState.Get());});
 D3D11_BLEND_DESC blend{};
 if(screenTargetsBound_&&!offscreen_&&!packet.outputOverlay){
  blend.IndependentBlendEnable=TRUE;
  for(unsigned target=1;target<8;++target){auto &aux=blend.RenderTarget[target];aux.BlendEnable=FALSE;aux.SrcBlend=aux.SrcBlendAlpha=D3D11_BLEND_ONE;aux.DestBlend=aux.DestBlendAlpha=D3D11_BLEND_ZERO;aux.BlendOp=aux.BlendOpAlpha=D3D11_BLEND_OP_ADD;aux.RenderTargetWriteMask=static_cast<UINT8>(ScreenMaterialGPU::AuxiliaryWriteMask(r[D3DRS_COLORWRITEENABLE],r[D3DRS_ZWRITEENABLE]!=0));}
 }
 auto &b=blend.RenderTarget[0];b.BlendEnable=r[D3DRS_ALPHABLENDENABLE]!=0;
 DWORD src=r[D3DRS_SRCBLEND],dst=r[D3DRS_DESTBLEND];
 if(src==D3DBLEND_BOTHSRCALPHA){src=D3DBLEND_SRCALPHA;dst=D3DBLEND_INVSRCALPHA;}
 if(src==D3DBLEND_BOTHINVSRCALPHA){src=D3DBLEND_INVSRCALPHA;dst=D3DBLEND_SRCALPHA;}
 b.SrcBlend=Blend(src);b.DestBlend=Blend(dst);b.BlendOp=static_cast<D3D11_BLEND_OP>(r[D3DRS_BLENDOP]);
 b.SrcBlendAlpha=AlphaBlend(b.SrcBlend);b.DestBlendAlpha=AlphaBlend(b.DestBlend);b.BlendOpAlpha=b.BlendOp;
 // DX8 ignores blend operands when blending is disabled. D3D11 still validates
 // its descriptor; unused legacy operands must not reject an opaque draw.
 if(!b.BlendEnable){b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlend=b.DestBlendAlpha=D3D11_BLEND_ZERO;b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;}
 b.RenderTargetWriteMask=static_cast<UINT8>(r[D3DRS_COLORWRITEENABLE]&D3D11_COLOR_WRITE_ENABLE_ALL);
 auto &blendState=blendStates_.Get(blend);
 if(!blendState&&FAILED(hr=device->CreateBlendState(&blend,blendState.GetAddressOf())))return Fail(hr,"blend state enabled="+std::to_string(b.BlendEnable)+" src="+std::to_string(b.SrcBlend)+" dest="+std::to_string(b.DestBlend)+" op="+std::to_string(b.BlendOp)+" mask="+std::to_string(b.RenderTargetWriteMask));
 sceneBindings_.Single(ScenePipelineBindings::Blend,blendState.Get(),[&]{context->OMSetBlendState(blendState.Get(),nullptr,0xffffffff);});
 D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=r[D3DRS_ZENABLE]!=0;depth.DepthWriteMask=r[D3DRS_ZWRITEENABLE]?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
 depth.DepthFunc=static_cast<D3D11_COMPARISON_FUNC>(r[D3DRS_ZFUNC]);depth.StencilEnable=r[D3DRS_STENCILENABLE]!=0;
 depth.StencilReadMask=static_cast<UINT8>(r[D3DRS_STENCILMASK]);depth.StencilWriteMask=static_cast<UINT8>(r[D3DRS_STENCILWRITEMASK]);
 depth.FrontFace.StencilFailOp=static_cast<D3D11_STENCIL_OP>(r[D3DRS_STENCILFAIL]);depth.FrontFace.StencilDepthFailOp=static_cast<D3D11_STENCIL_OP>(r[D3DRS_STENCILZFAIL]);
 depth.FrontFace.StencilPassOp=static_cast<D3D11_STENCIL_OP>(r[D3DRS_STENCILPASS]);depth.FrontFace.StencilFunc=static_cast<D3D11_COMPARISON_FUNC>(r[D3DRS_STENCILFUNC]);depth.BackFace=depth.FrontFace;
 auto &depthState=depthStates_.Get(depth);
 if(!depthState&&FAILED(hr=device->CreateDepthStencilState(&depth,depthState.GetAddressOf())))return Fail(hr,"depth/stencil state");
 sceneBindings_.Depth(depthState.Get(),r[D3DRS_STENCILREF],[&]{context->OMSetDepthStencilState(depthState.Get(),r[D3DRS_STENCILREF]);});
 D3D11_VIEWPORT viewport{packet.constants.viewport[0],packet.constants.viewport[1],packet.constants.viewport[2],packet.constants.viewport[3],packet.constants.bridgeFlags[2],packet.constants.bridgeFlags[3]};sceneBindings_.Viewport({viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height,viewport.MinDepth,viewport.MaxDepth},[&]{context->RSSetViewports(1,&viewport);});
 std::array<ID3D11SamplerState*,8> boundSamplers{};
 for(UINT i=0;i<8;++i){auto &t=packet.States().stages[i];D3D11_SAMPLER_DESC sampler{};
  sampler.AddressU=Address(t[D3DTSS_ADDRESSU]);sampler.AddressV=Address(t[D3DTSS_ADDRESSV]);sampler.AddressW=Address(t[D3DTSS_ADDRESSW]);
  sampler.Filter=t[D3DTSS_MINFILTER]==D3DTEXF_ANISOTROPIC?D3D11_FILTER_ANISOTROPIC:
   D3D11_ENCODE_BASIC_FILTER(t[D3DTSS_MINFILTER]==D3DTEXF_POINT?D3D11_FILTER_TYPE_POINT:D3D11_FILTER_TYPE_LINEAR,
    t[D3DTSS_MAGFILTER]==D3DTEXF_POINT?D3D11_FILTER_TYPE_POINT:D3D11_FILTER_TYPE_LINEAR,
    t[D3DTSS_MIPFILTER]==D3DTEXF_LINEAR?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,D3D11_FILTER_REDUCTION_TYPE_STANDARD);
  sampler.MaxAnisotropy=std::clamp(t[D3DTSS_MAXANISOTROPY],DWORD(1),DWORD(16));sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;
  sampler.MipLODBias=AsFloat(t[D3DTSS_MIPMAPLODBIAS]);sampler.MinLOD=static_cast<float>(t[D3DTSS_MAXMIPLEVEL]);
  sampler.MaxLOD=t[D3DTSS_MIPFILTER]==D3DTEXF_NONE?sampler.MinLOD:D3D11_FLOAT32_MAX;PackedColor(t[D3DTSS_BORDERCOLOR],sampler.BorderColor);
  // Modern opaque surfaces use the mip chains already generated on upload.
  // Native HUD and blended effects keep their own sampling contract.
  if(packet.constants.modernMaterial[0]>.5f&&!packet.outputOverlay&&!packet.States().render[D3DRS_ALPHABLENDENABLE]){
   sampler.Filter=D3D11_FILTER_ANISOTROPIC;sampler.MaxAnisotropy=8;sampler.MinLOD=0;sampler.MaxLOD=D3D11_FLOAT32_MAX;sampler.MipLODBias=0;
  }
  auto &state=samplerStates_.Get(sampler);
  if(!state&&FAILED(hr=device->CreateSamplerState(&sampler,state.GetAddressOf())))return hr;
  boundSamplers[i]=state.Get();
 }
 sceneBindings_.Samplers(boundSamplers,[&]{context->PSSetSamplers(0,8,boundSamplers.data());});
 return S_OK;
}

HRESULT CompatibilityRenderer::BindStates(const DrawPacket &packet){
 if(!resolvedStatesEnabled_||resolvedStatesReference)return BindStatesOriginal(packet);
 auto *device=renderer_.Device();auto *context=renderer_.Context();const auto &r=packet.States().render;HRESULT hr=S_OK;
 ComPtr<ID3D11RasterizerState> *rasterStateResolved=nullptr;
 if(FAILED(hr=resolvedRaster_.Get(ResolvedStateKey(r,RasterInputStates,packet.terrain&&(static_cast<UINT>(packet.constants.modernMaterial[3])&32)),resolvedStatesEnabled_,rasterStateResolved,[&](auto *&out)->HRESULT{
 HRESULT stateHR=S_OK;D3D11_RASTERIZER_DESC raster{};raster.FillMode=r[D3DRS_FILLMODE]==D3DFILL_WIREFRAME?D3D11_FILL_WIREFRAME:D3D11_FILL_SOLID;
 raster.CullMode=r[D3DRS_CULLMODE]==D3DCULL_NONE?D3D11_CULL_NONE:D3D11_CULL_BACK;raster.FrontCounterClockwise=r[D3DRS_CULLMODE]==D3DCULL_CW;
 if(packet.terrain&&(static_cast<UINT>(packet.constants.modernMaterial[3])&32))raster.CullMode=D3D11_CULL_NONE;
 raster.DepthClipEnable=TRUE;raster.DepthBias=-static_cast<int>(r[D3DRS_ZBIAS]);
 auto &rasterState=rasterStates_.Get(raster);
 if(!rasterState&&FAILED(stateHR=device->CreateRasterizerState(&raster,rasterState.GetAddressOf())))return Fail(stateHR,"raster state");
 
 out=std::addressof(rasterState);return S_OK;
 })))return hr;
 auto &rasterState=*rasterStateResolved;
 sceneBindings_.Single(ScenePipelineBindings::Raster,rasterState.Get(),[&]{context->RSSetState(rasterState.Get());});
 ComPtr<ID3D11BlendState> *blendStateResolved=nullptr;
 if(FAILED(hr=resolvedBlend_.Get(ResolvedStateKey(r,BlendInputStates,screenTargetsBound_&&!offscreen_&&!packet.outputOverlay),resolvedStatesEnabled_,blendStateResolved,[&](auto *&out)->HRESULT{
 HRESULT stateHR=S_OK;D3D11_BLEND_DESC blend{};
 if(screenTargetsBound_&&!offscreen_&&!packet.outputOverlay){
  blend.IndependentBlendEnable=TRUE;
  for(unsigned target=1;target<8;++target){auto &aux=blend.RenderTarget[target];aux.BlendEnable=FALSE;aux.SrcBlend=aux.SrcBlendAlpha=D3D11_BLEND_ONE;aux.DestBlend=aux.DestBlendAlpha=D3D11_BLEND_ZERO;aux.BlendOp=aux.BlendOpAlpha=D3D11_BLEND_OP_ADD;aux.RenderTargetWriteMask=static_cast<UINT8>(ScreenMaterialGPU::AuxiliaryWriteMask(r[D3DRS_COLORWRITEENABLE],r[D3DRS_ZWRITEENABLE]!=0));}
 }
 auto &b=blend.RenderTarget[0];b.BlendEnable=r[D3DRS_ALPHABLENDENABLE]!=0;
 DWORD src=r[D3DRS_SRCBLEND],dst=r[D3DRS_DESTBLEND];
 if(src==D3DBLEND_BOTHSRCALPHA){src=D3DBLEND_SRCALPHA;dst=D3DBLEND_INVSRCALPHA;}
 if(src==D3DBLEND_BOTHINVSRCALPHA){src=D3DBLEND_INVSRCALPHA;dst=D3DBLEND_SRCALPHA;}
 b.SrcBlend=Blend(src);b.DestBlend=Blend(dst);b.BlendOp=static_cast<D3D11_BLEND_OP>(r[D3DRS_BLENDOP]);
 b.SrcBlendAlpha=AlphaBlend(b.SrcBlend);b.DestBlendAlpha=AlphaBlend(b.DestBlend);b.BlendOpAlpha=b.BlendOp;
 // DX8 ignores blend operands when blending is disabled. D3D11 still validates
 // its descriptor; unused legacy operands must not reject an opaque draw.
 if(!b.BlendEnable){b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlend=b.DestBlendAlpha=D3D11_BLEND_ZERO;b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;}
 b.RenderTargetWriteMask=static_cast<UINT8>(r[D3DRS_COLORWRITEENABLE]&D3D11_COLOR_WRITE_ENABLE_ALL);
 auto &blendState=blendStates_.Get(blend);
 if(!blendState&&FAILED(stateHR=device->CreateBlendState(&blend,blendState.GetAddressOf())))return Fail(stateHR,"blend state enabled="+std::to_string(b.BlendEnable)+" src="+std::to_string(b.SrcBlend)+" dest="+std::to_string(b.DestBlend)+" op="+std::to_string(b.BlendOp)+" mask="+std::to_string(b.RenderTargetWriteMask));
 
 out=std::addressof(blendState);return S_OK;
 })))return hr;
 auto &blendState=*blendStateResolved;
 sceneBindings_.Single(ScenePipelineBindings::Blend,blendState.Get(),[&]{context->OMSetBlendState(blendState.Get(),nullptr,0xffffffff);});
 ComPtr<ID3D11DepthStencilState> *depthStateResolved=nullptr;
 if(FAILED(hr=resolvedDepth_.Get(ResolvedStateKey(r,DepthInputStates,false),resolvedStatesEnabled_,depthStateResolved,[&](auto *&out)->HRESULT{
 HRESULT stateHR=S_OK;D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=r[D3DRS_ZENABLE]!=0;depth.DepthWriteMask=r[D3DRS_ZWRITEENABLE]?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
 depth.DepthFunc=static_cast<D3D11_COMPARISON_FUNC>(r[D3DRS_ZFUNC]);depth.StencilEnable=r[D3DRS_STENCILENABLE]!=0;
 depth.StencilReadMask=static_cast<UINT8>(r[D3DRS_STENCILMASK]);depth.StencilWriteMask=static_cast<UINT8>(r[D3DRS_STENCILWRITEMASK]);
 depth.FrontFace.StencilFailOp=static_cast<D3D11_STENCIL_OP>(r[D3DRS_STENCILFAIL]);depth.FrontFace.StencilDepthFailOp=static_cast<D3D11_STENCIL_OP>(r[D3DRS_STENCILZFAIL]);
 depth.FrontFace.StencilPassOp=static_cast<D3D11_STENCIL_OP>(r[D3DRS_STENCILPASS]);depth.FrontFace.StencilFunc=static_cast<D3D11_COMPARISON_FUNC>(r[D3DRS_STENCILFUNC]);depth.BackFace=depth.FrontFace;
 auto &depthState=depthStates_.Get(depth);
 if(!depthState&&FAILED(stateHR=device->CreateDepthStencilState(&depth,depthState.GetAddressOf())))return Fail(stateHR,"depth/stencil state");
 
 out=std::addressof(depthState);return S_OK;
 })))return hr;
 auto &depthState=*depthStateResolved;
 sceneBindings_.Depth(depthState.Get(),r[D3DRS_STENCILREF],[&]{context->OMSetDepthStencilState(depthState.Get(),r[D3DRS_STENCILREF]);});
 D3D11_VIEWPORT viewport{packet.constants.viewport[0],packet.constants.viewport[1],packet.constants.viewport[2],packet.constants.viewport[3],packet.constants.bridgeFlags[2],packet.constants.bridgeFlags[3]};sceneBindings_.Viewport({viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height,viewport.MinDepth,viewport.MaxDepth},[&]{context->RSSetViewports(1,&viewport);});
 std::array<ID3D11SamplerState*,8> boundSamplers{};
 for(UINT i=0;i<8;++i){auto &t=packet.States().stages[i];ComPtr<ID3D11SamplerState> *stateResolved=nullptr;
 if(FAILED(hr=resolvedSampler_.Get(ResolvedStateKey(t,SamplerInputStates,packet.constants.modernMaterial[0]>.5f&&!packet.outputOverlay&&!packet.States().render[D3DRS_ALPHABLENDENABLE]),resolvedStatesEnabled_,stateResolved,[&](auto *&out)->HRESULT{
 HRESULT stateHR=S_OK;D3D11_SAMPLER_DESC sampler{};
  sampler.AddressU=Address(t[D3DTSS_ADDRESSU]);sampler.AddressV=Address(t[D3DTSS_ADDRESSV]);sampler.AddressW=Address(t[D3DTSS_ADDRESSW]);
  sampler.Filter=t[D3DTSS_MINFILTER]==D3DTEXF_ANISOTROPIC?D3D11_FILTER_ANISOTROPIC:
   D3D11_ENCODE_BASIC_FILTER(t[D3DTSS_MINFILTER]==D3DTEXF_POINT?D3D11_FILTER_TYPE_POINT:D3D11_FILTER_TYPE_LINEAR,
    t[D3DTSS_MAGFILTER]==D3DTEXF_POINT?D3D11_FILTER_TYPE_POINT:D3D11_FILTER_TYPE_LINEAR,
    t[D3DTSS_MIPFILTER]==D3DTEXF_LINEAR?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,D3D11_FILTER_REDUCTION_TYPE_STANDARD);
  sampler.MaxAnisotropy=std::clamp(t[D3DTSS_MAXANISOTROPY],DWORD(1),DWORD(16));sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;
  sampler.MipLODBias=AsFloat(t[D3DTSS_MIPMAPLODBIAS]);sampler.MinLOD=static_cast<float>(t[D3DTSS_MAXMIPLEVEL]);
  sampler.MaxLOD=t[D3DTSS_MIPFILTER]==D3DTEXF_NONE?sampler.MinLOD:D3D11_FLOAT32_MAX;PackedColor(t[D3DTSS_BORDERCOLOR],sampler.BorderColor);
  // Modern opaque surfaces use the mip chains already generated on upload.
  // Native HUD and blended effects keep their own sampling contract.
  if(packet.constants.modernMaterial[0]>.5f&&!packet.outputOverlay&&!packet.States().render[D3DRS_ALPHABLENDENABLE]){
   sampler.Filter=D3D11_FILTER_ANISOTROPIC;sampler.MaxAnisotropy=8;sampler.MinLOD=0;sampler.MaxLOD=D3D11_FLOAT32_MAX;sampler.MipLODBias=0;
  }
  auto &state=samplerStates_.Get(sampler);
  if(!state&&FAILED(stateHR=device->CreateSamplerState(&sampler,state.GetAddressOf())))return stateHR;
  
 out=std::addressof(state);return S_OK;
 })))return hr;
 auto &state=*stateResolved;
 boundSamplers[i]=state.Get();
 }
 sceneBindings_.Samplers(boundSamplers,[&]{context->PSSetSamplers(0,8,boundSamplers.data());});
 return S_OK;
}

HRESULT CompatibilityRenderer::UploadGeometry(const DrawPacket &packet,bool shadow){
 const auto &indices=packet.Indices();
 const bool compact=packet.CompactVertices();
 const void *vertexData=packet.geometry?packet.geometry->VertexData():(compact?static_cast<const void*>(packet.fixedVertices.data()):static_cast<const void*>(packet.vertices.data()));
 const UINT vertexStride=packet.geometry?packet.geometry->Stride():(compact?sizeof(FixedVertex):sizeof(LegacyVertex));
 std::size_t vertexBytes=packet.VertexCount()*vertexStride,indexBytes=indices.size()*4;
 if(vertexBytes>UINT_MAX||indexBytes>UINT_MAX)return E_INVALIDARG;
 HRESULT hr;
 if(packet.geometry){
  auto &geometry=*packet.geometry;
  auto create=[&](UINT bytes,UINT bind,const void *data,ComPtr<ID3D11Buffer> &out){
   D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=bind;
   D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;
   HRESULT result=renderer_.Device()->CreateBuffer(&desc,&initial,out.ReleaseAndGetAddressOf());
   if(SUCCEEDED(result))stats_.uploadBytes+=bytes;return result;
  };
  if(!geometry.VertexBuffer()&&FAILED(hr=create(static_cast<UINT>(vertexBytes),D3D11_BIND_VERTEX_BUFFER,vertexData,geometry.VertexBuffer())))return hr;
  const auto &indexData=geometry.sharedIndices?geometry.sharedIndices->indices:indices;
  const auto sharedIndexBytes=indexData.size()*sizeof(std::uint32_t);
  if(sharedIndexBytes>UINT_MAX)return E_INVALIDARG;
  if(sharedIndexBytes&&!geometry.IndexBuffer()&&FAILED(hr=create(static_cast<UINT>(sharedIndexBytes),D3D11_BIND_INDEX_BUFFER,indexData.data(),geometry.IndexBuffer())))return hr;
 }else{
 if(vertexBytes>vertexCapacity_){vertexCapacity_=static_cast<UINT>(vertexBytes);if(FAILED(hr=DynamicBuffer(renderer_.Device(),vertexCapacity_,D3D11_BIND_VERTEX_BUFFER,vertexBuffer_)))return hr;}
 if(indexBytes>indexCapacity_){indexCapacity_=static_cast<UINT>(indexBytes);if(FAILED(hr=DynamicBuffer(renderer_.Device(),indexCapacity_,D3D11_BIND_INDEX_BUFFER,indexBuffer_)))return hr;}
 if(FAILED(hr=WriteBuffer(renderer_.Context(),vertexBuffer_.Get(),vertexData,vertexBytes)))return hr;
 if(indexBytes&&FAILED(hr=WriteBuffer(renderer_.Context(),indexBuffer_.Get(),indices.data(),indexBytes)))return hr;
 stats_.uploadBytes+=vertexBytes+indexBytes;
 workStats_.dynamicVertexBytes+=vertexBytes;workStats_.dynamicIndexBytes+=indexBytes;
 }
 if(packet.vertexShader&&FAILED(hr=vertexConstantsCache_.Upload(renderer_.Context(),vertexConstants_.Get(),packet.vertexConstants.Read(),workStats_.constantUploads,workStats_.constantSkips)))return hr;
 if(!shadow&&sceneConstantArena_.Has(sceneConstantIndex_)){++workStats_.constantSkips;return S_OK;}
 const auto constants=DrawConstants(packet,shadow);
 return drawConstantsCache_.Upload(renderer_.Context(),constantBuffer_.Get(),&constants,workStats_.constantUploads,workStats_.constantSkips);
}
void CompatibilityRenderer::ApplyTaaJitter(LegacyConstants &constants){
 // Only the primary camera's world draws: HUD, shadow and offscreen passes keep
 // their exact projection; the camera match uses the unjittered packet values.
 if(taaActive_&&renderer_.ScreenMaterials().Matches(constants.view,constants.projection,constants.viewport,constants.bridgeFlags+2))
  ApplyTemporalJitter(constants.projection,taaJitter_[0],taaJitter_[1],constants.viewport[2],constants.viewport[3]);
}
void CompatibilityRenderer::FillMotion(LegacyConstants &constants,const DrawPacket &packet,bool shadow){
 // photo107: per-object motion (GENZH_MOTION). Each object (render root, mesh, pass) keeps last frame's
 // world matrix. A moving object carries its previous world and the camera's previous and current
 // unjittered view-projections in its constants (MotionSettings.x = 1); everything else is zero.
 std::memset(constants.motionSettings,0,sizeof(constants.motionSettings));
 if(!motionEnabled_||shadow||offscreen_||packet.outputOverlay||packet.terrain||packet.vertexShader||!packet.nativeSource.root||packet.nativeSource.sorted||
    packet.constants.geometryFlags[0]>=.5f||!primaryDepthTarget_||!renderer_.ScreenMaterials().Available()||
    !(packet.primitive==D3DPT_TRIANGLELIST||packet.primitive==D3DPT_TRIANGLESTRIP))return;
 // The camera of this frame, unjittered: the packet's own matrices (the TAA jitter is applied to copies).
 if(!motionCurrentValid_){
  DirectX::XMFLOAT4X4 view,projection,viewProjection;std::memcpy(&view,packet.constants.view,64);std::memcpy(&projection,packet.constants.projection,64);
  DirectX::XMStoreFloat4x4(&viewProjection,DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&view),DirectX::XMLoadFloat4x4(&projection)));
  std::memcpy(motionCurrentVP_,&viewProjection,64);motionCurrentValid_=true;
 }
 const MotionKey key{packet.nativeSource.root,packet.nativeSource.mesh,packet.nativeSource.pass};
 std::array<float,16> world{};std::memcpy(world.data(),packet.constants.world,64);
 const auto previous=motionPrevious_.find(key);
 if(motionPreviousValid_&&previous!=motionPrevious_.end()&&std::memcmp(previous->second.data(),world.data(),64)!=0&&std::fabs(DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(world.data())))))>1e-12f){
  DirectX::XMFLOAT4X4 relativeWorld;DirectX::XMStoreFloat4x4(&relativeWorld,DirectX::XMMatrixMultiply(DirectX::XMMatrixInverse(nullptr,DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(world.data()))),DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(previous->second.data()))));std::memcpy(constants.motionPreviousWorld,&relativeWorld,64);std::memcpy(constants.motionCurrentVP,motionCurrentVP_,64);
  std::memcpy(constants.motionPreviousVP,motionPreviousVP_,64);constants.motionSettings[0]=1;++motionDraws_;
 }
 motionCurrent_[key]=world;
}
LegacyConstants CompatibilityRenderer::DrawConstants(const DrawPacket &packet,bool shadow){
 LegacyConstants constants=packet.constants;for(unsigned i=0;i<4;++i)constants.lightDirection[i][3]=0;
 const D3D11_VIEWPORT viewport{constants.viewport[0],constants.viewport[1],constants.viewport[2],constants.viewport[3],constants.bridgeFlags[2],constants.bridgeFlags[3]};
 if(!shadow&&!offscreen_&&AtmosphereWorldPacket(packet)&&localSurfaceLighting_.Matches(constants.view,constants.projection,viewport)){const auto tagged=localShadowBindings_.Apply(constants);localSurfaceStats_.taggedLights+=tagged;if(tagged)++localSurfaceStats_.taggedDraws;}
 FillMotion(constants,packet,shadow);
 if(!shadow&&!offscreen_)ApplyTaaJitter(constants);
 return constants;
}

HRESULT CompatibilityRenderer::BindInstances(const DrawGroup *instances,UINT sortedBase){
 auto *device=renderer_.Device();auto *context=renderer_.Context();HRESULT hr;
 if(!instanceSettings_&&FAILED(hr=DynamicBuffer(device,16,D3D11_BIND_CONSTANT_BUFFER,instanceSettings_)))return hr;
 UINT settings[4]={instances&&instances->size()>1?1u:0u,sortedBase,0,0};
 if(FAILED(hr=instanceSettingsCache_.Upload(context,instanceSettings_.Get(),settings,workStats_.constantUploads,workStats_.constantSkips)))return hr;
 ID3D11Buffer *constant=instanceSettings_.Get();sceneBindings_.ExtraSingle(ScenePipelineBindings::VSInstanceCB,constant,[&]{context->VSSetConstantBuffers(4,1,&constant);});sceneBindings_.ExtraSingle(ScenePipelineBindings::PSInstanceCB,constant,[&]{context->PSSetConstantBuffers(4,1,&constant);});
 ID3D11ShaderResourceView *view=nullptr;
 if(settings[0]){
  struct Matrices{float world[16],normal[16];};
  const auto bytes=instances->size()*sizeof(Matrices);if(bytes>UINT_MAX)return E_INVALIDARG;
  if(bytes>instanceCapacity_){
   D3D11_BUFFER_DESC desc{};desc.ByteWidth=static_cast<UINT>(bytes);desc.Usage=D3D11_USAGE_DEFAULT;
   desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
   desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=sizeof(Matrices);
   if(FAILED(hr=device->CreateBuffer(&desc,nullptr,instanceBuffer_.ReleaseAndGetAddressOf())))return hr;
   D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;srv.Format=DXGI_FORMAT_UNKNOWN;
   srv.Buffer.NumElements=static_cast<UINT>(instances->size());
   if(FAILED(hr=device->CreateShaderResourceView(instanceBuffer_.Get(),&srv,instanceView_.ReleaseAndGetAddressOf())))return hr;
   instanceCapacity_=static_cast<UINT>(bytes);
  }
  std::vector<Matrices> matrices(instances->size());auto *dest=matrices.data();
  for(std::size_t i=0;i<instances->size();++i){std::memcpy(dest[i].world,(*instances)[i]->constants.world,64);std::memcpy(dest[i].normal,(*instances)[i]->constants.normalWorld,64);}
  // Structured buffers cannot combine MiscFlags with CPU access. The immediate
  // context snapshots this upload before the following instanced draw.
  D3D11_BOX box{0,0,0,static_cast<UINT>(bytes),1,1};
  context->UpdateSubresource(instanceBuffer_.Get(),0,&box,matrices.data(),0,0);
  stats_.uploadBytes+=bytes;view=instanceView_.Get();stats_.groupedInstances+=instances->size();
 }
 sceneBindings_.ExtraSingle(ScenePipelineBindings::VSInstanceView,view,[&]{context->VSSetShaderResources(15,1,&view);});return S_OK;
}
HRESULT CompatibilityRenderer::Execute(const DrawPacket &packet,bool shadow,const DrawGroup *instances,UINT shadowSize,const SortedColorPalette *palette,UINT sortedIndexCount){
 GpuPhase timed=shadow?GpuPhase::Shadow:(packet.outputOverlay?GpuPhase::HUD:(offscreen_?GpuPhase::Offscreen:(packet.terrain?GpuPhase::Terrain:GpuPhase::Scene)));
 // perf116 measurement: the scene phase is split into water and alpha-blended draws (opaque stays in Scene).
 if(timed==GpuPhase::Scene){if(static_cast<UINT>(packet.constants.modernMaterial[3])&(2097152u|8388608u))timed=GpuPhase::SceneWater;else if(packet.States().render[D3DRS_ALPHABLENDENABLE])timed=GpuPhase::SceneBlend;}
 passTimer_.Mark(renderer_.Context(),timed);
 Program *program=nullptr;HRESULT hr=GetProgram(packet,program,shadow,palette!=nullptr);if(FAILED(hr))return hr;
 if(FAILED(hr=UploadGeometry(packet,shadow)))return Fail(hr,"draw geometry/constant upload");
 const bool screenMain=!shadow&&!offscreen_&&!packet.outputOverlay&&primaryDepthTarget_&&renderer_.ScreenMaterials().Available();
 if(screenMain&&!screenTargetsBound_){renderer_.ScreenMaterials().BindTargets(renderer_.Context(),renderer_.SceneTarget(),renderer_.SceneDepth());screenTargetsBound_=true;}
 if(shadow||offscreen_||packet.outputOverlay)screenTargetsBound_=false;
 const bool capture=screenMain&&ScreenMaterialPacket(packet,shadow,offscreen_,primaryDepthTarget_,renderer_.ScreenMaterials().Matches(packet.constants.view,packet.constants.projection,packet.constants.viewport,packet.constants.bridgeFlags+2));
 renderer_.ScreenMaterials().BindControl(renderer_.Context(),capture,renderer_.ScreenMaterials().SunMask(packet.constants.lightPosition,packet.constants.lightDirection));
 if(FAILED(hr=BindStates(packet)))return hr;
 if(FAILED(hr=BindInstances(instances,palette?static_cast<UINT>(packet.geometry->IndexBase()):0)))return hr;
 auto *context=renderer_.Context();const bool opaqueDepth=shadow&&opaqueShadowDepth_&&OpaqueShadowDepthPacket(packet);
 sceneBindings_.Single(ScenePipelineBindings::Layout,program->layout.Get(),[&]{context->IASetInputLayout(program->layout.Get());});
 auto *vertexShader=opaqueDepth?program->opaqueShadowVertex.Get():(shadow&&!packet.terrain?program->shadowVertex.Get():program->vertex.Get());
 sceneBindings_.Single(ScenePipelineBindings::VS,vertexShader,[&]{context->VSSetShader(vertexShader,nullptr,0);});const bool compactTerrain=packet.terrain&&(shadow?program->terrainCompactShadow:program->terrainCompactColor);
 auto *pixelShader=opaqueDepth?nullptr:shadow?(compactTerrain?program->terrainShadowPixel.Get():program->shadowPixel.Get()):(compactTerrain?program->terrainPixel.Get():program->pixel.Get());
 sceneBindings_.Single(ScenePipelineBindings::PS,pixelShader,[&]{context->PSSetShader(pixelShader,nullptr,0);});
 auto *geometryShader=!shadow&&packet.primitive==D3DPT_POINTLIST?program->point.Get():nullptr;
 if(shadow){D3D11_VIEWPORT viewport{0,0,float(shadowSize),float(shadowSize),0,1};context->RSSetViewports(1,&viewport);context->RSSetState(shadowRaster_.Get());context->OMSetDepthStencilState(shadowState_.Get(),0);context->OMSetBlendState(nullptr,nullptr,0xffffffff);}
 const bool terrain=packet.terrain;
 if(terrain){
  const bool compact=shadow?program->terrainCompactShadow:program->terrainCompactColor;
  geometryShader=compact?program->terrainGrassCompact.Get():program->terrainGrass.Get();
  if(compact)++workStats_.terrainCompactDraws;else ++workStats_.terrainFullDraws;
 }
 sceneBindings_.Single(ScenePipelineBindings::GS,geometryShader,[&]{context->GSSetShader(geometryShader,nullptr,0);});
 auto *hull=terrain?program->terrainHull.Get():nullptr;auto *domain=terrain?program->terrainDomain.Get():nullptr;
 sceneBindings_.Single(ScenePipelineBindings::HS,hull,[&]{context->HSSetShader(hull,nullptr,0);});sceneBindings_.Single(ScenePipelineBindings::DS,domain,[&]{context->DSSetShader(domain,nullptr,0);});
 ID3D11Buffer *constants=constantBuffer_.Get();
 if(shadow||!sceneConstantArena_.Bind(sceneConstantIndex_)){
  context->VSSetConstantBuffers(0,1,&constants);context->PSSetConstantBuffers(0,1,&constants);context->GSSetConstantBuffers(0,1,&constants);
  context->HSSetConstantBuffers(0,1,&constants);context->DSSetConstantBuffers(0,1,&constants);
 }
 constants=vertexConstants_.Get();sceneBindings_.Single(ScenePipelineBindings::VertexConstants,constants,[&]{context->VSSetConstantBuffers(1,1,&constants);});
 // taa101: game vertex programs (trees, water waves) of the primary camera get its jitter.
 if(packet.vertexShader){const bool jittered=!shadow&&!offscreen_&&taaActive_&&renderer_.ScreenMaterials().Matches(packet.constants.view,packet.constants.projection,packet.constants.viewport,packet.constants.bridgeFlags+2);
  ID3D11Buffer *jitter=jittered?taaVertexJitter_.Get():taaVertexZero_.Get();context->VSSetConstantBuffers(8,1,&jitter);if(jittered)++taaVertexDraws_;}
  const auto passShadows=ShadowConstantsForPass(shadowData_,shadow,packet.outputOverlay,offscreen_);
 if(FAILED(hr=shadowConstantsCache_.Upload(context,shadowConstants_.Get(),&passShadows,workStats_.constantUploads,workStats_.constantSkips)))return hr;
 constants=shadowConstants_.Get();sceneBindings_.ExtraSingle(ScenePipelineBindings::ShadowVS,constants,[&]{context->VSSetConstantBuffers(2,1,&constants);});sceneBindings_.ExtraSingle(ScenePipelineBindings::ShadowPS,constants,[&]{context->PSSetConstantBuffers(2,1,&constants);});
 if(terrain){sceneBindings_.ExtraSingle(ScenePipelineBindings::ShadowHS,constants,[&]{context->HSSetConstantBuffers(2,1,&constants);});sceneBindings_.ExtraSingle(ScenePipelineBindings::ShadowDS,constants,[&]{context->DSSetConstantBuffers(2,1,&constants);});sceneBindings_.ExtraSingle(ScenePipelineBindings::ShadowGS,constants,[&]{context->GSSetConstantBuffers(2,1,&constants);});}
 UINT stride=packet.geometry?packet.geometry->Stride():(packet.CompactVertices()?sizeof(FixedVertex):sizeof(LegacyVertex)),offset=palette?static_cast<UINT>(packet.geometry->IndexBase())*packet.geometry->Stride():0;ID3D11Buffer *vertices=packet.geometry?packet.geometry->VertexBuffer().Get():vertexBuffer_.Get();
 sceneBindings_.Vertex(vertices,stride,offset,[&]{context->IASetVertexBuffers(0,1,&vertices,&stride,&offset);});
 auto *indexBuffer=packet.geometry?packet.geometry->IndexBuffer().Get():indexBuffer_.Get();sceneBindings_.Single(ScenePipelineBindings::Indices,indexBuffer,[&]{context->IASetIndexBuffer(indexBuffer,DXGI_FORMAT_R32_UINT,0);});
 D3D11_PRIMITIVE_TOPOLOGY topology=packet.primitive==D3DPT_POINTLIST?D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:
  packet.primitive==D3DPT_LINELIST?D3D11_PRIMITIVE_TOPOLOGY_LINELIST:packet.primitive==D3DPT_LINESTRIP?D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
  packet.primitive==D3DPT_TRIANGLESTRIP?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
 if(terrain)topology=D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
 sceneBindings_.Topology(topology,[&]{context->IASetPrimitiveTopology(topology);});
 std::array<ID3D11ShaderResourceView*,10> views{};
 for(UINT i=0;i<8;++i)views[i]=packet.capturedTextures[i].Get();
 for(UINT i=0;i<2;++i)views[8+i]=packet.materialTextures[i].Get();
 sceneBindings_.Textures(views,[&]{context->PSSetShaderResources(0,10,views.data());});
 ID3D11ShaderResourceView *extra[]={packet.heightTexture.Get(),packet.emissiveTexture.Get()};sceneBindings_.Extra(extra,[&]{context->PSSetShaderResources(16,2,extra);});
 ID3D11ShaderResourceView *wear[]={packet.wearTextures[0].Get(),packet.wearTextures[1].Get(),packet.wearTextures[2].Get()};sceneBindings_.Wear(wear,[&]{context->PSSetShaderResources(26,3,wear);});
 ID3D11ShaderResourceView *surface[]={terrain?packet.terrainHeight.Get():nullptr,terrain?packet.terrainClass.Get():nullptr};
 sceneBindings_.ExtraSurface(ScenePipelineBindings::SurfaceDomain,surface,[&]{context->DSSetShaderResources(11,2,surface);});sceneBindings_.ExtraSurface(ScenePipelineBindings::SurfacePixel,surface,[&]{context->PSSetShaderResources(11,2,surface);});
 sceneBindings_.ExtraSurface(ScenePipelineBindings::SurfaceGeometry,surface,[&]{context->GSSetShaderResources(11,2,surface);});sceneBindings_.ExtraSurface(ScenePipelineBindings::SurfaceHull,surface,[&]{context->HSSetShaderResources(11,2,surface);});
 ID3D11ShaderResourceView *cells=terrain?packet.terrainCells.Get():nullptr;
 sceneBindings_.ExtraSingle(ScenePipelineBindings::DSCells,cells,[&]{context->DSSetShaderResources(13,1,&cells);});sceneBindings_.ExtraSingle(ScenePipelineBindings::GSCells,cells,[&]{context->GSSetShaderResources(13,1,&cells);});sceneBindings_.ExtraSingle(ScenePipelineBindings::HSCells,cells,[&]{context->HSSetShaderResources(13,1,&cells);});
 ID3D11ShaderResourceView *exclusion=terrain?terrainExclusionView_.Get():nullptr;
 sceneBindings_.ExtraSingle(ScenePipelineBindings::GSExclusion,exclusion,[&]{context->GSSetShaderResources(14,1,&exclusion);});sceneBindings_.ExtraSingle(ScenePipelineBindings::DSExclusion,exclusion,[&]{context->DSSetShaderResources(14,1,&exclusion);});
 if(terrain){
  if(FAILED(hr=terrainConstantsCache_.Upload(context,terrainConstants_.Get(),packet.terrainDimensions,workStats_.constantUploads,workStats_.constantSkips)))return hr;
  constants=terrainConstants_.Get();context->DSSetConstantBuffers(3,1,&constants);context->GSSetConstantBuffers(3,1,&constants);context->HSSetConstantBuffers(3,1,&constants);
 }
 ID3D11SamplerState *terrainSampler=terrainSampler_.Get();sceneBindings_.ExtraSingle(ScenePipelineBindings::DSSampler,terrainSampler,[&]{context->DSSetSamplers(9,1,&terrainSampler);});
 // Hull/domain stages read the library for macro relief in colour and shadow passes alike.
 if(terrain&&(static_cast<UINT>(packet.constants.modernMaterial[3])&4096))terrainLibrary_.Bind(context,!shadow);
 // terrain98: analytic grass shadows read the canonical cells, map info and the
 // grass placement/interaction constants in the terrain pixel shader; terrain101:
 // so does the continuous ground field (65536).
 if(terrain&&!shadow&&(static_cast<UINT>(packet.constants.modernMaterial[3])&(32768|65536))){
  ID3D11ShaderResourceView *cellView=packet.terrainCells.Get();context->PSSetShaderResources(13,1,&cellView);
  ID3D11ShaderResourceView *classView=packet.terrainClasses.Get();context->PSSetShaderResources(TerrainGrassGPU::ClassesSlot,1,&classView);
  ID3D11Buffer *mapInfo=terrainConstants_.Get();context->PSSetConstantBuffers(3,1,&mapInfo);grass_.BindPixel(context);
 }
 renderer_.BindEnvironment(!shadow&&!packet.outputOverlay&&!offscreen_,&sceneBindings_);
 // water107: the scene depth behind the water (read-only depth target) and its shore settings.
 const bool water=!shadow&&(static_cast<UINT>(packet.constants.modernMaterial[3])&(2097152u|8388608u))!=0;
 if(water){
  float shore[4]={packet.waterShore[0],packet.waterShore[1],packet.waterShore[2],0};
  if(screenMain&&renderer_.SceneDepthView()&&renderer_.SceneReadOnlyDepth()){
   renderer_.ScreenMaterials().BindTargets(context,renderer_.SceneTarget(),renderer_.SceneReadOnlyDepth());screenTargetsBound_=false;
   ID3D11ShaderResourceView *depthView=renderer_.SceneDepthView();context->PSSetShaderResources(60,1,&depthView);shore[3]=1;++waterShoreDraws_;
   // photo107: the scene copy taken before the water draws (the reflection's colour source).
   renderer_.CopySceneForWater();ID3D11ShaderResourceView *sceneCopy=renderer_.WaterSceneView();context->PSSetShaderResources(61,1,&sceneCopy);
  }else ++waterShoreFallbacks_;
  if(!waterConstants_){D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(FAILED(hr=renderer_.Device()->CreateBuffer(&desc,nullptr,waterConstants_.GetAddressOf())))return Fail(hr,"water constants");}
  context->UpdateSubresource(waterConstants_.Get(),0,nullptr,shore,0,0);ID3D11Buffer *waterBuffer=waterConstants_.Get();context->PSSetConstantBuffers(6,1,&waterBuffer);
 }
 const D3D11_VIEWPORT atmosphereViewport{packet.constants.viewport[0],packet.constants.viewport[1],packet.constants.viewport[2],packet.constants.viewport[3],packet.constants.bridgeFlags[2],packet.constants.bridgeFlags[3]};
 atmosphere_.Bind(context,!shadow&&!offscreen_&&AtmosphereWorldPacket(packet)&&
                  atmosphere_.Matches(packet.constants.view,packet.constants.projection,atmosphereViewport),&sceneBindings_);
 const bool calibratedSurface=!shadow&&!offscreen_&&!packet.outputOverlay&&packet.constants.modernMaterial[0]>.5f;
 if(FAILED(hr=calibratedLights_.Prepare(context,packet.constants,calibratedSurface,&sceneBindings_)))return Fail(hr,"native light calibration upload");
 const bool localSurface=!shadow&&!offscreen_&&AtmosphereWorldPacket(packet)&&localSurfaceLighting_.Matches(packet.constants.view,packet.constants.projection,atmosphereViewport);localSurfaceLighting_.Bind(context,localSurface,renderer_.ShadowSampler(),&sceneBindings_);if(localSurface)++localSurfaceStats_.binds;
 ID3D11ShaderResourceView *sceneShadow=shadow||packet.outputOverlay||offscreen_?nullptr:shadowCascades_.View();sceneBindings_.ExtraSingle(ScenePipelineBindings::PSShadowView,sceneShadow,[&]{context->PSSetShaderResources(10,1,&sceneShadow);});
 ID3D11SamplerState *comparison=renderer_.ShadowSampler();sceneBindings_.ExtraSingle(ScenePipelineBindings::PSCompare,comparison,[&]{context->PSSetSamplers(8,1,&comparison);});
 if(packet.outputOverlay){ID3D11RenderTargetView *output=renderer_.OutputTarget();context->OMSetRenderTargets(1,&output,nullptr);}
 const UINT indexFirst=packet.geometry?packet.geometry->IndexFirst():0;
 const INT indexBase=palette?0:(packet.geometry?packet.geometry->IndexBase():0);
 if(instances&&instances->size()>1)context->DrawIndexedInstanced(static_cast<UINT>(packet.Indices().size()),static_cast<UINT>(instances->size()),indexFirst,indexBase,0);
 else if(packet.Indices().empty())context->Draw(static_cast<UINT>(packet.VertexCount()),0);else{if(palette)palette->Bind(context);context->DrawIndexed(palette?sortedIndexCount:static_cast<UINT>(packet.Indices().size()),indexFirst,indexBase);}
 ++stats_.draws;
 if(water){ID3D11ShaderResourceView *none[2]{};context->PSSetShaderResources(60,2,none);}
 if(shadow){if(opaqueDepth)++workStats_.opaqueShadowDraws;else ++workStats_.fallbackShadowDraws;}
 // terrain97: the frame's GPU grass, once, after its first opaque modern terrain draw.
 if(terrain&&!shadow&&!offscreen_&&!packet.outputOverlay&&!grassDrawn_&&(static_cast<UINT>(packet.constants.modernMaterial[3])&16384)&&packet.constants.modernMaterial[0]>1.5f){
  grassDrawn_=true;if(FAILED(hr=DrawGrass(packet,program->pixel.Get())))return Fail(hr,"GPU grass");
 }
 return S_OK;
}
std::array<std::uint64_t,8> CompatibilityRenderer::CaptureContext()const{
 return {offscreen_?1ull:0ull,(materialCompatibility_?1ull:0ull)|(ReplaceShroudPasses()?2ull:0ull),modernShadows_?1ull:0ull,(terrainEnabled_?1ull:0ull)|(TerrainLibraryActive()?2ull:0ull)|(TerrainReliefActive()?4ull:0ull)|(grassEnabled_&&!grass_.Failed()?8ull:0ull)|(grassShadowsEnabled_?16ull:0ull)|(TerrainLibraryActive()&&terrainFieldEnabled_?32ull:0ull)|(TerrainLibraryActive()&&terrainHexEnabled_?64ull:0ull)|(TerrainLibraryActive()&&terrainWetEnabled_?128ull:0ull)|(TerrainLibraryActive()&&terrainRocksEnabled_?256ull:0ull)|(static_cast<std::uint64_t>(accumulationFlags_.load(std::memory_order_relaxed))<<8),
  materialRootRevision_,GetD3D11GroundOverlayRevision(),materialStreaming_.Epoch(),inScene_?1ull:0ull};
}
bool CompatibilityRenderer::ShroudTexture(TextureData &texture)const{
 const auto revision=GetD3D11ShroudRevision();
 if(texture.shroudRevision!=revision){
  texture.shroudRevision=revision;D3D11ShroudMapping mapping;DWORD bytes=sizeof(mapping);
  texture.shroud=texture.native&&SUCCEEDED(texture.native->GetPrivateData(D3D11ShroudMappingGUID,&mapping,&bytes))&&bytes==sizeof(mapping)&&ValidShroudMapping(mapping);
  texture.shroudMapping=texture.shroud?mapping:D3D11ShroudMapping{};
 }
 return texture.shroud;
}
// Offscreen targets (water reflection) keep the native passes. The previous
// world frame must have prepared the atmosphere the fog pass reads.
bool CompatibilityRenderer::ReplaceShroudPasses()const{
 return shroudFogEnabled.load(std::memory_order_relaxed)&&!offscreen_&&atmosphere_.Available()&&atmosphereReadyLast_;
}
// perf94: scene depth, GTAO and the fog of war belong to the world. They run once
// per frame, before the first screen-space (HUD) draw of the main scene queue,
// or at EndScene when the frame has none; HUD drawn in the queue stays on top.
HRESULT CompatibilityRenderer::DrawGrass(const DrawPacket &packet,ID3D11PixelShader *pixel){
 if(!packet.textures[0])return S_OK;
 const auto found=terrainMaps_.find(packet.textures[0]->native.Get());
 if(found==terrainMaps_.end()||!found->second.elevation||!found->second.flips)return S_OK;
 const auto &maps=found->second;
 TerrainGrassInputs inputs;inputs.height=packet.terrainHeight.Get();inputs.identity=packet.terrainClass.Get();inputs.cells=packet.terrainCells.Get();inputs.classes=packet.terrainClasses.Get();
 inputs.exclusion=terrainExclusionView_.Get();inputs.elevation=maps.elevation.Get();inputs.flips=maps.flips.Get();inputs.atlas=packet.capturedTextures[0].Get();
 inputs.terrainSampler=terrainSampler_.Get();inputs.library=&terrainLibrary_;std::copy_n(packet.terrainDimensions,4,inputs.dimensions.begin());
 inputs.minHeight=maps.minHeight;inputs.maxHeight=maps.maxHeight;
 const float seconds=static_cast<float>(std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now()-grassEpoch_).count(),3600.0));
 grass_.SetVehicles(grassVehicles_);
 // The blades are drawn with the same jitter as the terrain they stand on.
 LegacyConstants constants=packet.constants;ApplyTaaJitter(constants);
 passTimer_.Mark(renderer_.Context(),GpuPhase::Grass);
 grass_.passTimer=&passTimer_;
 const HRESULT hr=grass_.Draw(renderer_.Device(),renderer_.Context(),constants,inputs,pixel,seconds);
 // A grass failure keeps the frame: Draw records it and the legacy blades return.
 return hr==E_INVALIDARG?S_OK:hr;
}
HRESULT CompatibilityRenderer::FinishWorldPasses(){
 if(worldPassesDone_)return S_OK;worldPassesDone_=true;HRESULT hr=S_OK;
 if(frameHasWorldDepth_){hr=renderer_.BuildSceneDepth();if(FAILED(hr))return Fail(hr,"Scene depth hierarchy");}
 if(frameHasWorldDepth_){hr=renderer_.ApplyScreenAO();if(FAILED(hr))return Fail(hr,"Screen ambient occlusion");}
 if(FAILED(hr=DrawShroudFog()))return hr;
 if(frameHasWorldDepth_)atmosphereReadyLast_=atmosphere_.Ready();
 // photo107: screen-space indirect light, added to the lit scene before the temporal resolve (GENZH_GI).
 if(frameHasWorldDepth_)renderer_.ApplyGlobalIllumination();
 // taa100: resolve the world image (after depth, GTAO and fog; before the HUD).
 if(taaActive_&&frameHasWorldDepth_&&renderer_.ScreenMaterials().CameraReady()){
  const auto &camera=renderer_.ScreenMaterials().camera;TemporalAAFrame frame;
  std::memcpy(frame.inverseViewProjection,camera.inverseVP,64);
  DirectX::XMFLOAT4X4 view,projection,vp;std::memcpy(&view,camera.viewMatrix,64);std::memcpy(&projection,camera.projectionMatrix,64);
  DirectX::XMStoreFloat4x4(&vp,DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&view),DirectX::XMLoadFloat4x4(&projection)));std::memcpy(frame.viewProjection,&vp,64);
  std::memcpy(frame.viewport,camera.viewport,16);frame.depthRange[0]=camera.depthRange[0];frame.depthRange[1]=camera.depthRange[1];
  std::memcpy(frame.eye,camera.eye,12);frame.jitter[0]=taaJitter_[0];frame.jitter[1]=taaJitter_[1];
  hr=renderer_.ApplyTemporalAA(frame);if(FAILED(hr)){renderer_.InvalidateTemporalAA();hr=S_OK;}
 }else renderer_.InvalidateTemporalAA();
 // post106: glow of the finished world image, before the HUD (a failure only drops the glow).
 if(frameHasWorldDepth_)renderer_.ApplyBloom();
 return S_OK;
}
HRESULT CompatibilityRenderer::DrawShroudFog(){
 const auto texture=std::move(shroudFrameTexture_);shroudFrameTexture_.reset();
 if(!texture)return S_OK;
 if(!frameHasWorldDepth_||!atmosphere_.Ready()){++shroudFogStats_.missed;return S_OK;}
 GPUTexture *gpu=nullptr;const HRESULT hr=GetTexture(texture,gpu);if(FAILED(hr))return Fail(hr,"shroud fog texture");
 const bool flat=shroudFogFlat.load(std::memory_order_relaxed);
 // perf93: the cell vision registered by the game places fog over ground out of
 // sight; without it (or for another layout) the native levels remain the cover.
 const auto &mapping=texture->shroudMapping;const auto &vision=GetD3D11ShroudVisibility();ID3D11ShaderResourceView *visionView=nullptr;
 if(!flat&&vision.cellsX==mapping.cellsX&&vision.cellsY==mapping.cellsY&&vision.cells.size()==std::size_t(vision.cellsX)*vision.cellsY&&
    atmosphere_.UpdateShroudVision(renderer_.Context(),vision.cells.data(),vision.cellsX,vision.cellsY,vision.border,vision.revision,mapping.textureWidth,mapping.textureHeight,atmosphereDelta_)){
  visionView=atmosphere_.ShroudVision();++shroudFogStats_.vision;
 }else if(!flat)atmosphere_.ResetShroudVision();
 if(!gpu||!gpu->view||!atmosphere_.DrawShroudFog(renderer_.Context(),renderer_.SceneTarget(),renderer_.SceneDepthView(),gpu->view.Get(),mapping,flat,visionView)){++shroudFogStats_.invalid;return S_OK;}
 if(flat)++shroudFogStats_.flatPasses;else ++shroudFogStats_.fogPasses;
 return S_OK;
}
HRESULT CompatibilityRenderer::Draw(DrawPacket &&packet,bool texturesCaptured){
 const bool staged=scenePackets_.staged(packet); // perf88: built in place in the queue's next slot
 if(!inScene_)return Fail(E_UNEXPECTED,"draw outside scene");
 ++stats_.nativeDraws;
 // perf92: the native shroud material pass is replaced by the scene-depth fog
 // of war (DrawShroudFog); a shroud texture in a later stage of the same pass
 // (water) samples white, so that surface is not darkened twice.
 bool neutralizeShroud=false;
 if(ReplaceShroudPasses()){
  if(packet.textures[0]&&ShroudTexture(*packet.textures[0])&&NativeShroudPassStates(packet.States().render,packet.vertexShader!=0,packet.pixelShader!=0)){
   shroudFrameTexture_=packet.textures[0];++shroudFogStats_.replaced;if(staged)scenePackets_.discard();return S_OK;
  }
  for(UINT i=1;i<8&&!neutralizeShroud;++i)neutralizeShroud=packet.textures[i]&&ShroudTexture(*packet.textures[i]);
 }
 HRESULT hr=texturesCaptured?S_OK:CaptureTextures(packet);if(FAILED(hr))return hr;
 if(neutralizeShroud){
  if(!shroudWhite_){
   const std::uint32_t white=0xffffffffu;D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
   desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
   D3D11_SUBRESOURCE_DATA initial{&white,4,4};ComPtr<ID3D11Texture2D> texture;
   if(FAILED(hr=renderer_.Device()->CreateTexture2D(&desc,&initial,texture.GetAddressOf()))||FAILED(hr=renderer_.Device()->CreateShaderResourceView(texture.Get(),nullptr,shroudWhite_.GetAddressOf())))return Fail(hr,"shroud stage texture");
  }
  for(UINT i=1;i<8;++i)if(packet.textures[i]&&ShroudTexture(*packet.textures[i])){packet.capturedTextures[i]=shroudWhite_;++shroudFogStats_.neutralized;}
 }
 if(packet.primitive==D3DPT_TRIANGLEFAN){
  if(packet.indices.empty()){packet.indices.resize(packet.vertices.size());for(UINT i=0;i<packet.indices.size();++i)packet.indices[i]=i;}
  std::vector<std::uint32_t> triangles;for(std::size_t i=1;i+1<packet.indices.size();++i){triangles.push_back(packet.indices[0]);triangles.push_back(packet.indices[i]);triangles.push_back(packet.indices[i+1]);}
  packet.indices=std::move(triangles);packet.primitive=D3DPT_TRIANGLELIST;
 }
 if(packet.terrain&&packet.primitive==D3DPT_TRIANGLESTRIP){
  if(packet.indices.empty()){packet.indices.resize(packet.vertices.size());for(UINT i=0;i<packet.indices.size();++i)packet.indices[i]=i;}
  std::vector<std::uint32_t> triangles;
  for(std::size_t i=0;i+2<packet.indices.size();++i){auto a=packet.indices[i],b=packet.indices[i+1],c=packet.indices[i+2];
   if(a==b||a==c||b==c)continue;if(i&1)std::swap(a,b);triangles.insert(triangles.end(),{a,b,c});}
  packet.indices=std::move(triangles);packet.primitive=D3DPT_TRIANGLELIST;
 }
 if(!packet.geometry&&!packet.vertexShader&&!packet.vertices.empty()){packet.CompactDynamic();++workStats_.compactedDraws;}
 if(packet.outputOverlay){overlays_.push_back(std::move(packet));if(staged)scenePackets_.discard();return S_OK;}
 if(!offscreen_){
  if(AtmosphereWorldPacket(packet))frameHasWorldDepth_=true;
  if(!scenePackets_.empty()&&TryAppendSortedPacket(scenePackets_.back(),packet)){++sortedPacketMerges_;if(staged)scenePackets_.discard();return S_OK;}
  if(staged)scenePackets_.commit();else scenePackets_.push_back(std::move(packet));
  if(scenePackets_.size()>queueHighWater_){
   queueHighWater_=scenePackets_.size();
   if(queueHighWater_>=1024&&(queueHighWater_&(queueHighWater_-1))==0)
    GenZHLoad::Record("scene_queue_growth",std::to_string(queueHighWater_)+" paged packets; merged="+std::to_string(sortedPacketMerges_),0,queueHighWater_*sizeof(DrawPacket));
  }
  return S_OK;
 }
 const HRESULT executed=Execute(packet);if(staged)scenePackets_.discard();return executed;
}
HRESULT CompatibilityRenderer::FlushScene(){
 if(scenePackets_.empty())return S_OK;
 if(scenePackets_.size()>10000){
  std::uint64_t sorted=0,opaqueSorted=0,untagged=0,triangles=0;
  for(const auto &p:scenePackets_){sorted+=p.nativeSource.sorted;untagged+=!p.nativeSource.Valid();
   if(p.nativeSource.sorted&&!p.States().render[D3DRS_ALPHABLENDENABLE])++opaqueSorted;
   if(p.primitive==D3DPT_TRIANGLELIST)triangles+=p.Indices().size()/3;}
  GenZHLoad::Record("scene_packet_pressure",std::to_string(scenePackets_.size())+" packets; sorted="+std::to_string(sorted)+"; opaque_sorted="+std::to_string(opaqueSorted)+"; untagged="+std::to_string(untagged)+"; triangles="+std::to_string(triangles),0,scenePackets_.size()*sizeof(DrawPacket));
 }
 screenTargetsBound_=false;
 // terrain98: vehicles (and low aircraft) that flatten grass: per-vehicle packets
 // (wear profile set), one entry per vehicle position, radius from model bounds.
 grassVehicles_.clear();
 if(grassInteractionEnabled_)for(const auto &packet:scenePackets_){
  const auto &c=packet.constants;if(c.vehicleAge[3]<.5f||packet.outputOverlay||c.geometryFlags[0]>.5f)continue;
  float radius=6.5f;
  if(packet.geometry&&packet.geometry->boundsReady){const auto &g=*packet.geometry;
   const float sx=std::sqrt(c.world[0]*c.world[0]+c.world[1]*c.world[1]+c.world[2]*c.world[2]),sy=std::sqrt(c.world[4]*c.world[4]+c.world[5]*c.world[5]+c.world[6]*c.world[6]);
   radius=std::clamp(.5f*std::max((g.high[0]-g.low[0])*sx,(g.high[1]-g.low[1])*sy),3.0f,14.0f);}
  const std::array<float,4> vehicle{c.world[12],c.world[13],c.world[14],radius};bool merged=false;
  for(auto &known:grassVehicles_)if(std::abs(known[0]-vehicle[0])<4&&std::abs(known[1]-vehicle[1])<4){known[3]=std::max(known[3],radius);merged=true;break;}
  if(!merged&&grassVehicles_.size()<TerrainGrassGPU::MaxVehicles)grassVehicles_.push_back(vehicle);
 }
 const auto exclusionStart=std::chrono::steady_clock::now();
 HRESULT exclusionResult=BuildTerrainExclusion();if(FAILED(exclusionResult))return exclusionResult;
 stats_.exclusionMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-exclusionStart).count();
 using namespace DirectX;
 std::vector<const DrawPacket*> casters;std::vector<XMFLOAT3> positions;
 std::vector<std::array<XMFLOAT3,8>> casterBounds;
 const LegacyConstants *shadowCamera=nullptr;
 // Terrain is the primary world camera; projected decal/stencil passes may
 // carry another projection and must not choose the cascade receiver frustum.
 for(const auto &packet:scenePackets_)if(packet.terrain&&packet.constants.geometryFlags[0]<.5f&&packet.States().render[D3DRS_ZENABLE]){shadowCamera=&packet.constants;break;}
 XMFLOAT3 sun{.4f,.5f,.8f};bool sunFound=false;
 for(const auto &packet:scenePackets_){
  const auto &c=packet.constants;
  bool triangles=packet.primitive==D3DPT_TRIANGLELIST||packet.primitive==D3DPT_TRIANGLESTRIP;
  const bool terrainCaster=modernShadows_&&terrainShadows_&&TerrainShadowCaster(packet);
  if(!terrainCaster&&(!triangles||packet.terrain||packet.vertexShader||packet.pixelShader||!(packet.fvf&D3DFVF_NORMAL)||c.geometryFlags[0]>.5f||(c.geometryFlags[1]<.5f&&c.modernMaterial[0]<.5f)||
    !packet.States().render[D3DRS_ZWRITEENABLE]||packet.States().render[D3DRS_ALPHABLENDENABLE]))continue;
  if(terrainCaster)++terrainShadowStats_.inputs;
  casters.push_back(&packet);std::array<XMFLOAT3,8> bounds{};XMFLOAT4X4 world;std::memcpy(&world,c.world,64);auto matrix=XMLoadFloat4x4(&world);
  // Fit the shadow frustum from eight conservative box corners. Transforming
  // and retaining every high-detail model vertex here cost CPU time and RAM.
  XMFLOAT3 localLow{FLT_MAX,FLT_MAX,FLT_MAX},localHigh{-FLT_MAX,-FLT_MAX,-FLT_MAX};
  if(packet.geometry&&packet.geometry->boundsReady){
   const auto &g=*packet.geometry;localLow={g.low[0],g.low[1],g.low[2]};localHigh={g.high[0],g.high[1],g.high[2]};
  }else for(std::size_t i=0;i<packet.VertexCount();++i){const auto *position=packet.Position(i);
   localLow.x=std::min(localLow.x,position[0]);localLow.y=std::min(localLow.y,position[1]);localLow.z=std::min(localLow.z,position[2]);
   localHigh.x=std::max(localHigh.x,position[0]);localHigh.y=std::max(localHigh.y,position[1]);localHigh.z=std::max(localHigh.z,position[2]);
  }
  if(packet.VertexCount()!=0)for(UINT corner=0;corner<8;++corner){XMFLOAT3 position;
   XMStoreFloat3(&position,XMVector3TransformCoord(XMVectorSet(corner&1?localHigh.x:localLow.x,corner&2?localHigh.y:localLow.y,corner&4?localHigh.z:localLow.z,1),matrix));positions.push_back(position);bounds[corner]=position;}
  if(terrainCaster)ExpandTerrainShadowBounds(bounds);
  casterBounds.push_back(bounds);
  if(!sunFound)for(UINT light=0;light<4;++light)if(c.lightPosition[light][3]>2.5f){
   sun={-c.lightDirection[light][0],-c.lightDirection[light][1],-c.lightDirection[light][2]};sunFound=true;break;}
 }
 if(!shadowCamera&&!casters.empty())shadowCamera=&casters.front()->constants;
 const bool taaRequested=taaEnabled_&&primaryDepthTarget_&&!offscreen_;
 if(primaryDepthTarget_&&!offscreen_){
  const LegacyConstants *captureCamera=nullptr;
  for(const auto &packet:scenePackets_)if(packet.terrain&&AtmosphereWorldPacket(packet)&&packet.constants.modernMaterial[0]>.5f){captureCamera=&packet.constants;break;}
  if(!captureCamera)for(const auto &packet:scenePackets_)if(AtmosphereWorldPacket(packet)&&packet.constants.modernMaterial[0]>.5f){captureCamera=&packet.constants;break;}
  if(captureCamera){
   const HRESULT prepared=renderer_.PrepareScreenMaterials();
   if(prepared==S_OK&&renderer_.ScreenMaterials().SelectCamera(captureCamera->view,captureCamera->projection,captureCamera->viewport,captureCamera->bridgeFlags+2)){
    // A selected native directional light, not a fallback cascade direction.
    // Additional native suns must retain their own unaffected direct component.
    auto &capture=renderer_.ScreenMaterials();capture.SelectSun(captureCamera->lightPosition,captureCamera->lightDirection);
    taaActive_=taaRequested;
    // taa101: the same sub-pixel offset in clip space for the game's vertex programs.
    if(taaActive_){const float *viewport=capture.camera.viewport;const float jitter[4]{2*taaJitter_[0]/std::max(viewport[2],1.0f),-2*taaJitter_[1]/std::max(viewport[3],1.0f),0,0};
     if(FAILED(WriteBuffer(renderer_.Context(),taaVertexJitter_.Get(),jitter,16)))taaActive_=false;}
    if(capture.camera.sunDirection[3]<.5f)for(const auto &candidate:scenePackets_){
     const auto &c=candidate.constants;
     if(AtmosphereWorldPacket(candidate)&&capture.Matches(c.view,c.projection,c.viewport,c.bridgeFlags+2)&&capture.SelectSun(c.lightPosition,c.lightDirection))break;
    }
   }
   else if(FAILED(prepared))D3D11Diagnostic("Renderer51 material capture unavailable; complete native material rendering retained");
  }
 }

 if(!std::isfinite(sun.x)||!std::isfinite(sun.y)||!std::isfinite(sun.z)||sun.x*sun.x+sun.y*sun.y+sun.z*sun.z<1e-8f)sun={.4f,.5f,.8f};
 shadowData_.nearSettings[3]=0;HRESULT hr=S_OK;auto *context=renderer_.Context();
 static bool castersAudited=false;
 if(!castersAudited&&scenePackets_.size()>50){D3D11Diagnostic(("Scene shadow caster draws: "+std::to_string(casters.size())).c_str());castersAudited=true;}
 const auto shadowStart=std::chrono::steady_clock::now();
 if(modernShadows_&&!positions.empty()){
  const float sunValues[]={sun.x,sun.y,sun.z};
  auto plan=shadowCamera?PlanShadowCascades(shadowCamera->view,shadowCamera->projection,sunValues,cascadeSize_,cascadeSplit_):ShadowCascadePlan{};
  shadowData_=plan.constants;
  if(!plan.valid){
  XMFLOAT3 low=positions[0],high=low;
  for(const auto &p:positions){low.x=std::min(low.x,p.x);low.y=std::min(low.y,p.y);low.z=std::min(low.z,p.z);high.x=std::max(high.x,p.x);high.y=std::max(high.y,p.y);high.z=std::max(high.z,p.z);}
  auto center=XMVectorSet((low.x+high.x)*.5f,(low.y+high.y)*.5f,(low.z+high.z)*.5f,1);
  float radius=std::max({high.x-low.x,high.y-low.y,high.z-low.z,64.0f});
  auto direction=XMVector3Normalize(XMLoadFloat3(&sun));auto up=std::abs(sun.z)>.99f&&std::abs(sun.x)+std::abs(sun.y)<.01f?XMVectorSet(0,1,0,0):XMVectorSet(0,0,1,0);
  auto view=XMMatrixLookAtLH(XMVectorAdd(center,XMVectorScale(direction,radius*2+512)),center,up);
  XMFLOAT3 lightLow{FLT_MAX,FLT_MAX,FLT_MAX},lightHigh{-FLT_MAX,-FLT_MAX,-FLT_MAX};
  for(const auto &position:positions){XMFLOAT3 p;XMStoreFloat3(&p,XMVector3TransformCoord(XMLoadFloat3(&position),view));
   lightLow.x=std::min(lightLow.x,p.x);lightLow.y=std::min(lightLow.y,p.y);lightLow.z=std::min(lightLow.z,p.z);lightHigh.x=std::max(lightHigh.x,p.x);lightHigh.y=std::max(lightHigh.y,p.y);lightHigh.z=std::max(lightHigh.z,p.z);}
  float width=lightHigh.x-lightLow.x+128,height=lightHigh.y-lightLow.y+128;
  const float mapSize=static_cast<float>(cascadeSize_);float cx=std::floor(((lightHigh.x+lightLow.x)*.5f)/(width/mapSize))*(width/mapSize);
  float cy=std::floor(((lightHigh.y+lightLow.y)*.5f)/(height/mapSize))*(height/mapSize);
  auto projection=XMMatrixOrthographicOffCenterLH(cx-width*.5f,cx+width*.5f,cy-height*.5f,cy+height*.5f,std::max(.01f,lightLow.z-512),lightHigh.z+512);
  XMFLOAT4X4 matrix;XMStoreFloat4x4(&matrix,XMMatrixMultiply(view,projection));std::memcpy(shadowData_.nearVP,&matrix,64);
  shadowData_.nearSettings[0]=.0008f;shadowData_.nearSettings[1]=shadowData_.nearSettings[2]=1.0f/mapSize;shadowData_.nearSettings[3]=1;
  std::copy_n(shadowData_.nearVP,16,shadowData_.farVP);std::copy_n(shadowData_.nearSettings,4,shadowData_.farSettings);
  shadowData_.split[3]=1;
  const auto fallbackSun=XMVector3Normalize(XMLoadFloat3(&sun));XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(shadowData_.sun),fallbackSun);
  }
  ++shadowStats_.frames;shadowStats_.inputs+=casters.size();if(plan.valid)++shadowStats_.cascadeFrames;else ++shadowStats_.fallbackFrames;
  ComPtr<ID3D11RenderTargetView> color;ComPtr<ID3D11DepthStencilView> depth;context->OMGetRenderTargets(1,color.GetAddressOf(),depth.GetAddressOf());
  passTimer_.Mark(context,GpuPhase::Shadow);if(stats_.frames%16==0)shadowDrawTimer_.Begin(context);
  ID3D11ShaderResourceView *empty[11]{};context->PSSetShaderResources(0,11,empty);
  for(UINT cascade=0;cascade<(plan.valid?2u:1u);++cascade){
   const auto start=std::chrono::steady_clock::now();
   std::vector<const DrawPacket*> retained;retained.reserve(casters.size());
   const float *vp=cascade?shadowData_.farVP:shadowData_.nearVP;
   for(std::size_t i=0;i<casters.size();++i)if(!plan.valid||ShadowBoundsIntersect(casterBounds[i],vp))retained.push_back(casters[i]);
   shadowStats_.retained[cascade]+=retained.size();shadowStats_.culled[cascade]+=casters.size()-retained.size();
   shadowStats_.texelSum[cascade]+=plan.texel[cascade];
   shadowData_.render[0]=static_cast<float>(cascade);
   const auto cacheStart=std::chrono::steady_clock::now();
   std::vector<const DrawPacket*> fixed,live;ShadowReuseCache::Plan cachePlan;
   if(shadowCache_){
    // Stamps are captured into per-cascade scratch kept across frames (perf86);
    // CaptureShadowStamp fully rewrites a slot before it is counted.
    auto &stamps=shadowStampScratch_[cascade];auto &stamped=shadowStamped_;stamped.clear();std::size_t stampCount=0;
    for(std::size_t i=0;i<retained.size();++i){
     if(stampCount<ShadowReuseCache::MaxDraws){if(stamps.size()<=stampCount)stamps.emplace_back();
      if(CaptureShadowStamp(*retained[i],stamps[stampCount],terrainExclusionRevision_)){++stampCount;stamped.push_back(i);continue;}}
     if(retained[i]->terrain)++workStats_.terrainUntracked;}
    cachePlan=shadowReuse_[cascade].Prepare(stamps,stampCount,vp);
    workStats_.terrainTracked+=cachePlan.terrainTracked;workStats_.terrainFirst+=cachePlan.terrainFirst;
    workStats_.terrainWorldChanged+=cachePlan.terrainWorldChanged;workStats_.terrainCameraChanged+=cachePlan.terrainCameraChanged;
    workStats_.terrainSourceChanged+=cachePlan.terrainSourceChanged;workStats_.terrainMapChanged+=cachePlan.terrainMapChanged;
    workStats_.terrainShapeChanged+=cachePlan.terrainShapeChanged;workStats_.terrainOwnerChanged+=cachePlan.terrainOwnerChanged;
    std::size_t stampIndex=0;
    for(std::size_t i=0;i<retained.size();++i){const bool tracked=stampIndex<stamped.size()&&stamped[stampIndex]==i;
     if(tracked&&cachePlan.stationary[stampIndex])fixed.push_back(retained[i]);else live.push_back(retained[i]);
     if(tracked)++stampIndex;}
   }else live=retained;
   shadowCacheStats_.preparationMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cacheStart).count();
   ++shadowCacheStats_.passes;shadowCacheStats_.liveDraws+=live.size();
   for(const auto *p:fixed)if(p->terrain)++terrainShadowStats_.stationary[cascade];
   for(const auto *p:live)if(p->terrain)++terrainShadowStats_.live[cascade];
   for(const auto *p:retained)if(p->terrain)++terrainShadowStats_.retained[cascade];
   if(shadowCache_){
    if(cachePlan.hit){++shadowCacheStats_.hits;shadowCacheStats_.savedDraws+=fixed.size();shadowReuse_[cascade].DiscardPending();}
    else{
     ++shadowCacheStats_.rebuilds;shadowCacheStats_.staticDraws+=fixed.size();
     context->OMSetRenderTargets(0,nullptr,staticShadowCascades_.Depth(cascade));
     context->ClearDepthStencilView(staticShadowCascades_.Depth(cascade),D3D11_CLEAR_DEPTH,1,0);
     const auto staticGroups=GroupOpaque(fixed);shadowStats_.groups[cascade]+=staticGroups.size();
     for(const auto &group:staticGroups)if(FAILED(hr=Execute(*group.front(),true,&group,cascadeSize_)))break;
     if(FAILED(hr)){shadowReuse_[cascade].Reset();break;}shadowReuse_[cascade].Commit();
    }
   }else ++shadowCacheStats_.bypassed;
   context->OMSetRenderTargets(0,nullptr,nullptr);
   if(shadowCache_&&!fixed.empty()){shadowCascades_.CopyLayerFrom(context,staticShadowCascades_,cascade);++shadowCacheStats_.copies;}
   else context->ClearDepthStencilView(shadowCascades_.Depth(cascade),D3D11_CLEAR_DEPTH,1,0);
   context->OMSetRenderTargets(0,nullptr,shadowCascades_.Depth(cascade));
   const auto shadowGroups=GroupOpaque(live);shadowStats_.groups[cascade]+=shadowGroups.size();
   for(const auto &group:shadowGroups)if(FAILED(hr=Execute(*group.front(),true,&group,cascadeSize_)))break;
   shadowStats_.submitMs[cascade]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(FAILED(hr))break;
  }
  shadowData_.render[0]=0;
  ID3D11RenderTargetView *restore=color.Get();context->OMSetRenderTargets(1,&restore,depth.Get());if(stats_.frames%16==0)shadowDrawTimer_.End(context);
  if(FAILED(hr)){scenePackets_.clear();return Fail(hr,"scene shadow pass");}
 }
 stats_.shadowMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-shadowStart).count();
 const auto sceneStart=std::chrono::steady_clock::now();
 if(stats_.frames%16==0)lightPreparationTimer_.Begin(context);
 // Prepare only a primary terrain camera. No hidden-object or gameplay weather queries.
 for(const auto &packet:scenePackets_)if(packet.terrain&&packet.constants.geometryFlags[0]<.5f&&packet.States().render[D3DRS_ZENABLE]){
  const auto &mapWeather=GetD3D11MapWeather();atmosphere_.SetClimate(packet.terrainClimate,mapWeather.snowEnabled,mapWeather.epoch);
  // accum105: settled snow on alpine maps or under mission snow, dust on arid maps.
  const std::uint32_t accumulation=!accumulationEnabled_?0u:(mapWeather.snowEnabled||packet.terrainClimate.kind==MapClimate::Alpine)?524288u:
   packet.terrainClimate.kind==MapClimate::Arid?1048576u:0u;
  accumulationFlags_.store(accumulation,std::memory_order_relaxed);
  if(accumulation!=accumulationReported_){accumulationReported_=accumulation;
   GenZHLoad::Record("accumulation_v105",accumulation==524288u?"snow":accumulation==1048576u?"dust":"none",0);}
  const auto &c=packet.constants;float nativeSun[]={sun.x,sun.y,sun.z},nativeColor[]={0,0,0},nativeAmbient[3]{};
  auto linear=[](float x){return NativeLightLinear(x);};
  for(UINT i=0;i<3;++i)nativeAmbient[i]=linear(c.globalAmbient[i]);
  for(UINT light=0;light<4;++light)if(c.lightPosition[light][3]>2.5f){
   for(UINT i=0;i<3;++i){nativeSun[i]=-c.lightDirection[light][i];nativeColor[i]=linear(c.lightDiffuse[light][i]);}break;
  }
  const D3D11_VIEWPORT viewport{packet.constants.viewport[0],packet.constants.viewport[1],packet.constants.viewport[2],packet.constants.viewport[3],packet.constants.bridgeFlags[2],packet.constants.bridgeFlags[3]};
  LocalVolumeSources localSources;
  std::array<LocalVolumeLight,LocalVolumeLightLimit> localNativeKeys{};localShadowBindings_.Clear();
  // perf86: a packet whose light fields equal the previous primary packet's
  // (bitwise) repeats its outcomes; Repeat keeps every counter identical.
  float previousLights[5][4][4];bool previousLightsValid=false;std::array<std::uint8_t,4> previousOutcomes{};
  for(const auto &worldPacket:scenePackets_){const auto &lightPacket=worldPacket.constants;
   if(!PrimaryLocalVolumePacket(lightPacket,c,worldPacket.States().render[D3DRS_ZENABLE]!=0))continue;
   float lights[5][4][4];std::memcpy(lights[0],lightPacket.lightPosition,64);std::memcpy(lights[1],lightPacket.lightDiffuse,64);
   std::memcpy(lights[2],lightPacket.lightAttenuation,64);std::memcpy(lights[3],lightPacket.lightSpot,64);std::memcpy(lights[4],lightPacket.lightDirection,64);
   if(previousLightsValid&&!std::memcmp(lights,previousLights,sizeof(lights))){localSources.Repeat(previousOutcomes);continue;}
   for(unsigned light=0;light<4;++light){const auto before=localSources.data.metadata[0];const auto outcome=localSources.AddOutcome(lightPacket,light);
    if(outcome==LocalVolumeSources::Inserted)localNativeKeys[before]=NativeLocalShadowKey(lightPacket,light);previousOutcomes[light]=outcome;}
   std::memcpy(previousLights,lights,sizeof(lights));previousLightsValid=true;
  }
  LocalShadowPlan localPlan;
  const auto localShadowStart=std::chrono::steady_clock::now();
  if(modernShadows_&&localSources.data.metadata[0]){
   XMFLOAT4X4 nativeView;std::memcpy(&nativeView,c.view,64);XMFLOAT3 eye;XMStoreFloat3(&eye,XMVector3TransformCoord(XMVectorZero(),XMMatrixInverse(nullptr,XMLoadFloat4x4(&nativeView))));
   const float camera[]={eye.x,eye.y,eye.z};localPlan=PlanLocalShadows(localSources.data,camera);localShadowStats_.selected+=localPlan.count;
   if(localPlan.count&&!localShadowAtlas_.Available()&&FAILED(hr=localShadowAtlas_.Create(renderer_.Device())))return Fail(hr,"local point/spot shadow atlas");
   ComPtr<ID3D11RenderTargetView> color;ComPtr<ID3D11DepthStencilView> depth;context->OMGetRenderTargets(1,color.GetAddressOf(),depth.GetAddressOf());
   const auto savedShadows=shadowData_;UINT viewportCount=1;D3D11_VIEWPORT savedViewport{};context->RSGetViewports(&viewportCount,&savedViewport);
   ID3D11ShaderResourceView *emptyLocal=nullptr;context->CSSetShaderResources(36,1,&emptyLocal);context->PSSetShaderResources(36,1,&emptyLocal);
   for(unsigned slot=0;slot<localPlan.count;++slot){
    struct Face {std::vector<const DrawPacket*> draws;std::vector<ShadowDrawStamp> stamps;bool cacheable=true,hit=false;std::uint64_t triangles=0;};std::array<Face,6> faces;
    std::uint64_t cost=0;
    for(unsigned face=0;face<localPlan.faceCounts[slot];++face){auto &f=faces[face];const unsigned layer=slot*6+face;const auto *matrix=localPlan.data.matrices[layer];
     for(std::size_t i=0;i<casters.size();++i){const auto &q=casters[i]->constants;
      if(q.bridgeFlags[0]>.5f||std::memcmp(q.view,c.view,64)||std::memcmp(q.projection,c.projection,64)||std::memcmp(q.viewport,c.viewport,16)||q.bridgeFlags[2]!=c.bridgeFlags[2]||q.bridgeFlags[3]!=c.bridgeFlags[3]||!LocalShadowBoundsIntersect(casterBounds[i],matrix))continue;
      f.draws.push_back(casters[i]);const auto indexCount=casters[i]->Indices().size();f.triangles+=casters[i]->primitive==D3DPT_TRIANGLESTRIP?(indexCount>2?indexCount-2:0):indexCount/3;
      ShadowDrawStamp stamp;if(f.stamps.size()<LocalShadowFaceCache::MaxDraws&&CaptureShadowStamp(*casters[i],stamp,terrainExclusionRevision_))f.stamps.push_back(std::move(stamp));else f.cacheable=false;
     }
     f.hit=f.cacheable&&localShadowCache_[layer].Matches(f.stamps,matrix);if(!f.hit)cost+=f.triangles;
    }
    // Reject the complete light before rendering any face. Partial caster maps
    // would introduce incorrect holes. The source retains full unshadowed light.
    if(cost>LocalShadowTriangleBudget-localShadowFrameTriangles_){++localShadowStats_.budgetBypass;continue;}
    for(unsigned face=0;face<localPlan.faceCounts[slot];++face){auto &f=faces[face];const unsigned layer=slot*6+face;const auto *matrix=localPlan.data.matrices[layer];
     if(f.hit){++localShadowStats_.hits;continue;}
     std::memcpy(shadowData_.nearVP,matrix,64);shadowData_.render[0]=0;
     context->OMSetRenderTargets(0,nullptr,localShadowAtlas_.Depth(layer));context->ClearDepthStencilView(localShadowAtlas_.Depth(layer),D3D11_CLEAR_DEPTH,1,0);
     const auto localGroups=GroupOpaque(f.draws);
     for(const auto &group:localGroups){if(FAILED(hr=Execute(*group.front(),true,&group,LocalShadowSize)))break;++localShadowStats_.draws;}
     if(FAILED(hr)){localShadowCache_[layer].Reset();break;}
     if(f.cacheable)localShadowCache_[layer].Commit(std::move(f.stamps),matrix);else localShadowCache_[layer].Reset();
     ++localShadowStats_.faces;localShadowStats_.triangles+=f.triangles;localShadowFrameTriangles_+=f.triangles;
    }
    if(FAILED(hr))break;
    localSources.data.lights[localPlan.sources[slot]].direction[3]=float(slot+1);localShadowBindings_.Add(localNativeKeys[localPlan.sources[slot]],slot);++localShadowStats_.shadowed;
   }
   shadowData_=savedShadows;ID3D11RenderTargetView *restore=color.Get();context->OMSetRenderTargets(1,&restore,depth.Get());if(viewportCount)context->RSSetViewports(1,&savedViewport);
   if(FAILED(hr)){scenePackets_.clear();return Fail(hr,"local shadow pass");}
  }
  for(unsigned i=0;i<localSources.data.metadata[0];++i)if(localSources.data.lights[i].direction[3]<.5f)++localShadowStats_.unshadowed;
  localShadowStats_.cpuMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-localShadowStart).count();
  atmosphere_.SetLocalSources(localSources);atmosphere_.SetLocalShadows(localPlan.data,localPlan.count?localShadowAtlas_.View():nullptr);
  localSurfaceLighting_.Prepare(context,localPlan.data,localPlan.count?localShadowAtlas_.View():nullptr,c.view,c.projection,viewport);if(localSurfaceLighting_.Prepared())++localSurfaceStats_.prepared;
  const HRESULT atmosphereResult=atmosphere_.Prepare(context,c.view,c.projection,nativeSun,nativeColor,shadowData_,shadowCascades_.View(),viewport,atmosphereDelta_,nativeAmbient);
  if(atmosphereResult==S_OK&&!worldFrameDrawn_){
   atmosphere_.DrawSky(context,renderer_.SceneTarget());
   auto *target=renderer_.SceneTarget();context->OMSetRenderTargets(1,&target,renderer_.SceneDepth());
  }
  break;
 }
 if(stats_.frames%16==0)lightPreparationTimer_.End(context);
 worldFrameDrawn_=true;
 const auto preparationStart=std::chrono::steady_clock::now();
 auto visible=PrepareCameraScene(scenePackets_,cameraCulling_);
 nativeStats_+=visible.native;
 sceneStats_.preparationMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-preparationStart).count();
 sceneStats_.seen+=visible.seen;sceneStats_.boundsTested+=visible.boundsTested;sceneStats_.culled+=visible.culled;sceneStats_.fallback+=visible.fallback;sceneStats_.shadowInputs+=casters.size();
 const auto groupingStart=std::chrono::steady_clock::now();
 GroupingStats grouping;auto groups=GroupScreenDisjoint(visible.packets,grouping,&visible.prepared);
 workStats_.groupingCpuMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-groupingStart).count();
 workStats_.sceneSeen+=grouping.seen;workStats_.sceneEligible+=grouping.reasons[0];workStats_.sceneGroups+=grouping.groups;workStats_.reorderedInstances+=grouping.reordered;
 for(unsigned i=0;i<grouping.reasons.size();++i)workStats_.eligibility[i]+=grouping.reasons[i];
 const bool arena=sceneConstantArena_.Prepare(renderer_.Device(),groups,[&](const DrawPacket &packet){return DrawConstants(packet,false);});
 if(arena)++workStats_.constantUploads;
 // Only qualifying runs build a table. Isolated original draws do not pay
 // a per-vertex scan or allocation. Their shaders never read the palette.
 char materialPaletteChoice[32]{};GetEnvironmentVariableA("GENZH_SORTED_MATERIAL_PALETTE",materialPaletteChoice,sizeof(materialPaletteChoice));
 ScopedSortedMaterialPaletteReference materialPaletteReference(sortedMaterialPaletteReference||std::strcmp(materialPaletteChoice,"off")==0);
 SortedColorPalettes palettes(sortedColorPaletteEnabled_);
 std::vector<std::size_t> runEnds(groups.size());
 // perf89: single-packet groups' SortedColorPacket (a pure predicate) once per group,
 // instead of again for the run head and the candidate in every comparison.
 std::vector<std::uint8_t> sortedColorHeads(groups.size());
 for(std::size_t i=0;i<groups.size();++i)sortedColorHeads[i]=groups[i].size()==1&&SortedColorPacket(*groups[i].front());
 for(std::size_t i=0;i<groups.size();){
  std::size_t end=i+1;std::uint64_t indices=groups[i].front()->Indices().size();
  if(sortedColorHeads[i])
   while(end<groups.size()&&sortedColorHeads[end]&&
         ConsecutiveSortedColorEligible(*groups[i].front(),*groups[end-1].front(),*groups[end].front())&&
         indices+groups[end].front()->Indices().size()<=UINT_MAX){indices+=groups[end].front()->Indices().size();++end;}
  runEnds[i]=end;
  if(end>i+1)for(auto n=i;n<end;++n)palettes.Add(*groups[n].front(),true);
  i=end;
 }
 {ScopedScenePipelineBindings bindings(sceneBindings_,sceneBindingsEnabled_);
  for(std::size_t i=0;i<groups.size();){
   // perf94: the first screen-space draw after the world (pretransformed or
   // orthographic: HUD, radar, text) gets the world passes underneath it.
   if(!worldPassesDone_&&frameHasWorldDepth_&&!offscreen_&&primaryDepthTarget_&&ScreenSpacePacket(*groups[i].front())){
    sceneBindings_.End();
    if(FAILED(hr=FinishWorldPasses()))break;
    ID3D11RenderTargetView *scene=renderer_.SceneTarget();renderer_.Context()->OMSetRenderTargets(1,&scene,renderer_.SceneDepth());
    screenTargetsBound_=false;++shroudFogStats_.beforeHud;
    sceneBindings_.Begin(sceneBindingsEnabled_);
   }
   sceneConstantIndex_=arena?static_cast<UINT>(i):UINT_MAX;
   const auto end=runEnds[i];auto *palette=end>i+1?palettes.Find(*groups[i].front()):nullptr;
   if(palette&&palette->Create(renderer_.Device())){
    UINT indices=0;for(auto n=i;n<end;++n)indices+=static_cast<UINT>(groups[n].front()->Indices().size());
    hr=Execute(*groups[i].front(),false,nullptr,2048,palette,indices);
    if(SUCCEEDED(hr)){++sortedColorPaletteCounts.draws;sortedColorPaletteCounts.savedDraws+=end-i-1;
     for(auto n=i+1;n<end;++n)if(std::memcmp(groups[i].front()->constants.materialDiffuse,groups[n].front()->constants.materialDiffuse,48))++sortedColorPaletteCounts.materialSavedDraws;
    }
    i=end;
   }else{hr=Execute(*groups[i].front(),false,&groups[i]);++i;}
   if(FAILED(hr))break;
  }
  SortedColorPalette::Unbind(renderer_.Context());
 }
 sceneConstantIndex_=UINT_MAX;sceneConstantArena_.End();
 static double lastConstantReport=0;const double now=GenZHLoad::Seconds();
 if(now-lastConstantReport>=1){const auto &c=sceneConstantCounts;
  GenZHLoad::Record("scene_constant_arena_v73","supported="+std::to_string(sceneConstantArena_.Supported())+" uploads="+std::to_string(c.uploads)+" slices="+std::to_string(c.slices)+" binds="+std::to_string(c.binds)+" bypassed="+std::to_string(c.bypassed)+" failures="+std::to_string(c.failures),0);lastConstantReport=now;
  GenZHLoad::Record("scene_bindings_v75","scopes="+std::to_string(sceneBindings_.scopes)+" requests="+std::to_string(sceneBindings_.requests)+" calls="+std::to_string(sceneBindings_.calls)+" skips="+std::to_string(sceneBindings_.skips),0);
  const auto &p=sortedColorPaletteCounts;
  GenZHLoad::Record("sorted_color_palette_v78","builds="+std::to_string(p.builds)+" gpu_bytes="+std::to_string(p.gpuBytes)+" draws="+std::to_string(p.draws)+" saved_draws="+std::to_string(p.savedDraws)+" material_saved_draws_v81="+std::to_string(p.materialSavedDraws)+" conflicts="+std::to_string(p.conflicts)+" budget_bypass="+std::to_string(p.budgetBypass)+" gpu_failures="+std::to_string(p.gpuFailures),0);
  const auto &rr=resolvedRaster_.counts;const auto &rb=resolvedBlend_.counts;const auto &rd=resolvedDepth_.counts;const auto &rs=resolvedSampler_.counts;
  GenZHLoad::Record("resolved_states_v76","requests="+std::to_string(rr.requests+rb.requests+rd.requests+rs.requests)+" hits="+std::to_string(rr.hits+rb.hits+rd.hits+rs.hits)+" misses="+std::to_string(rr.misses+rb.misses+rd.misses+rs.misses)+" bypassed="+std::to_string(rr.bypassed+rb.bypassed+rd.bypassed+rs.bypassed)+" failures="+std::to_string(rr.failures+rb.failures+rd.failures+rs.failures),0);
 }
 stats_.sceneMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-sceneStart).count();
 scenePackets_.clear();return hr;
}

HRESULT CompatibilityRenderer::MapSurface(IDirect3DSurface8 *surface,SurfaceMapping *&mapping){
 if(!surface){mapping=nullptr;return S_OK;}
 auto found=surfaces_.find(surface);if(found!=surfaces_.end()){mapping=&found->second;return S_OK;}
 SurfaceMapping created;created.native=surface;D3DSURFACE_DESC desc{};HRESULT hr=surface->GetDesc(&desc);if(FAILED(hr))return hr;
 if(surface==nativeDefault_.Get()){
  renderer_.SceneTarget()->GetResource(created.resource.GetAddressOf());created.target=renderer_.SceneTarget();
 }else if(desc.Usage&D3DUSAGE_DEPTHSTENCIL){
  D3D11_TEXTURE2D_DESC gpu{};gpu.Width=desc.Width;gpu.Height=desc.Height;gpu.ArraySize=gpu.MipLevels=gpu.SampleDesc.Count=1;
  gpu.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;gpu.BindFlags=D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> resource;if(FAILED(hr=renderer_.Device()->CreateTexture2D(&gpu,nullptr,resource.GetAddressOf())))return hr;
  if(FAILED(hr=renderer_.Device()->CreateDepthStencilView(resource.Get(),nullptr,created.depth.GetAddressOf())))return hr;created.resource=resource;
 }else{
  ComPtr<IDirect3DBaseTexture8> texture;hr=surface->GetContainer(IID_IDirect3DBaseTexture8,reinterpret_cast<void**>(texture.GetAddressOf()));
  if(SUCCEEDED(hr)){
   // Reuse the metadata already associated with the native resource.
   for(auto &entry:textures_){auto owner=entry.second.owner.lock();if(owner&&owner->native.Get()==texture.Get()){created.owner=owner;break;}}
   if(!created.owner){created.owner=std::make_shared<TextureData>();created.owner->native=texture;}
   GPUTexture *gpu=nullptr;if(FAILED(hr=GetTexture(created.owner,gpu)))return hr;created.resource=gpu->texture;
   ComPtr<ID3D11Texture2D> resource;if(FAILED(hr=created.resource.As(&resource)))return hr;D3D11_TEXTURE2D_DESC description{};resource->GetDesc(&description);
   UINT face=0,mip=0;bool matched=false;
   for(UINT f=0;f<description.ArraySize&&!matched;++f)for(UINT m=0;m<texture->GetLevelCount();++m){ComPtr<IDirect3DSurface8> candidate;
    if(texture->GetType()==D3DRTYPE_CUBETEXTURE)hr=static_cast<IDirect3DCubeTexture8*>(texture.Get())->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(f),m,candidate.GetAddressOf());
    else hr=static_cast<IDirect3DTexture8*>(texture.Get())->GetSurfaceLevel(m,candidate.GetAddressOf());
    if(SUCCEEDED(hr)&&candidate.Get()==surface){face=f;mip=m;matched=true;break;}}
   if(!matched||mip>=description.MipLevels)return Fail(E_INVALIDARG,"render surface subresource mismatch");
   created.subresource=D3D11CalcSubresource(mip,face,description.MipLevels);D3D11_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=description.Format;
   rtv.ViewDimension=description.ArraySize>1?D3D11_RTV_DIMENSION_TEXTURE2DARRAY:D3D11_RTV_DIMENSION_TEXTURE2D;
   if(description.ArraySize>1){rtv.Texture2DArray.MipSlice=mip;rtv.Texture2DArray.FirstArraySlice=face;rtv.Texture2DArray.ArraySize=1;}else rtv.Texture2D.MipSlice=mip;
   if(FAILED(hr=renderer_.Device()->CreateRenderTargetView(resource.Get(),&rtv,created.target.GetAddressOf())))return hr;
  }else{
   D3D11_TEXTURE2D_DESC gpu{};gpu.Width=desc.Width;gpu.Height=desc.Height;gpu.ArraySize=gpu.MipLevels=gpu.SampleDesc.Count=1;
   gpu.Format=DXGI_FORMAT_R8G8B8A8_UNORM;gpu.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
   ComPtr<ID3D11Texture2D> resource;if(FAILED(hr=renderer_.Device()->CreateTexture2D(&gpu,nullptr,resource.GetAddressOf())))return hr;
   if(FAILED(hr=renderer_.Device()->CreateRenderTargetView(resource.Get(),nullptr,created.target.GetAddressOf())))return hr;created.resource=resource;
  }
 }
 auto inserted=surfaces_.emplace(surface,std::move(created));mapping=&inserted.first->second;return S_OK;
}
HRESULT CompatibilityRenderer::SetTarget(IDirect3DSurface8 *color,IDirect3DSurface8 *depth){
 HRESULT flush=FlushScene();if(FAILED(flush))return flush;
 screenTargetsBound_=false;
 if(!color){ComPtr<IDirect3DSurface8> current;HRESULT hr=native_->GetRenderTarget(current.GetAddressOf());if(FAILED(hr))return hr;color=current.Get();}
 SurfaceMapping *mapped=nullptr;HRESULT hr=MapSurface(color,mapped);if(FAILED(hr)||!mapped)return FAILED(hr)?hr:E_INVALIDARG;
 ComPtr<ID3D11DepthStencilView> ds;
 if(depth){D3DSURFACE_DESC desc{};depth->GetDesc(&desc);D3DSURFACE_DESC colorDesc{};color->GetDesc(&colorDesc);
  if(color==nativeDefault_.Get()&&desc.Width==colorDesc.Width&&desc.Height==colorDesc.Height)ds=renderer_.SceneDepth();
  else{SurfaceMapping *depthMap=nullptr;if(FAILED(hr=MapSurface(depth,depthMap)))return hr;ds=depthMap->depth;}}
 std::array<ID3D11ShaderResourceView*,8> empty{};renderer_.Context()->PSSetShaderResources(0,8,empty.data());
 ID3D11RenderTargetView *target=mapped->target.Get();renderer_.Context()->OMSetRenderTargets(1,&target,ds.Get());offscreen_=color!=nativeDefault_.Get();primaryDepthTarget_=!offscreen_&&ds.Get()==renderer_.SceneDepth();return S_OK;
}
HRESULT CompatibilityRenderer::ReadSurface(ID3D11Resource *resource,UINT subresource,IDirect3DSurface8 *dest,bool linear){
 if(!resource||!dest)return E_POINTER;ComPtr<ID3D11Texture2D> texture;HRESULT hr=resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf()));if(FAILED(hr))return hr;
 D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);UINT mip=subresource%desc.MipLevels;
 UINT width=std::max(1u,desc.Width>>mip),height=std::max(1u,desc.Height>>mip);
 if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return E_NOTIMPL;
 D3D11_TEXTURE2D_DESC staging=desc;staging.Width=width;staging.Height=height;staging.MipLevels=staging.ArraySize=1;
 staging.BindFlags=staging.MiscFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> cpu;if(FAILED(hr=renderer_.Device()->CreateTexture2D(&staging,nullptr,cpu.GetAddressOf())))return hr;
 renderer_.Context()->CopySubresourceRegion(cpu.Get(),0,0,0,0,resource,subresource,nullptr);
 D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(hr=renderer_.Context()->Map(cpu.Get(),0,D3D11_MAP_READ,0,&mapped)))return hr;
 std::vector<std::uint8_t> bgra(std::size_t(width)*height*4);
 for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)for(UINT channel=0;channel<4;++channel){float value;
  const auto *row=static_cast<const std::uint8_t*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
  if(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT){std::uint16_t bits;std::memcpy(&bits,row+x*8+channel*2,2);value=DirectX::PackedVector::XMConvertHalfToFloat(bits);}
  else value=row[x*4+channel]/255.0f;
  if(linear&&channel<3)value=std::pow(std::max(value,0.0f),1.0f/2.2f);
  bgra[(std::size_t(y)*width+x)*4+(channel==0?2:channel==2?0:channel)]=static_cast<std::uint8_t>(std::clamp(value,0.0f,1.0f)*255+.5f);
 }
 renderer_.Context()->Unmap(cpu.Get(),0);
 RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
 return D3DXLoadSurfaceFromMemory(dest,nullptr,nullptr,bgra.data(),D3DFMT_A8R8G8B8,width*4,nullptr,&rect,D3DX_FILTER_NONE,0);
}
HRESULT CompatibilityRenderer::SynchronizeSurface(IDirect3DSurface8 *surface){
 auto found=surfaces_.find(surface);
 // CPU font/image sources were never written by DX11. Flushing the world here
 // split shadow/material passes and forced driver work inside HUD preparation.
 if(surface!=nativeDefault_.Get()&&found==surfaces_.end())return S_OK;
 HRESULT flush=FlushScene();if(FAILED(flush))return flush;
 if(surface==nativeDefault_.Get()){ComPtr<ID3D11Resource> resource;renderer_.SceneTarget()->GetResource(resource.GetAddressOf());return ReadSurface(resource.Get(),0,surface,true);}
 return found==surfaces_.end()?S_OK:ReadSurface(found->second.resource.Get(),found->second.subresource,surface,false);
}
HRESULT CompatibilityRenderer::SurfaceChanged(IDirect3DSurface8 *surface){
 auto found=surfaces_.find(surface);if(found==surfaces_.end()||!found->second.target)return S_OK;
 D3DSURFACE_DESC desc{};HRESULT hr=surface->GetDesc(&desc);if(FAILED(hr))return hr;
 ComPtr<IDirect3DSurface8> image;if(FAILED(hr=native_->CreateImageSurface(desc.Width,desc.Height,D3DFMT_A8R8G8B8,image.GetAddressOf())))return hr;
 if(FAILED(hr=D3DXLoadSurfaceFromSurface(image.Get(),nullptr,nullptr,surface,nullptr,nullptr,D3DX_FILTER_NONE,0)))return hr;
 D3DLOCKED_RECT lock{};if(FAILED(hr=image->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return hr;
 std::vector<std::uint8_t> rgba(std::size_t(desc.Width)*desc.Height*4);
 for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x){const auto *src=static_cast<const std::uint8_t*>(lock.pBits)+std::size_t(y)*lock.Pitch+x*4;
  auto *dst=rgba.data()+(std::size_t(y)*desc.Width+x)*4;dst[0]=src[2];dst[1]=src[1];dst[2]=src[0];dst[3]=src[3];}
 image->UnlockRect();ComPtr<ID3D11Texture2D> gpu;if(FAILED(hr=found->second.resource.As(&gpu)))return hr;D3D11_TEXTURE2D_DESC description{};gpu->GetDesc(&description);
 if(description.Format==DXGI_FORMAT_R16G16B16A16_FLOAT){std::vector<std::uint16_t> linear(rgba.size());
  for(std::size_t i=0;i<rgba.size();++i){float value=rgba[i]/255.0f;if(i%4!=3)value=std::pow(value,2.2f);linear[i]=DirectX::PackedVector::XMConvertFloatToHalf(value);}
  renderer_.Context()->UpdateSubresource(gpu.Get(),found->second.subresource,nullptr,linear.data(),desc.Width*8,0);
 }else renderer_.Context()->UpdateSubresource(gpu.Get(),found->second.subresource,nullptr,rgba.data(),desc.Width*4,0);return S_OK;
}
HRESULT CompatibilityRenderer::ReadFrontBuffer(IDirect3DSurface8 *surface){
 HRESULT flush=FlushScene();if(FAILED(flush))return flush;
 ComPtr<ID3D11Resource> resource;renderer_.OutputTarget()->GetResource(resource.GetAddressOf());return ReadSurface(resource.Get(),0,surface,false);
}
}
