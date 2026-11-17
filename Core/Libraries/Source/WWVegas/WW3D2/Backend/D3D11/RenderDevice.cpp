// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderDevice.h"
#include <dxgi1_6.h>
#include "ShaderSource.h"
#include "ShaderBytecodeCache.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include "ColourGrade.h"

namespace GenZH::Modern {
namespace {
HRESULT MakeConstants(ID3D11Device *device,UINT bytes,ID3D11Buffer **buffer) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth=bytes; desc.Usage=D3D11_USAGE_DYNAMIC;
    desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    return device->CreateBuffer(&desc,nullptr,buffer);
}
bool Finite(const float *values,UINT count) {
    for(UINT i=0;i<count;++i) if(!std::isfinite(values[i])) return false;
    return true;
}
}

HRESULT RenderDevice::Fail(HRESULT result,const char *operation) {
    std::ostringstream message; message<<operation<<" failed: HRESULT 0x"<<std::hex<<result;
    error_=message.str(); return result;
}

HRESULT RenderDevice::Initialize(HWND window,UINT width,UINT height,bool useWarp) {
    Shutdown(); error_.clear();
    char choice[32]{};GetEnvironmentVariableA("GENZH_GTAO",choice,sizeof(choice));screenAOEnabled_=std::strcmp(choice,"off")!=0;
    std::memset(choice,0,sizeof(choice));GetEnvironmentVariableA("GENZH_CONTACT_SHADOWS",choice,sizeof(choice));screenContactEnabled_=std::strcmp(choice,"off")!=0;
    GetEnvironmentVariableA("GENZH_AA",choice,sizeof(choice));postSettings_[2]=std::strcmp(choice,"off")==0?0.0f:1.0f;
    std::memset(choice,0,sizeof(choice));GetEnvironmentVariableA("GENZH_BLOOM",choice,sizeof(choice));SetBloomMode(std::strcmp(choice,"off")==0?0:std::strcmp(choice,"legacy")==0?1:2);
    std::memset(choice,0,sizeof(choice));GetEnvironmentVariableA("GENZH_GRADE",choice,sizeof(choice));SetColourGrade(std::strcmp(choice,"off")!=0);
    std::memset(choice,0,sizeof(choice));GetEnvironmentVariableA("GENZH_EXPOSURE_TRIM",choice,sizeof(choice));
    if(choice[0]){const double trim=std::atof(choice);postSettings_[5]=(std::isfinite(trim)&&trim>0)?float(std::clamp(trim,.1,4.0)):.85f;}
    std::memset(choice,0,sizeof(choice));GetEnvironmentVariableA("GENZH_GI",choice,sizeof(choice));giEnabled_=std::strcmp(choice,"off")!=0;
    if(!window || width==0 || height==0) return Fail(E_INVALIDARG,"Initialize dimensions/window");
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width=width; desc.BufferDesc.Height=height;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.OutputWindow=window; desc.Windowed=TRUE;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // present101: in a composed window a flip swap chain without tearing locked to
    // vblank divisors (measured: 72 FPS on a 144 Hz display, Present blocking 6.5 ms).
    // Tearing (where the system supports it) lets the game's own frame cap pace the
    // frames; a frame-latency waitable object bounds the queue instead of blocking
    // inside Present. GENZH_TEARING=off keeps the previous swap chain.
    std::memset(choice,0,sizeof(choice));GetEnvironmentVariableA("GENZH_TEARING",choice,sizeof(choice));
    const bool modernPresent=std::strcmp(choice,"off")!=0&&!useWarp;
    BOOL allowTearing=FALSE;
    if(modernPresent){ComPtr<IDXGIFactory5> factory5;if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory5.GetAddressOf()))))
        if(FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allowTearing,sizeof(allowTearing))))allowTearing=FALSE;}
    tearing_=allowTearing!=FALSE;
    if(modernPresent){desc.BufferCount=3;desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|(tearing_?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0);}
    swapFlags_=desc.Flags;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    ComPtr<IDXGIAdapter1> chosen;
    if(!useWarp){
        ComPtr<IDXGIFactory1> chooser;
        if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(chooser.GetAddressOf())))){
            ComPtr<IDXGIFactory6> modern;
            if(SUCCEEDED(chooser.As(&modern))){
                for(UINT index=0;;++index){ComPtr<IDXGIAdapter1> candidate;
                    if(FAILED(modern->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(candidate.GetAddressOf()))))break;
                    DXGI_ADAPTER_DESC1 info{};candidate->GetDesc1(&info);
                    if(!(info.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){chosen=candidate;break;}
                }
            }else{
                SIZE_T memory=0;for(UINT index=0;;++index){ComPtr<IDXGIAdapter1> candidate;
                    if(chooser->EnumAdapters1(index,candidate.GetAddressOf())==DXGI_ERROR_NOT_FOUND)break;
                    if(!candidate)continue;DXGI_ADAPTER_DESC1 info{};candidate->GetDesc1(&info);
                    if(!(info.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&(!chosen||info.DedicatedVideoMemory>memory)){chosen=candidate;memory=info.DedicatedVideoMemory;}
                }
            }
        }
    }
    auto driver=useWarp?D3D_DRIVER_TYPE_WARP:chosen?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(chosen.Get(),driver,nullptr,0,levels,2,D3D11_SDK_VERSION,
        &desc,swap_.GetAddressOf(),device_.GetAddressOf(),&selected,context_.GetAddressOf());
    if(hr==E_INVALIDARG) {
        swap_.Reset(); device_.Reset(); context_.Reset();
        hr=D3D11CreateDeviceAndSwapChain(chosen.Get(),driver,nullptr,0,levels+1,1,D3D11_SDK_VERSION,
            &desc,swap_.GetAddressOf(),device_.GetAddressOf(),&selected,context_.GetAddressOf());
    }
    if(FAILED(hr)&&chosen){
        swap_.Reset();device_.Reset();context_.Reset();chosen.Reset();
        hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels+1,1,D3D11_SDK_VERSION,&desc,swap_.GetAddressOf(),device_.GetAddressOf(),&selected,context_.GetAddressOf());
    }
    if(FAILED(hr)) { Shutdown(); return Fail(hr,"D3D11 device/swapchain creation"); }
    if(swapFlags_&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT){ComPtr<IDXGISwapChain2> waitable;
        if(SUCCEEDED(swap_.As(&waitable))&&SUCCEEDED(waitable->SetMaximumFrameLatency(2)))frameWait_=waitable->GetFrameLatencyWaitableObject();}
    else{ComPtr<IDXGIDevice1> latency;if(SUCCEEDED(device_.As(&latency)))latency->SetMaximumFrameLatency(2);}
    GenZHLoad::Record("present_v101",std::string("flip discard, buffers ")+std::to_string(desc.BufferCount)+(tearing_?", tearing":", no tearing")+(frameWait_?", waitable latency 2":", device latency 2"),0);
    ComPtr<IDXGIFactory> factory;
    hr=swap_->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if(SUCCEEDED(hr)) hr=factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
    if(FAILED(hr)) { Shutdown(); return Fail(hr,"DXGI window association"); }
    if(FAILED(hr=CreatePipeline()) || FAILED(hr=CreateShadowTarget()) || FAILED(hr=CreateTargets(width,height))) {
        Shutdown(); return hr;
    }
    {
        GenZHLoad::Scope timing("environment_upload","HDR GGX cube + irradiance SH + BRDF LUT");
        const HRESULT environment=environment_.LoadProfile(device_.Get());timing.hit=environment==S_OK;
        if(environment!=S_OK)GenZHLoad::Record("environment_fallback",environment==S_FALSE?"missing/disabled precomputed environment":"GPU environment allocation failed",0);
    }
    return S_OK;
}

void RenderDevice::Shutdown() {
    if(context_) { context_->ClearState(); context_->Flush(); }
    environment_.Reset();
    white_.view.Reset(); flatNormal_.view.Reset();gammaView_.Reset();
    backBuffer_.Reset(); hdrTarget_.Reset(); hdrView_.Reset(); sceneDepth_.Reset();screenMaterials_.Reset();screenMaterialAttempted_=false;screenAO_.Reset();screenAOAttempted_=false;tonemapTarget_.Reset();tonemapView_.Reset();
    temporal_.Reset();temporalAttempted_=false;temporalResolved_=false;bloom_.Reset();bloomAttempted_=false;bloomReady_=false;
    shadowDepth_.Reset(); shadowView_.Reset();
    meshVS_.Reset(); presentVS_.Reset(); meshPS_.Reset(); shadowPS_.Reset(); tonemapPS_.Reset();aaPS_.Reset();giPS_.Reset();giComposePS_.Reset();giTarget_.Reset();giView_.Reset();giTexture_.Reset();giConstants_.Reset();giAdditive_.Reset();giWidth_=giHeight_=0;
    layout_.Reset(); meshConstants_.Reset(); frameConstants_.Reset();postConstants_.Reset();
    materialSampler_.Reset(); shadowSampler_.Reset(); presentSampler_.Reset();
    raster_.Reset(); presentRaster_.Reset(); depthState_.Reset(); noDepth_.Reset(); opaqueBlend_.Reset();
    if(frameWait_){CloseHandle(frameWait_);frameWait_=nullptr;}
    swap_.Reset(); context_.Reset(); device_.Reset();swapFlags_=0;tearing_=false;
    width_=height_=0; frameSet_=shadowRendered_=false; pass_=Pass::None;
}

HRESULT RenderDevice::CreatePipeline() {
    ComPtr<ID3DBlob> meshVS,meshPS,shadowPS,presentVS,tonemapPS,aaPS,giPS,giComposePS;
    auto compile=[&](const char *entry,const char *profile,ComPtr<ID3DBlob> &result)->HRESULT {
        bool hit=false;std::string error;
        HRESULT hr=CompileCachedShader(std::string(ShaderSource,sizeof(ShaderSource)-1),entry,profile,result,error,hit);
        if(FAILED(hr)) {
            Fail(hr,entry);
            error_.append("\n").append(error);
        }
        return hr;
    };
    HRESULT hr;
    if(FAILED(hr=compile("VSMain","vs_5_0",meshVS)) || FAILED(hr=compile("PSMain","ps_5_0",meshPS)) ||
       FAILED(hr=compile("PSShadow","ps_5_0",shadowPS)) || FAILED(hr=compile("VSPresent","vs_5_0",presentVS)) ||
       FAILED(hr=compile("PSTonemap","ps_5_0",tonemapPS))||FAILED(hr=compile("PSSceneAA","ps_5_0",aaPS))||FAILED(hr=compile("PSGlobalIllumination","ps_5_0",giPS))||FAILED(hr=compile("PSGlobalCompose","ps_5_0",giComposePS))) return hr;
    if(FAILED(hr=device_->CreateVertexShader(meshVS->GetBufferPointer(),meshVS->GetBufferSize(),nullptr,meshVS_.GetAddressOf())) ||
       FAILED(hr=device_->CreateVertexShader(presentVS->GetBufferPointer(),presentVS->GetBufferSize(),nullptr,presentVS_.GetAddressOf())) ||
       FAILED(hr=device_->CreatePixelShader(meshPS->GetBufferPointer(),meshPS->GetBufferSize(),nullptr,meshPS_.GetAddressOf())) ||
       FAILED(hr=device_->CreatePixelShader(shadowPS->GetBufferPointer(),shadowPS->GetBufferSize(),nullptr,shadowPS_.GetAddressOf())) ||
       FAILED(hr=device_->CreatePixelShader(tonemapPS->GetBufferPointer(),tonemapPS->GetBufferSize(),nullptr,tonemapPS_.GetAddressOf()))||
       FAILED(hr=device_->CreatePixelShader(aaPS->GetBufferPointer(),aaPS->GetBufferSize(),nullptr,aaPS_.GetAddressOf()))||
       FAILED(hr=device_->CreatePixelShader(giPS->GetBufferPointer(),giPS->GetBufferSize(),nullptr,giPS_.GetAddressOf()))||
       FAILED(hr=device_->CreatePixelShader(giComposePS->GetBufferPointer(),giComposePS->GetBufferSize(),nullptr,giComposePS_.GetAddressOf())))
        return Fail(hr,"Shader object creation");
    const D3D11_INPUT_ELEMENT_DESC elements[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if(FAILED(hr=device_->CreateInputLayout(elements,3,meshVS->GetBufferPointer(),meshVS->GetBufferSize(),layout_.GetAddressOf())) ||
       FAILED(hr=MakeConstants(device_.Get(),sizeof(MeshConstants),meshConstants_.GetAddressOf())) ||
       FAILED(hr=MakeConstants(device_.Get(),sizeof(FrameConstants),frameConstants_.GetAddressOf()))||
       FAILED(hr=MakeConstants(device_.Get(),sizeof(postSettings_),postConstants_.GetAddressOf())))
        return Fail(hr,"Input layout/constant buffers");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter=D3D11_FILTER_ANISOTROPIC; sampler.MaxAnisotropy=8;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.ComparisonFunc=D3D11_COMPARISON_NEVER; sampler.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(hr=device_->CreateSamplerState(&sampler,materialSampler_.GetAddressOf()))) return Fail(hr,"Material sampler");
    sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    if(FAILED(hr=device_->CreateSamplerState(&sampler,presentSampler_.GetAddressOf()))) return Fail(hr,"Present sampler");
    sampler.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;
    std::fill_n(sampler.BorderColor,4,1.0f);
    if(FAILED(hr=device_->CreateSamplerState(&sampler,shadowSampler_.GetAddressOf()))) return Fail(hr,"Shadow sampler");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_BACK;
    raster.FrontCounterClockwise=TRUE; raster.DepthClipEnable=TRUE;
    if(FAILED(hr=device_->CreateRasterizerState(&raster,raster_.GetAddressOf()))) return Fail(hr,"Mesh rasterizer");
    raster.CullMode=D3D11_CULL_NONE;
    if(FAILED(hr=device_->CreateRasterizerState(&raster,presentRaster_.GetAddressOf()))) return Fail(hr,"Present rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable=TRUE; depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    if(FAILED(hr=device_->CreateDepthStencilState(&depth,depthState_.GetAddressOf()))) return Fail(hr,"Depth state");
    depth.DepthEnable=FALSE; depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    if(FAILED(hr=device_->CreateDepthStencilState(&depth,noDepth_.GetAddressOf()))) return Fail(hr,"Present depth state");
    D3D11_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    if(FAILED(hr=device_->CreateBlendState(&blend,opaqueBlend_.GetAddressOf()))) return Fail(hr,"Opaque blend state");
    const std::uint8_t white[]={255,255,255,255},flat[]={128,128,255,255};
    if(FAILED(hr=CreateTextureRGBA(white,1,1,4,false,white_)) ||
       FAILED(hr=CreateTextureRGBA(flat,1,1,4,false,flatNormal_))) return hr;
    std::uint16_t identity[256];for(UINT i=0;i<256;++i)identity[i]=static_cast<std::uint16_t>(i*257);
    return SetGamma(identity,identity,identity);
}

HRESULT RenderDevice::SetGamma(const std::uint16_t *red,const std::uint16_t *green,const std::uint16_t *blue){
    if(!device_||!red||!green||!blue)return E_INVALIDARG;
    std::uint16_t pixels[256][4];for(UINT i=0;i<256;++i){pixels[i][0]=red[i];pixels[i][1]=green[i];pixels[i][2]=blue[i];pixels[i][3]=65535;}
    D3D11_TEXTURE1D_DESC desc{};desc.Width=256;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R16G16B16A16_UNORM;
    desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=pixels;
    ComPtr<ID3D11Texture1D> texture;HRESULT hr=device_->CreateTexture1D(&desc,&initial,texture.GetAddressOf());if(FAILED(hr))return Fail(hr,"gamma lookup texture");
    hr=device_->CreateShaderResourceView(texture.Get(),nullptr,gammaView_.ReleaseAndGetAddressOf());return FAILED(hr)?Fail(hr,"gamma lookup view"):hr;
}

HRESULT RenderDevice::CreateTargets(UINT width,UINT height) {
    if(width==0 || height==0 || width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return Fail(E_INVALIDARG,"Render target dimensions");
    HRESULT hr; ComPtr<ID3D11Texture2D> back,hdr;
    ComPtr<ID3D11RenderTargetView> backRTV,hdrRTV;
    ComPtr<ID3D11ShaderResourceView> hdrSRV;
    D3D11_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    if(FAILED(hr=swap_->GetBuffer(0,IID_PPV_ARGS(back.GetAddressOf()))) ||
       FAILED(hr=device_->CreateRenderTargetView(back.Get(),&rtv,backRTV.GetAddressOf()))) return Fail(hr,"Swapchain SRGB target");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
    desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(hr=device_->CreateTexture2D(&desc,nullptr,hdr.GetAddressOf())) ||
       FAILED(hr=device_->CreateRenderTargetView(hdr.Get(),nullptr,hdrRTV.GetAddressOf())) ||
       FAILED(hr=device_->CreateShaderResourceView(hdr.Get(),nullptr,hdrSRV.GetAddressOf()))) return Fail(hr,"Linear HDR target");
    if(FAILED(hr=sceneDepth_.Create(device_.Get(),width,height)))return Fail(hr,"Readable scene depth target");
    backBuffer_=backRTV; hdrTarget_=hdrRTV; hdrView_=hdrSRV; hdrTexture_=hdr;
    desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tonemap;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    if(FAILED(hr=device_->CreateTexture2D(&desc,nullptr,tonemap.GetAddressOf()))||
       FAILED(hr=device_->CreateRenderTargetView(tonemap.Get(),&rtv,tonemapTarget_.ReleaseAndGetAddressOf()))||
       FAILED(hr=device_->CreateShaderResourceView(tonemap.Get(),&srv,tonemapView_.ReleaseAndGetAddressOf())))return Fail(hr,"Tone-map AA target");
    postSettings_[0]=1.0f/static_cast<float>(width);postSettings_[1]=1.0f/static_cast<float>(height);
    width_=width; height_=height; return S_OK;
}

HRESULT RenderDevice::CreateShadowTarget() {
    ComPtr<ID3D11Texture2D> texture; D3D11_TEXTURE2D_DESC desc{};
    desc.Width=desc.Height=ShadowSize; desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R32_TYPELESS; desc.SampleDesc.Count=1;
    desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format=DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format=DXGI_FORMAT_R32_FLOAT; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
    HRESULT hr;
    if(FAILED(hr=device_->CreateTexture2D(&desc,nullptr,texture.GetAddressOf())) ||
       FAILED(hr=device_->CreateDepthStencilView(texture.Get(),&dsv,shadowDepth_.GetAddressOf())) ||
       FAILED(hr=device_->CreateShaderResourceView(texture.Get(),&srv,shadowView_.GetAddressOf()))) return Fail(hr,"Shadow target");
    return S_OK;
}

HRESULT RenderDevice::Resize(UINT width,UINT height) {
    if(!swap_ || !width || !height || pass_!=Pass::None) return Fail(E_INVALIDARG,"Resize state/dimensions");
    if(width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return Fail(E_INVALIDARG,"Resize limit");
    if(width==width_ && height==height_) return S_OK;
    context_->ClearState(); backBuffer_.Reset(); hdrTarget_.Reset(); hdrView_.Reset(); sceneDepth_.Reset();screenMaterials_.Reset();screenMaterialAttempted_=false;screenAO_.Reset();screenAOAttempted_=false;
    width_=height_=0;
    HRESULT hr=swap_->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,swapFlags_);
    return FAILED(hr)?Fail(hr,"ResizeBuffers"):CreateTargets(width,height);
}

HRESULT RenderDevice::CreateMesh(const Vertex *vertices,UINT vertexCount,
                                const std::uint32_t *indices,UINT indexCount,Mesh &result) {
    if(!device_ || !vertices || !indices || !vertexCount || !indexCount || indexCount%3 ||
       vertexCount>std::numeric_limits<UINT>::max()/sizeof(Vertex) || indexCount>std::numeric_limits<UINT>::max()/sizeof(std::uint32_t))
        return Fail(E_INVALIDARG,"Mesh arguments");
    for(UINT i=0;i<vertexCount;++i) {
        if(!Finite(vertices[i].position,3) || !Finite(vertices[i].normal,3) || !Finite(vertices[i].uv,2))
            return Fail(E_INVALIDARG,"Nonfinite mesh vertex");
    }
    for(UINT i=0;i<indexCount;++i) if(indices[i]>=vertexCount) return Fail(E_INVALIDARG,"Mesh index bounds");
    Mesh created; D3D11_BUFFER_DESC desc{}; D3D11_SUBRESOURCE_DATA data{};
    desc.Usage=D3D11_USAGE_IMMUTABLE; desc.ByteWidth=vertexCount*static_cast<UINT>(sizeof(Vertex));
    desc.BindFlags=D3D11_BIND_VERTEX_BUFFER; data.pSysMem=vertices;
    HRESULT hr=device_->CreateBuffer(&desc,&data,created.vertices.GetAddressOf());
    if(FAILED(hr)) return Fail(hr,"Vertex buffer");
    desc.ByteWidth=indexCount*static_cast<UINT>(sizeof(std::uint32_t)); desc.BindFlags=D3D11_BIND_INDEX_BUFFER; data.pSysMem=indices;
    if(FAILED(hr=device_->CreateBuffer(&desc,&data,created.indices.GetAddressOf()))) return Fail(hr,"Index buffer");
    created.indexCount=indexCount; created.owner=device_.Get(); result=created; return S_OK;
}

HRESULT RenderDevice::CreateTextureRGBA(const void *pixels,UINT width,UINT height,UINT rowPitch,bool srgb,Texture &result) {
    if(!device_ || !pixels || !width || !height || width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || rowPitch<width*4) return Fail(E_INVALIDARG,"Texture arguments");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height; desc.ArraySize=1; desc.SampleDesc.Count=1;
    desc.Format=srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
    UINT support=0; HRESULT hr=device_->CheckFormatSupport(desc.Format,&support);
    if(FAILED(hr)) return Fail(hr,"Texture format support query");
    constexpr UINT required=D3D11_FORMAT_SUPPORT_MIP_AUTOGEN|D3D11_FORMAT_SUPPORT_SHADER_SAMPLE|D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if((support&required)!=required) return Fail(DXGI_ERROR_UNSUPPORTED,"Texture mip support");
    ComPtr<ID3D11Texture2D> texture; Texture created;
    if(FAILED(hr=device_->CreateTexture2D(&desc,nullptr,texture.GetAddressOf())) ||
       FAILED(hr=device_->CreateShaderResourceView(texture.Get(),nullptr,created.view.GetAddressOf()))) return Fail(hr,"Texture/SRV creation");
    context_->UpdateSubresource(texture.Get(),0,nullptr,pixels,rowPitch,0);
    context_->GenerateMips(created.view.Get()); created.owner=device_.Get(); result=created; return S_OK;
}

HRESULT RenderDevice::Upload(ID3D11Buffer *buffer,const void *data,UINT bytes) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr=context_->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if(FAILED(hr)) return Fail(hr,"Constant buffer map");
    std::memcpy(mapped.pData,data,bytes); context_->Unmap(buffer,0); return S_OK;
}

HRESULT RenderDevice::SetFrame(const FrameConstants &constants) {
    if(!device_ || pass_!=Pass::None ||
       !Finite(constants.cameraExposure,4) || !Finite(constants.sunDirectionIntensity,4) ||
       !Finite(constants.sunColor,4) || !Finite(constants.ambient,4) ||
       !Finite(constants.shadowSettings,4) || !Finite(constants.lightViewProjection,16) ||
       constants.cameraExposure[3]<=0 || constants.sunDirectionIntensity[3]<0)
        return Fail(E_INVALIDARG,"Frame constants/state");
    frame_=constants;
    frame_.shadowSettings[1]=frame_.shadowSettings[2]=1.0f/ShadowSize;
    frameSet_=true; shadowRendered_=false; return S_OK;
}

void RenderDevice::BindMeshPipeline() {
    BindEnvironment();
    context_->IASetInputLayout(layout_.Get()); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(meshVS_.Get(),nullptr,0); context_->GSSetShader(nullptr,nullptr,0);
    context_->HSSetShader(nullptr,nullptr,0); context_->DSSetShader(nullptr,nullptr,0);
    ID3D11Buffer *buffers[]={meshConstants_.Get(),frameConstants_.Get()};
    context_->VSSetConstantBuffers(0,2,buffers); context_->PSSetConstantBuffers(0,2,buffers);
    ID3D11SamplerState *samplers[]={materialSampler_.Get(),shadowSampler_.Get()};
    context_->PSSetSamplers(0,2,samplers);
    context_->RSSetState(raster_.Get()); context_->OMSetDepthStencilState(depthState_.Get(),0);
    context_->OMSetBlendState(opaqueBlend_.Get(),nullptr,0xffffffff);
}

HRESULT RenderDevice::BeginShadowPass() {
    if(!device_ || !frameSet_ || pass_!=Pass::None) return Fail(E_UNEXPECTED,"BeginShadowPass state");
    ID3D11ShaderResourceView *empty=nullptr; context_->PSSetShaderResources(3,1,&empty);
    context_->OMSetRenderTargets(0,nullptr,shadowDepth_.Get());
    context_->ClearDepthStencilView(shadowDepth_.Get(),D3D11_CLEAR_DEPTH,1,0);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(ShadowSize),static_cast<float>(ShadowSize),0,1};
    context_->RSSetViewports(1,&viewport); BindMeshPipeline(); context_->PSSetShader(shadowPS_.Get(),nullptr,0);
    HRESULT hr=Upload(frameConstants_.Get(),&frame_,sizeof(frame_));
    if(FAILED(hr)) return hr;
    pass_=Pass::Shadow; shadowRendered_=true; return S_OK;
}

HRESULT RenderDevice::BeginFrame(const float clearLinearRGBA[4]) {
    if(!device_ || !frameSet_ || !hdrTarget_ || pass_==Pass::Opaque || !clearLinearRGBA || !Finite(clearLinearRGBA,4))
        return Fail(E_UNEXPECTED,"BeginFrame state/color");
    SceneDepthGPU::Unbind(context_.Get());sceneDepth_.Invalidate();ScreenMaterialGPU::UnbindInputs(context_.Get());screenMaterials_.BeginFrame();screenAO_.BeginFrame();waterCopyValid_=false;
    ID3D11Buffer *emptyCapture=nullptr;context_->PSSetConstantBuffers(13,1,&emptyCapture);
    ID3D11RenderTargetView *target=hdrTarget_.Get();
    context_->OMSetRenderTargets(1,&target,sceneDepth_.Writable());
    context_->ClearRenderTargetView(hdrTarget_.Get(),clearLinearRGBA);
    context_->ClearDepthStencilView(sceneDepth_.Writable(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};
    context_->RSSetViewports(1,&viewport); BindMeshPipeline(); context_->PSSetShader(meshPS_.Get(),nullptr,0);
    FrameConstants visible=frame_;
    if(!shadowRendered_) visible.shadowSettings[3]=0;
    HRESULT hr=Upload(frameConstants_.Get(),&visible,sizeof(visible));
    if(FAILED(hr)) return hr;
    ID3D11ShaderResourceView *shadow=shadowView_.Get(); context_->PSSetShaderResources(3,1,&shadow);
    pass_=Pass::Opaque; return S_OK;
}

HRESULT RenderDevice::DrawMesh(const Mesh &mesh,const MeshConstants &constants,const MaterialTextures &textures) {
    if(!device_ || pass_==Pass::None || !mesh.vertices || !mesh.indices || !mesh.indexCount || mesh.owner!=device_.Get() ||
       !Finite(constants.world,16) || !Finite(constants.viewProjection,16) || !Finite(constants.normalWorld,16) ||
       !Finite(constants.baseColor,4) || !Finite(constants.surface,4) || !Finite(constants.emissive,4))
        return Fail(E_INVALIDARG,"DrawMesh resources/constants/state");
    const Texture *inputs[]={textures.albedo,textures.normal,textures.orm};
    for(const Texture *texture:inputs)
        if(texture && texture->view && texture->owner!=device_.Get())
            return Fail(E_INVALIDARG,"DrawMesh texture belongs to another device");
    MeshConstants data=constants;
    if(pass_==Pass::Shadow) std::copy_n(frame_.lightViewProjection,16,data.viewProjection);
    HRESULT hr=Upload(meshConstants_.Get(),&data,sizeof(data)); if(FAILED(hr)) return hr;
    UINT stride=sizeof(Vertex),offset=0; ID3D11Buffer *vertices=mesh.vertices.Get();
    context_->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    context_->IASetIndexBuffer(mesh.indices.Get(),DXGI_FORMAT_R32_UINT,0);
    ID3D11ShaderResourceView *views[]={
        textures.albedo&&textures.albedo->view?textures.albedo->view.Get():white_.view.Get(),
        textures.normal&&textures.normal->view?textures.normal->view.Get():flatNormal_.view.Get(),
        textures.orm&&textures.orm->view?textures.orm->view.Get():white_.view.Get()};
    context_->PSSetShaderResources(0,3,views); context_->DrawIndexed(mesh.indexCount,0,0); return S_OK;
}

HRESULT RenderDevice::EndFrame(bool vsync) {
    HRESULT hr=ToneMapFrame();
    return FAILED(hr)?hr:PresentFrame(vsync);
}

HRESULT RenderDevice::ToneMapFrame() {
    if(!device_ || pass_!=Pass::Opaque) return Fail(E_UNEXPECTED,"EndFrame state");
    if(lutDirty_)BuildColourLut();
    postSettings_[4]=bloomReady_?bloom_.CompositeScale():0.0f;
    HRESULT hr=Upload(postConstants_.Get(),postSettings_,sizeof(postSettings_));if(FAILED(hr))return hr;
    ID3D11Buffer *post=postConstants_.Get();context_->PSSetConstantBuffers(2,1,&post);
    // Compatibility draws use b1 for legacy VS registers. Restore the frame data
    // explicitly before presentation instead of relying on BeginFrame state.
    hr=Upload(frameConstants_.Get(),&frame_,sizeof(frame_));if(FAILED(hr))return hr;
    ID3D11Buffer *frame=frameConstants_.Get();context_->PSSetConstantBuffers(1,1,&frame);
    // taa100: a temporally resolved frame needs no spatial edge AA (it would blur it).
    const bool edgeAA=postSettings_[2]>.5f&&!temporalResolved_;temporalResolved_=false;
    ID3D11RenderTargetView *target=edgeAA?tonemapTarget_.Get():backBuffer_.Get(); context_->OMSetRenderTargets(1,&target,nullptr);
    context_->OMSetDepthStencilState(noDepth_.Get(),0); context_->RSSetState(presentRaster_.Get());
    context_->IASetInputLayout(nullptr); context_->VSSetShader(presentVS_.Get(),nullptr,0);
    context_->PSSetShader(tonemapPS_.Get(),nullptr,0);
    context_->GSSetShader(nullptr,nullptr,0);
    context_->HSSetShader(nullptr,nullptr,0);
    context_->DSSetShader(nullptr,nullptr,0);
    context_->OMSetBlendState(opaqueBlend_.Get(),nullptr,0xffffffff);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};
    context_->RSSetViewports(1,&viewport);
    ID3D11ShaderResourceView *hdr=hdrView_.Get(); context_->PSSetShaderResources(0,1,&hdr);
    ID3D11ShaderResourceView *gamma=gammaView_.Get();context_->PSSetShaderResources(1,1,&gamma);
    ID3D11ShaderResourceView *glow=bloomReady_?bloom_.Result():nullptr;context_->PSSetShaderResources(2,1,&glow);
    ID3D11ShaderResourceView *lut=postSettings_[11]>.5f?lutView_.Get():nullptr;context_->PSSetShaderResources(3,1,&lut);
    ID3D11SamplerState *sampler=presentSampler_.Get(); context_->PSSetSamplers(0,1,&sampler);
    context_->Draw(3,0);
    if(edgeAA){
        target=backBuffer_.Get();context_->OMSetRenderTargets(1,&target,nullptr);
        ID3D11ShaderResourceView *scene=tonemapView_.Get();context_->PSSetShaderResources(0,1,&scene);
        context_->PSSetShader(aaPS_.Get(),nullptr,0);context_->Draw(3,0);
    }
    ID3D11ShaderResourceView *empty[4]={}; context_->PSSetShaderResources(0,4,empty);
    pass_=Pass::None; frameSet_=false; shadowRendered_=false;bloomReady_=false;
    return S_OK;
}

HRESULT RenderDevice::ApplyScreenAO(){
    if(!context_||pass_!=Pass::Opaque)return E_UNEXPECTED;
    if((!screenAOEnabled_&&!screenContactEnabled_)||!screenMaterials_.CameraReady())return S_FALSE;
    GTAOGPU::Constants camera;if(!GTAOGPU::PrepareConstants(screenMaterials_.camera,width_,height_,8,1,camera))return S_FALSE;
    if(!screenAO_.Available()){
        if(screenAOAttempted_)return S_FALSE;screenAOAttempted_=true;
        const HRESULT created=screenAO_.Create(device_.Get(),width_,height_,8,screenAOEnabled_?1.f:0.f,screenContactEnabled_?1.f:0.f);
        if(FAILED(created)){GenZHLoad::Record("screen_AO_fallback","GTAO allocation/shader preparation failed; native material retained",0);return S_FALSE;}
    }
    const HRESULT result=screenAO_.Apply(context_.Get(),screenMaterials_,sceneDepth_.DepthView(),hdrView_.Get());
    if(result==S_OK){screenAO_.SwapColor(hdrTarget_,hdrView_);ID3D11RenderTargetView *target=hdrTarget_.Get();context_->OMSetRenderTargets(1,&target,sceneDepth_.Writable());}
    return result;
}

HRESULT RenderDevice::ApplyBloom(){
    bloomReady_=false;
    if(bloomMode_!=2||!context_||pass_!=Pass::Opaque||!hdrView_)return S_FALSE;
    if(!bloom_.Ready()||bloom_.Width()!=width_||bloom_.Height()!=height_){
        if(bloomAttempted_&&!bloom_.Ready())return S_FALSE;bloomAttempted_=true;
        const HRESULT created=bloom_.Create(device_.Get(),width_,height_);
        if(FAILED(created)){GenZHLoad::Record("bloom_v106","bloom unavailable, former bloom kept: "+bloom_.Stats().lastError,0);SetBloomMode(1);return S_FALSE;}
        GenZHLoad::Record("bloom_v106","mip-chain bloom "+std::to_string(bloom_.Stats().levels)+" levels",0);
    }
    const HRESULT result=bloom_.Apply(context_.Get(),hdrView_.Get(),frame_.cameraExposure[3]);
    // The HUD continues into the world target at full size.
    ID3D11RenderTargetView *target=hdrTarget_.Get();context_->OMSetRenderTargets(1,&target,sceneDepth_.Writable());
    const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};context_->RSSetViewports(1,&viewport);
    bloomReady_=SUCCEEDED(result);return result;
}

HRESULT RenderDevice::CopySceneForWater(){
    // Once per frame, before the first water draw: the scene as drawn so far.
    if(waterCopyValid_)return S_OK;
    if(!context_||!hdrTexture_)return E_UNEXPECTED;
    D3D11_TEXTURE2D_DESC desc{};hdrTexture_->GetDesc(&desc);
    if(!waterCopy_||waterCopyWidth_!=desc.Width||waterCopyHeight_!=desc.Height){
        waterCopyView_.Reset();waterCopy_.Reset();
        D3D11_TEXTURE2D_DESC copy=desc;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;copy.Usage=D3D11_USAGE_DEFAULT;copy.CPUAccessFlags=0;copy.MiscFlags=0;
        HRESULT hr=device_->CreateTexture2D(&copy,nullptr,waterCopy_.GetAddressOf());
        if(SUCCEEDED(hr))hr=device_->CreateShaderResourceView(waterCopy_.Get(),nullptr,waterCopyView_.GetAddressOf());
        if(FAILED(hr)){waterCopy_.Reset();waterCopyView_.Reset();waterCopyWidth_=waterCopyHeight_=0;return Fail(hr,"Water scene copy");}
        waterCopyWidth_=desc.Width;waterCopyHeight_=desc.Height;
    }
    context_->CopyResource(waterCopy_.Get(),hdrTexture_.Get());
    waterCopyValid_=true;
    return S_OK;
}
HRESULT RenderDevice::ApplyGlobalIllumination(){
    if(!giEnabled_||!context_||pass_!=Pass::Opaque||!hdrView_||!hdrTarget_||!giPS_||!giComposePS_||!screenMaterials_.CameraReady())return S_FALSE;
    ID3D11ShaderResourceView *normal=screenMaterials_.View(ScreenMaterialGPU::NormalOcclusion),*albedo=screenMaterials_.View(ScreenMaterialGPU::Albedo),*depth=sceneDepth_.DepthView();
    if(!normal||!albedo||!depth)return S_FALSE;
    HRESULT hr=S_OK;
    const UINT halfWidth=std::max<UINT>((width_+1)/2,1),halfHeight=std::max<UINT>((height_+1)/2,1);
    if(!giTarget_||giWidth_!=halfWidth||giHeight_!=halfHeight){
        giTarget_.Reset();giView_.Reset();giTexture_.Reset();
        D3D11_TEXTURE2D_DESC desc{};desc.Width=halfWidth;desc.Height=halfHeight;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R11G11B10_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(hr=device_->CreateTexture2D(&desc,nullptr,giTexture_.GetAddressOf()))||
           FAILED(hr=device_->CreateRenderTargetView(giTexture_.Get(),nullptr,giTarget_.GetAddressOf()))||
           FAILED(hr=device_->CreateShaderResourceView(giTexture_.Get(),nullptr,giView_.GetAddressOf()))){
            giTarget_.Reset();giView_.Reset();giTexture_.Reset();giWidth_=giHeight_=0;return Fail(hr,"Indirect light target");
        }
        giWidth_=halfWidth;giHeight_=halfHeight;
    }
    if(!giConstants_&&FAILED(hr=MakeConstants(device_.Get(),sizeof(GlobalIlluminationConstants),giConstants_.GetAddressOf())))return Fail(hr,"Indirect light constants");
    if(!giAdditive_){
        // Additive over the lit colour; alpha is left as it was.
        D3D11_BLEND_DESC blend{};auto &target=blend.RenderTarget[0];
        target.BlendEnable=TRUE;target.SrcBlend=D3D11_BLEND_ONE;target.DestBlend=D3D11_BLEND_ONE;target.BlendOp=D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha=D3D11_BLEND_ZERO;target.DestBlendAlpha=D3D11_BLEND_ONE;target.BlendOpAlpha=D3D11_BLEND_OP_ADD;target.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(hr=device_->CreateBlendState(&blend,giAdditive_.GetAddressOf())))return Fail(hr,"Indirect light blend");
    }
    // The primary camera (unjittered) and its raw depth range; strength and radius in world units.
    const auto &camera=screenMaterials_.camera;GlobalIlluminationConstants k{};
    DirectX::XMFLOAT4X4 view,projection,viewProjection;std::memcpy(&view,camera.viewMatrix,64);std::memcpy(&projection,camera.projectionMatrix,64);
    DirectX::XMStoreFloat4x4(&viewProjection,DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&view),DirectX::XMLoadFloat4x4(&projection)));
    std::memcpy(k.viewProjection,&viewProjection,64);std::memcpy(k.inverseViewProjection,camera.inverseVP,64);std::memcpy(k.eye,camera.eye,16);
    k.depthRange[0]=camera.depthRange[0];k.depthRange[1]=camera.depthRange[1];k.depthRange[2]=giStrength_;k.depthRange[3]=giRadius_;
    k.frame[0]=float(giFrame_++);k.frame[1]=float(width_);k.frame[2]=float(height_);
    context_->UpdateSubresource(giConstants_.Get(),0,nullptr,&k,0,0);
    ID3D11Buffer *constants=giConstants_.Get();
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(presentVS_.Get(),nullptr,0);context_->HSSetShader(nullptr,nullptr,0);context_->DSSetShader(nullptr,nullptr,0);context_->GSSetShader(nullptr,nullptr,0);
    context_->RSSetState(presentRaster_.Get());context_->OMSetDepthStencilState(noDepth_.Get(),0);
    context_->PSSetConstantBuffers(13,1,&constants);
    ID3D11SamplerState *sampler=presentSampler_.Get();context_->PSSetSamplers(0,1,&sampler);
    ID3D11ShaderResourceView *none[5]{};
    // Pass 1: the radiance seen behind each half-resolution pixel's rays (the lit HDR is only read).
    ID3D11RenderTargetView *target=giTarget_.Get();context_->OMSetRenderTargets(1,&target,nullptr);
    context_->OMSetBlendState(opaqueBlend_.Get(),nullptr,0xffffffff);
    const D3D11_VIEWPORT half{0,0,float(halfWidth),float(halfHeight),0,1};context_->RSSetViewports(1,&half);
    ID3D11ShaderResourceView *inputs[3]={depth,normal,albedo};context_->PSSetShaderResources(24,3,inputs);
    ID3D11ShaderResourceView *lit=hdrView_.Get();context_->PSSetShaderResources(28,1,&lit);
    context_->PSSetShader(giPS_.Get(),nullptr,0);
    context_->Draw(3,0);
    context_->PSSetShaderResources(24,5,none);
    // Pass 2: the indirect light is added to the lit scene (before the temporal resolve smooths it).
    target=hdrTarget_.Get();context_->OMSetRenderTargets(1,&target,nullptr);
    context_->OMSetBlendState(giAdditive_.Get(),nullptr,0xffffffff);
    const D3D11_VIEWPORT full{0,0,float(width_),float(height_),0,1};context_->RSSetViewports(1,&full);
    ID3D11ShaderResourceView *radiance=giView_.Get();context_->PSSetShaderResources(27,1,&radiance);
    context_->PSSetShader(giComposePS_.Get(),nullptr,0);
    context_->Draw(3,0);
    context_->PSSetShaderResources(27,1,none);
    context_->OMSetBlendState(opaqueBlend_.Get(),nullptr,0xffffffff);
    return S_OK;
}
HRESULT RenderDevice::BuildColourLut(){
    // The grade of the tone map as a 32^3 colour LUT (linear in and out): saturation about luma, highlight
    // warmth (from the luma before saturation), contrast about 18% grey, clamped to the display range.
    lutDirty_=false;
    const float saturation=postSettings_[8],warmth=postSettings_[9],contrast=postSettings_[10];
    if(saturation==1.0f&&warmth==0.0f&&contrast==1.0f){postSettings_[11]=0;lutView_.Reset();lutTexture_.Reset();return S_OK;}
    const UINT n=32;std::vector<float> texels(std::size_t(n)*n*n*4);
    for(UINT b=0;b<n;++b)for(UINT g=0;g<n;++g)for(UINT r=0;r<n;++r){
        float c[3]={float(r)/float(n-1),float(g)/float(n-1),float(b)/float(n-1)};
        GradeColour(c,saturation,warmth,contrast);
        float *texel=&texels[(std::size_t(b)*n*n+std::size_t(g)*n+r)*4];
        texel[0]=c[0];texel[1]=c[1];texel[2]=c[2];texel[3]=1;
    }
    D3D11_TEXTURE3D_DESC desc{};desc.Width=desc.Height=desc.Depth=n;desc.MipLevels=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{texels.data(),n*16,n*n*16};
    ComPtr<ID3D11Texture3D> texture;HRESULT hr=device_->CreateTexture3D(&desc,&init,texture.GetAddressOf());
    if(SUCCEEDED(hr))hr=device_->CreateShaderResourceView(texture.Get(),nullptr,lutView_.ReleaseAndGetAddressOf());
    if(FAILED(hr)){lutView_.Reset();lutTexture_.Reset();postSettings_[11]=0;return Fail(hr,"Colour LUT");}
    lutTexture_=texture;postSettings_[11]=1;
    return S_OK;
}
HRESULT RenderDevice::ApplyTemporalAA(const TemporalAAFrame &frame){
    if(!context_||pass_!=Pass::Opaque||!hdrView_)return E_UNEXPECTED;
    if(!temporal_.Ready()||temporal_.Width()!=width_||temporal_.Height()!=height_){
        if(temporalAttempted_&&!temporal_.Ready())return S_FALSE;temporalAttempted_=true;
        const HRESULT created=temporal_.Create(device_.Get(),width_,height_);
        if(FAILED(created)){GenZHLoad::Record("temporal_aa_v100","TAA unavailable, edge AA kept: "+temporal_.Stats().lastError,0);return S_FALSE;}
    }
    ComPtr<ID3D11Resource> color;hdrView_->GetResource(color.GetAddressOf());
    const HRESULT result=temporal_.Resolve(context_.Get(),color.Get(),hdrView_.Get(),sceneDepth_.DepthView(),frame,hdrTarget_.Get(),screenMaterials_.View(ScreenMaterialGPU::MotionVector));
    ID3D11RenderTargetView *target=hdrTarget_.Get();context_->OMSetRenderTargets(1,&target,sceneDepth_.Writable());
    if(SUCCEEDED(result))temporalResolved_=true;
    return result;
}

HRESULT RenderDevice::PresentFrame(bool vsync) {
    if(!swap_ || pass_!=Pass::None) return Fail(E_UNEXPECTED,"PresentFrame state");
    HRESULT hr=swap_->Present(vsync?1:0,!vsync&&tearing_?DXGI_PRESENT_ALLOW_TEARING:0);
    if(FAILED(hr)) return Fail(hr,"DXGI Present");
    // At most two frames queued: wait here (not inside Present) for a free slot.
    if(frameWait_)WaitForSingleObjectEx(frameWait_,250,TRUE);
    return hr; // Includes DXGI_STATUS_OCCLUDED; caller decides whether to pause.
}

HRESULT RenderDevice::DeviceStatus() const {
    return device_?device_->GetDeviceRemovedReason():E_UNEXPECTED;
}
}
