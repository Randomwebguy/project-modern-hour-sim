#include "CompatibilityRenderer.h"
#include "ScreenMaterialGate.h"
#include "LegacyShaderSource.h"
#include "ProgramSource.h"
#include "ShaderBytecodeCache.h"
#include "ShaderStageLinkage.h"
#include "TerrainCompactShaderSource.h"
#include "OpaqueShadowShaderSource.h"
#include "OpaqueShadowPolicy.h"
#include "TerrainExclusionFilter.h"
#include <DirectXPackedVector.h>
#include "LocalShadowBindings.h"
#include "ShadowReuseState.h"
#include "AtmosphereDrawPolicy.h"
#include "NativeSharedGeometry.h"
#include "EnvironmentGPU.h"
#include "NativeLightCalibrationGPU.h"
#include <limits>
#include "NativeLegacyPacking.h"
#include "DrawGrouping.h"
#include "SceneVisibility.h"
#include "../../NativeMeshLimits.h"
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstring>
using namespace GenZH::Modern;
unsigned checks=0;
void Check(bool ok,const char *label){++checks;if(!ok)throw std::runtime_error(label);}
void HR(HRESULT hr,const char *label){Check(SUCCEEDED(hr),label);}
std::vector<char> File(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){try{
 Check(argc==2,"shader directory required");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL fl{};
 const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};
 HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,device.GetAddressOf(),&fl,context.GetAddressOf()),"headless hardware device");
 ComPtr<IDXGIDevice> gd;ComPtr<IDXGIAdapter> ga;HR(device.As(&gd),"DXGI device");HR(gd->GetAdapter(ga.GetAddressOf()),"actual GPU");DXGI_ADAPTER_DESC ad{};ga->GetDesc(&ad);
 const std::filesystem::path directory(argv[1]);SetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",(directory/L"fixture-profile").c_str());
 std::array<D3DRESOURCETYPE,8> textureTypes{};textureTypes[0]=D3DRTYPE_TEXTURE;std::string sourceError;
 auto candidate=BuildProgramSource(LegacyShaderSource,false,false,{},{},textureTypes,sourceError);
 auto oldFile=File(directory/"reference.hlsl");Check(!oldFile.empty(),"immutable prior production source");
 const std::string oldSource(oldFile.begin(),oldFile.end());
 auto reference=BuildProgramSource(oldSource.c_str(),false,false,{},{},textureTypes,sourceError);
 auto compile=[&](const std::string &source,const char *entry,const char *profile){ComPtr<ID3DBlob> code;bool hit=false;std::string error;HR(CompileCachedShader(source,entry,profile,code,error,hit),error.c_str());return code;};
 auto vertex=compile(candidate,"LegacyVS","vs_5_0"),pixel=compile(candidate,"LegacyPS","ps_5_0"),prior=compile(reference,"LegacyPS","ps_5_0");
 const auto bytes=[](ID3DBlob *blob){const char *p=static_cast<const char*>(blob->GetBufferPointer());return std::vector<char>(p,p+blob->GetBufferSize());};
 auto vsCode=bytes(vertex.Get()),psCode=bytes(pixel.Get());ComPtr<ID3D11PixelShader> referencePS;
 HR(device->CreatePixelShader(prior->GetBufferPointer(),prior->GetBufferSize(),nullptr,referencePS.GetAddressOf()),"prior production PS");Check(!vsCode.empty()&&!psCode.empty(),"real shader bytecode");
 ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
 HR(device->CreateVertexShader(vsCode.data(),vsCode.size(),nullptr,vs.GetAddressOf()),"real VS");HR(device->CreatePixelShader(psCode.data(),psCode.size(),nullptr,ps.GetAddressOf()),"real PS");
 std::vector<D3D11_INPUT_ELEMENT_DESC> elements{{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0}};
 for(UINT i=0;i<8;++i)elements.push_back({"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64+i*16,D3D11_INPUT_PER_VERTEX_DATA,0});
 ComPtr<ID3D11InputLayout> layout;HR(device->CreateInputLayout(elements.data(),static_cast<UINT>(elements.size()),vsCode.data(),vsCode.size(),layout.GetAddressOf()),"compact input layout");
 auto buffer=[&](UINT bytes,UINT bind,const void *data){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.Usage=data?D3D11_USAGE_IMMUTABLE:D3D11_USAGE_DEFAULT;d.BindFlags=bind;
  D3D11_SUBRESOURCE_DATA init{};init.pSysMem=data;ComPtr<ID3D11Buffer> b;HR(device->CreateBuffer(&d,data?&init:nullptr,b.GetAddressOf()),"buffer");return b;};
 FixedVertex verts[3]{};const float xy[3][2]={{-1,-1},{3,-1},{-1,3}};
 for(unsigned i=0;i<3;++i){auto &v=verts[i];v.position[0]=xy[i][0];v.position[1]=xy[i][1];v.position[3]=1;v.normal[2]=1;
  for(unsigned j=0;j<4;++j)v.diffuse[j]=1;v.uv[0][0]=(xy[i][0]+1)*.5f;v.uv[0][1]=(xy[i][1]+1)*.5f;}
 const UINT indices[]={0,1,2};
 std::array<std::array<float,3>,3> nativePositions{},nativeNormals{};std::array<std::array<float,2>,3> nativeUV{};
 for(unsigned i=0;i<3;++i){std::memcpy(nativePositions[i].data(),verts[i].position,12);std::memcpy(nativeNormals[i].data(),verts[i].normal,12);std::memcpy(nativeUV[i].data(),verts[i].uv[0],8);}
 NativeGeometryView nativeView;nativeView.recipe=std::make_shared<NativeGeometryRecipe>();nativeView.recipe->polygons={0};nativeView.model=50;
 nativeView.fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1;nativeView.positions=nativePositions.data();nativeView.normals=nativeNormals.data();nativeView.uv[0]=nativeUV.data();nativeView.triangles=indices;
 nativeView.vertices=nativeView.range=3;nativeView.triangleCount=1;DecodedGeometry nativeGPUInput;
 HR(BuildNativeGeometry(nativeView,nativeGPUInput),"actual native compact GPU input");
 Check(std::memcmp(nativeGPUInput.fixedVertices.data(),verts,sizeof(verts))==0,"native GPU vertex input equals legacy fixture");
 Check(nativeGPUInput.indices==std::vector<std::uint32_t>({0,1,2}),"native GPU R32 topology");
 std::array<std::uint8_t,96> fallbackPacked{};
 Check(PackNativeLegacyVertices(nativeView,fallbackPacked.data(),fallbackPacked.size()),"actual fallback packer GPU input");
 std::vector<LegacyVertex> fallbackDecoded;std::string fallbackError;
 HR(DecodeVertices(fallbackPacked.data(),fallbackPacked.size(),32,nativeView.fvf,0,3,fallbackDecoded,fallbackError),"fallback FVF decode");
 for(unsigned i=0;i<3;++i)Check(std::memcmp(&fallbackDecoded[i],&nativeGPUInput.fixedVertices[i],192)==0,"native/fallback GPU input byte identical");
 // The visible material references nonzero global indices into one shared
 // full-model vertex buffer. Compare its real PBR pixels with the local path.
 std::array<std::array<float,3>,6> sharedPositions{},sharedNormals{};
 std::array<std::array<float,2>,6> sharedUV{};
 for(unsigned i=0;i<3;++i){sharedPositions[i]={50,50,0};sharedNormals[i]={0,0,1};sharedPositions[i+3]=nativePositions[i];sharedNormals[i+3]=nativeNormals[i];sharedUV[i+3]=nativeUV[i];}
 const UINT sharedTriangles[]={0,1,2,3,4,5};
 auto sharedView=nativeView;sharedView.positions=sharedPositions.data();sharedView.normals=sharedNormals.data();sharedView.uv[0]=sharedUV.data();sharedView.triangles=sharedTriangles;
 sharedView.vertices=6;sharedView.triangleCount=2;sharedView.first=3;sharedView.range=3;sharedView.recipe=std::make_shared<NativeGeometryRecipe>();sharedView.recipe->polygons={1};
 NativeVertexPool sharedPool;NativeVertexStats sharedStats;DecodedGeometry sharedInput,sharedOther;
 HR(BuildNativeSharedGeometry(sharedView,sharedPool,sharedInput,sharedStats),"actual shared material GPU input");
 auto otherView=sharedView;otherView.recipe=std::make_shared<NativeGeometryRecipe>();otherView.recipe->polygons={0};otherView.first=0;
 HR(BuildNativeSharedGeometry(otherView,sharedPool,sharedOther,sharedStats),"second shared GPU material");
 Check(sharedInput.sharedVertices==sharedOther.sharedVertices,"two materials one shared GPU owner");
 for(unsigned i=0;i<3;++i)Check(!std::memcmp(sharedInput.FixedData()+3+i,nativeGPUInput.FixedData()+i,192),"shared actual GPU attributes equal original local input");
 sharedInput.VertexBuffer()=buffer(6*192,D3D11_BIND_VERTEX_BUFFER,sharedInput.FixedData());
 Check(sharedInput.VertexBuffer().Get()==sharedOther.VertexBuffer().Get(),"one actual immutable GPU vertex buffer reused across materials");
 auto vb=sharedInput.VertexBuffer(),ib=buffer(sizeof(indices),D3D11_BIND_INDEX_BUFFER,sharedInput.indices.data());
 auto localVB=buffer(sizeof(verts),D3D11_BIND_VERTEX_BUFFER,nativeGPUInput.FixedData()),localIB=buffer(sizeof(indices),D3D11_BIND_INDEX_BUFFER,nativeGPUInput.indices.data());
 auto cb=buffer(sizeof(LegacyConstants),D3D11_BIND_CONSTANT_BUFFER,nullptr),sb=buffer(sizeof(ShadowConstants),D3D11_BIND_CONSTANT_BUFFER,nullptr),is=buffer(16,D3D11_BIND_CONSTANT_BUFFER,nullptr);
 LegacyConstants c{};for(unsigned i=0;i<4;++i){c.world[i*5]=c.view[i*5]=c.projection[i*5]=c.normalWorld[i*5]=1;c.materialDiffuse[i]=1;}
 c.cameraWorld[2]=5;c.modernMaterial[0]=1;c.modernMaterial[1]=.6f;c.viewport[2]=c.viewport[3]=32;
 for(unsigned i=0;i<3;++i){c.globalAmbient[i]=.4f;c.lightDiffuse[0][i]=.65f;}c.lightDirection[0][0]=-.6f;c.lightDirection[0][2]=-.8f;c.lightPosition[0][3]=3;
 c.colorStage[0][0]=4;c.colorStage[0][1]=2;c.colorStage[0][2]=0;c.alphaStage[0][0]=4;c.alphaStage[0][1]=2;c.alphaStage[0][2]=0;c.colorStage[1][0]=1;
 auto tex=[&](std::array<unsigned char,4> color){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{color.data(),4,4};ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11ShaderResourceView> srv;HR(device->CreateTexture2D(&d,&init,t.GetAddressOf()),"material texture");HR(device->CreateShaderResourceView(t.Get(),nullptr,srv.GetAddressOf()),"material SRV");return srv;};
 auto albedo=tex({128,128,128,255}),normal=tex({128,128,255,255}),tilted=tex({255,128,128,255}),orm=tex({192,192,0,255}),metal=tex({192,192,255,255}),height=tex({0,0,0,255}),emission=tex({128,128,128,255});
 auto lowAO=tex({0,192,0,255}),smooth=tex({192,32,0,255}),bright=tex({192,192,192,255});
 D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=32;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;HR(device->CreateTexture2D(&td,nullptr,target.GetAddressOf()),"offscreen float target");HR(device->CreateRenderTargetView(target.Get(),nullptr,rtv.GetAddressOf()),"offscreen RTV");
 td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&td,nullptr,staging.GetAddressOf()),"readback target");
 D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;HR(device->CreateSamplerState(&sd,sampler.GetAddressOf()),"sampler");
 D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;HR(device->CreateRasterizerState(&rd,raster.GetAddressOf()),"rasterizer");
 UINT stride=192,offset=0;auto *rawVB=vb.Get();context->IASetVertexBuffers(0,1,&rawVB,&stride,&offset);context->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);auto *rawCB=cb.Get();context->VSSetConstantBuffers(0,1,&rawCB);context->PSSetConstantBuffers(0,1,&rawCB);
 ShadowConstants shadow{};context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);rawCB=sb.Get();context->PSSetConstantBuffers(2,1,&rawCB);
 UINT settings[4]{};context->UpdateSubresource(is.Get(),0,nullptr,settings,0,0);rawCB=is.Get();context->VSSetConstantBuffers(4,1,&rawCB);
 auto *rawRTV=rtv.Get();context->OMSetRenderTargets(1,&rawRTV,nullptr);D3D11_VIEWPORT viewport{0,0,32,32,0,1};context->RSSetViewports(1,&viewport);context->RSSetState(raster.Get());auto *rawSampler=sampler.Get();context->PSSetSamplers(0,1,&rawSampler);
 NativeLightCalibrationGPU calibrated;HR(calibrated.Create(device.Get()),"canonical native lights GPU buffer");bool calibrationEnabled=true;
 GpuFrameTimer timer;timer.Initialize(device.Get());unsigned gpuSamples=0;double gpuMs=0;
 std::array<ID3D11ShaderResourceView*,3> terrainNativeExtras{};
 auto draw=[&](UINT flags,ID3D11ShaderResourceView *n,ID3D11ShaderResourceView *o,ID3D11ShaderResourceView *color=nullptr){c.modernMaterial[3]=static_cast<float>(flags);HR(calibrated.Prepare(context.Get(),c,calibrationEnabled),"production native light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
  ID3D11ShaderResourceView *v[10]{};v[0]=color?color:albedo.Get();for(unsigned j=0;j<3;++j)v[j+1]=terrainNativeExtras[j];v[8]=n;v[9]=o;context->PSSetShaderResources(0,10,v);ID3D11ShaderResourceView *extras[]={height.Get(),emission.Get()};context->PSSetShaderResources(16,2,extras);
  const float clear[4]{};timer.Begin(context.Get());context->ClearRenderTargetView(rtv.Get(),clear);context->DrawIndexed(3,0,0);timer.End(context.Get());context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"material readback");
  std::array<float,3> pixel{};auto *p=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+16*map.RowPitch)+16*4;std::copy_n(p,3,pixel.begin());context->Unmap(staging.Get(),0);timer.Poll(context.Get(),[&](double ms){++gpuSamples;gpuMs+=ms;});for(float value:pixel)Check(std::isfinite(value)&&value>=0,"finite PBR output");return pixel;};

 std::array<ComPtr<ID3D11Texture2D>,6> outputs;std::array<ComPtr<ID3D11RenderTargetView>,6> outputViews;
 outputs[0]=target;outputViews[0]=rtv;
 td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 for(unsigned i=1;i<6;++i){HR(device->CreateTexture2D(&td,nullptr,outputs[i].GetAddressOf()),"complete capture target");HR(device->CreateRenderTargetView(outputs[i].Get(),nullptr,outputViews[i].GetAddressOf()),"capture RTV");}
 ID3D11RenderTargetView *allTargets[6]{};for(unsigned i=0;i<6;++i)allTargets[i]=outputViews[i].Get();context->OMSetRenderTargets(6,allTargets,nullptr);
 const float captureData[]={1,15,0,0};auto control=buffer(sizeof(captureData),D3D11_BIND_CONSTANT_BUFFER,captureData);auto *controlRaw=control.Get();context->PSSetConstantBuffers(13,1,&controlRaw);
 auto readAll=[&](){std::vector<float> result;result.reserve(6*32*32*4);
  for(const auto &output:outputs){context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"full MRT GPU readback");
   for(unsigned y=0;y<32;++y){auto *row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);result.insert(result.end(),row,row+32*4);}context->Unmap(staging.Get(),0);}return result;};
 auto clearAll=[&](){const float zero[4]{};for(const auto &view:outputViews)context->ClearRenderTargetView(view.Get(),zero);};
 IBL::Data environmentData;Check(IBL::Load(directory/"environment-v46.bin",environmentData),"real environment asset");EnvironmentGPU environment;HR(environment.Create(device.Get(),environmentData),"full IBL GPU resources");environment.Bind(context.Get());
 const unsigned sunSize=256;std::vector<float> sunDepth(sunSize*sunSize,1.f);for(unsigned y=0;y<sunSize;++y)for(unsigned x=0;x<sunSize/2;++x)sunDepth[y*sunSize+x]=.3f;
 D3D11_TEXTURE2D_DESC sunDesc{};sunDesc.Width=sunDesc.Height=sunSize;sunDesc.MipLevels=1;sunDesc.ArraySize=2;sunDesc.SampleDesc.Count=1;sunDesc.Format=DXGI_FORMAT_R32_FLOAT;sunDesc.Usage=D3D11_USAGE_DEFAULT;sunDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 D3D11_SUBRESOURCE_DATA sunInitial[2]={{sunDepth.data(),sunSize*4,0},{sunDepth.data(),sunSize*4,0}};ComPtr<ID3D11Texture2D> sunTexture;ComPtr<ID3D11ShaderResourceView> sunView;
 HR(device->CreateTexture2D(&sunDesc,sunInitial,sunTexture.GetAddressOf()),"two real shadow cascades");HR(device->CreateShaderResourceView(sunTexture.Get(),nullptr,sunView.GetAddressOf()),"sun SRV");
 D3D11_SAMPLER_DESC sunSamplerDesc{};sunSamplerDesc.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;sunSamplerDesc.AddressU=sunSamplerDesc.AddressV=sunSamplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;sunSamplerDesc.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;std::fill_n(sunSamplerDesc.BorderColor,4,1.f);ComPtr<ID3D11SamplerState> sunSampler;
 HR(device->CreateSamplerState(&sunSamplerDesc,sunSampler.GetAddressOf()),"full PCSS comparison sampler");auto *sunRaw=sunView.Get();auto *sunSamplerRaw=sunSampler.Get();context->PSSetShaderResources(10,1,&sunRaw);context->PSSetSamplers(8,1,&sunSamplerRaw);
 for(unsigned i=0;i<4;++i)shadow.nearVP[i*5]=shadow.farVP[i*5]=1;
 shadow.nearVP[0]=shadow.nearVP[5]=shadow.farVP[0]=shadow.farVP[5]=.2f;shadow.nearVP[10]=shadow.farVP[10]=.001f;shadow.nearVP[14]=.6f;shadow.farVP[14]=.7f;
 shadow.nearSettings[0]=shadow.farSettings[0]=.00001f;shadow.nearSettings[1]=shadow.nearSettings[2]=shadow.farSettings[1]=shadow.farSettings[2]=1.f/sunSize;shadow.nearSettings[3]=1;shadow.sun[0]=.6f;shadow.sun[2]=.8f;
 AtmosphereConstants weather{};weather.range[0]=4096;weather.shadowArea[2]=8192;for(unsigned i=0;i<4;++i)weather.viewProjection[i*5]=1;
 auto weatherCB=buffer(sizeof(weather),D3D11_BIND_CONSTANT_BUFFER,nullptr);auto *weatherRaw=weatherCB.Get();context->PSSetConstantBuffers(7,1,&weatherRaw);
 D3D11_TEXTURE3D_DESC neutralDesc{};neutralDesc.Width=neutralDesc.Height=neutralDesc.Depth=neutralDesc.MipLevels=1;neutralDesc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;neutralDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 float neutralPixel[]={0,0,0,1};D3D11_SUBRESOURCE_DATA neutralInitial{neutralPixel,16,16};ComPtr<ID3D11Texture3D> neutralTexture;ComPtr<ID3D11ShaderResourceView> neutralVolume;
 HR(device->CreateTexture3D(&neutralDesc,&neutralInitial,neutralTexture.GetAddressOf()),"neutral integral");HR(device->CreateShaderResourceView(neutralTexture.Get(),nullptr,neutralVolume.GetAddressOf()),"volume SRV");auto *volumeRaw=neutralVolume.Get();context->PSSetShaderResources(23,1,&volumeRaw);context->PSSetSamplers(12,1,&rawSampler);
 auto cloud=tex({137,137,137,255});auto *cloudRaw=cloud.Get();context->PSSetShaderResources(24,1,&cloudRaw);
 auto cube=[&](float red,float green,float blue){D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=desc.MipLevels=desc.SampleDesc.Count=1;desc.ArraySize=6;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
  float color[]={red,green,blue,1};D3D11_SUBRESOURCE_DATA initial[6]{};for(auto &entry:initial){entry.pSysMem=color;entry.SysMemPitch=16;}ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11ShaderResourceView> view;HR(device->CreateTexture2D(&desc,initial,t.GetAddressOf()),"weather cube");HR(device->CreateShaderResourceView(t.Get(),nullptr,view.GetAddressOf()),"weather cube view");return view;};
 auto cold=cube(.1f,.25f,.8f),warm=cube(.9f,.22f,.06f);ID3D11ShaderResourceView *weatherProbes[]={warm.Get(),warm.Get(),cold.Get(),cold.Get()};context->PSSetShaderResources(29,4,weatherProbes);
 ID3D11ShaderResourceView *wearViews[]={bright.Get(),tilted.Get(),smooth.Get()};context->PSSetShaderResources(26,3,wearViews);
 unsigned cases=0;std::uint64_t values=0;float maxError=0;double nonempty=0;
 const auto base=c;
 for(unsigned lights=0;lights<=4;++lights)for(unsigned weatherMode=0;weatherMode<3;++weatherMode)for(unsigned cascade=0;cascade<2;++cascade)for(unsigned material=0;material<4;++material)for(unsigned wet=0;wet<2;++wet){
  c=base;for(unsigned i=0;i<4;++i){c.lightPosition[i][3]=i<lights?3.f:0.f;c.lightDirection[i][0]=-.6f;c.lightDirection[i][2]=-.8f;for(unsigned channel=0;channel<3;++channel){c.lightDiffuse[i][channel]=.2f+.1f*i;c.lightAmbient[i][channel]=.03f;}}
  if(lights==0){c.lightPosition[0][0]=.5f;c.lightPosition[0][2]=2;c.lightPosition[0][3]=1;c.lightAttenuation[0][0]=1;c.lightAttenuation[0][3]=100;}
  weather.camera[3]=weatherMode?1.f:0.f;weather.shadowArea[3]=weatherMode==2?1.f:0.f;weather.sunColor[3]=.37f;weather.weather[0]=.45f;weather.weather[2]=wet?1.f:0.f;for(unsigned i=0;i<3;++i)weather.ambient[i]=.12f;
  context->UpdateSubresource(weatherCB.Get(),0,nullptr,&weather,0,0);shadow.split[3]=cascade?2.f:0.f;shadow.split[0]=0;shadow.split[1]=10;shadow.split[2]=100;shadow.cameraDepth[2]=1;context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);
  const UINT flags=material==0?0u:material==1?3u:material==2?195u:1795u;
  if(material==3){c.vehicleAge[0]=.65f;c.vehicleAge[1]=.35f;c.vehicleAge[3]=1;c.vehicleProfile[1]=10;}
  auto *normalView=material==2?tilted.Get():normal.Get();auto *ormView=material==2?metal.Get():orm.Get();
  context->PSSetShader(referencePS.Get(),nullptr,0);clearAll();draw(flags,normalView,ormView);const auto before=readAll();
  context->PSSetShader(ps.Get(),nullptr,0);clearAll();draw(flags,normalView,ormView);const auto after=readAll();Check(before.size()==after.size(),"equal capture sizes");
  for(std::size_t i=0;i<before.size();++i){Check(std::isfinite(before[i])&&std::isfinite(after[i]),"finite complete MRT");const float error=std::abs(before[i]-after[i]);maxError=std::max(maxError,error);Check(error<=.00001f+.00002f*std::abs(before[i]),"old/new full material/shadow/MRT equality");nonempty+=std::abs(before[i]);++values;}++cases;
 }
 Check(nonempty>1,"fixture draws real illuminated pixels");
 unsigned exclusionValues=0;
 for(unsigned width:{1u,2u,3u,8u,31u})for(unsigned maskHeight:{1u,2u,7u,19u}){
  std::vector<std::uint8_t> source(static_cast<std::size_t>(width)*maskHeight);
  for(std::size_t i=0;i<source.size();++i)source[i]=static_cast<std::uint8_t>((i*37+i/3*53)%256);
  const auto filtered=FilterTerrainExclusion(source,width,maskHeight);
  for(unsigned y=0;y<maskHeight;++y)for(unsigned x=0;x<width;++x){std::uint8_t expected=0;
   for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const auto xx=std::clamp(static_cast<int>(x)+dx,0,static_cast<int>(width)-1),yy=std::clamp(static_cast<int>(y)+dy,0,static_cast<int>(maskHeight)-1);expected=std::max(expected,source[static_cast<std::size_t>(yy)*width+xx]);}
   Check(filtered[static_cast<std::size_t>(y)*width+x]==expected,"production occupancy filter equals independent clamped 3x3 oracle");++exclusionValues;
  }
 }
 // Actual production tessellation/blade generation, all retained varyings.
 {
 const auto savedTerrainC=c;const auto savedTerrainShadow=shadow;
 auto hull=compile(candidate,"TerrainHS","hs_5_0"),oldHull=compile(reference,"TerrainHS","hs_5_0"),domain=compile(candidate,"TerrainDS","ds_5_0"),oldDomain=compile(reference,"TerrainDS","ds_5_0"),fullGrass=compile(reference,"TerrainGrassGS","gs_5_0"),compactGrass=compile(candidate,"TerrainGrassCompactGS","gs_5_0");
 Check(CanLinkRasterStage(compactGrass.Get(),pixel.Get()),"original material inputs can be represented");
 auto compactPixel=compile(candidate+TerrainCompactShaderSuffix,"TerrainCompactPS","ps_5_0"),compactShadowPixel=compile(candidate+TerrainCompactShaderSuffix,"TerrainCompactShadowPS","ps_5_0");
 Check(CanLinkRasterStage(compactGrass.Get(),compactPixel.Get(),true)&&CanLinkRasterStage(compactGrass.Get(),compactShadowPixel.Get(),true),"paired color/shadow raster registers match exactly");
 ComPtr<ID3D11PixelShader> compactRasterPS;HR(device->CreatePixelShader(compactPixel->GetBufferPointer(),compactPixel->GetBufferSize(),nullptr,compactRasterPS.GetAddressOf()),"paired compact material PS");
 auto actualShadow=compile(candidate,"LegacyShadowPS","ps_5_0");Check(CanLinkRasterStage(compactGrass.Get(),actualShadow.Get()),"compact signature serves actual alpha shadow PS");
 Check(!CanLinkRasterStage(nullptr,pixel.Get()),"missing reflection falls back");
 auto unsupported=compile("float4 Probe(float4 missing:TEXCOORD7):SV_TARGET{return missing;}","Probe","ps_5_0");Check(!CanLinkRasterStage(compactGrass.Get(),unsupported.Get()),"unsupported UV consumer retains full grass stage");
 ComPtr<ID3D11HullShader> hs,priorHS;ComPtr<ID3D11DomainShader> ds,priorDS;ComPtr<ID3D11GeometryShader> oldGrassSO,newGrassSO,oldGrassRaster,newGrassRaster;
 HR(device->CreateHullShader(hull->GetBufferPointer(),hull->GetBufferSize(),nullptr,hs.GetAddressOf()),"production terrain HS");HR(device->CreateHullShader(oldHull->GetBufferPointer(),oldHull->GetBufferSize(),nullptr,priorHS.GetAddressOf()),"prior terrain HS");HR(device->CreateDomainShader(domain->GetBufferPointer(),domain->GetBufferSize(),nullptr,ds.GetAddressOf()),"production terrain DS");HR(device->CreateDomainShader(oldDomain->GetBufferPointer(),oldDomain->GetBufferSize(),nullptr,priorDS.GetAddressOf()),"prior terrain DS");
 const D3D11_SO_DECLARATION_ENTRY declarations[]={
 {0,"SV_POSITION",0,0,4,0},{0,"COLOR",0,0,4,0},{0,"COLOR",1,0,4,0},
 {0,"TEXCOORD",0,0,4,0},{0,"TEXCOORD",1,0,4,0},{0,"TEXCOORD",2,0,4,0},{0,"TEXCOORD",3,0,4,0},
 {0,"TEXCOORD",8,0,1,0},{0,"TEXCOORD",10,0,3,0},{0,"TEXCOORD",11,0,3,0},{0,"TEXCOORD",12,0,1,0}};
 const UINT outputStride=36*4;
 HR(device->CreateGeometryShaderWithStreamOutput(fullGrass->GetBufferPointer(),fullGrass->GetBufferSize(),declarations,11,&outputStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,oldGrassSO.GetAddressOf()),"prior full GS stream");
 HR(device->CreateGeometryShaderWithStreamOutput(compactGrass->GetBufferPointer(),compactGrass->GetBufferSize(),declarations,11,&outputStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,newGrassSO.GetAddressOf()),"compact GS stream");
 HR(device->CreateGeometryShader(fullGrass->GetBufferPointer(),fullGrass->GetBufferSize(),nullptr,oldGrassRaster.GetAddressOf()),"prior full raster GS");HR(device->CreateGeometryShader(compactGrass->GetBufferPointer(),compactGrass->GetBufferSize(),nullptr,newGrassRaster.GetAddressOf()),"compact raster GS");
 FixedVertex terrainVertices[3]{};const float terrainXY[3][2]={{1,1},{9,1},{1,9}};
 for(unsigned i=0;i<3;++i){auto &terrainVertex=terrainVertices[i];terrainVertex.position[0]=terrainXY[i][0];terrainVertex.position[1]=terrainXY[i][1];terrainVertex.position[3]=1;terrainVertex.normal[2]=1;for(float &channel:terrainVertex.diffuse)channel=1;for(unsigned uv=0;uv<8;++uv){terrainVertex.uv[uv][0]=terrainVertex.position[0]/10;terrainVertex.uv[uv][1]=terrainVertex.position[1]/10;}}
 auto terrainVB=buffer(sizeof(terrainVertices),D3D11_BIND_VERTEX_BUFFER,terrainVertices);
 struct Cell{float uv[4][4];float alpha[4];};Cell cell{};const float canonicalUV[4][2]={{0,0},{1,0},{1,1},{0,1}};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<2;++j)cell.uv[i][j]=cell.uv[i][j+2]=canonicalUV[i][j];
 D3D11_BUFFER_DESC cellDesc{};cellDesc.ByteWidth=cellDesc.StructureByteStride=sizeof(cell);cellDesc.Usage=D3D11_USAGE_IMMUTABLE;cellDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;cellDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
 D3D11_SUBRESOURCE_DATA cellInit{&cell,0,0};ComPtr<ID3D11Buffer> cells;ComPtr<ID3D11ShaderResourceView> cellView;HR(device->CreateBuffer(&cellDesc,&cellInit,cells.GetAddressOf()),"canonical GPU terrain cell");D3D11_SHADER_RESOURCE_VIEW_DESC cellSRV{};cellSRV.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;cellSRV.Buffer.NumElements=1;HR(device->CreateShaderResourceView(cells.Get(),&cellSRV,cellView.GetAddressOf()),"terrain cells view");
 auto terrainHeight=tex({255,255,255,255}),grassClass=tex({1,0,0,255}),soilClass=tex({2,0,0,255}),uncovered=tex({0,0,0,255}),covered=tex({255,0,0,255});const float dimensions[]={1,1,0,10};auto mapConstants=buffer(16,D3D11_BIND_CONSTANT_BUFFER,dimensions);
 std::array<ComPtr<ID3D11ShaderResourceView>,4> rawMasks,filteredMasks;
 auto makeMask=[&](const std::vector<std::uint8_t> &maskPixels){D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=8;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8_UNORM;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{maskPixels.data(),8,0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;HR(device->CreateTexture2D(&desc,&data,texture.GetAddressOf()),"occupancy fixture texture");HR(device->CreateShaderResourceView(texture.Get(),nullptr,view.GetAddressOf()),"occupancy fixture view");return view;};
 for(unsigned kind=0;kind<4;++kind){std::vector<std::uint8_t> maskPixels(64);for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)maskPixels[y*8+x]=(kind==1||(kind==2&&x==3)||(kind==3&&x+y>7&&x+y<10))?255:0;rawMasks[kind]=makeMask(maskPixels);filteredMasks[kind]=makeMask(FilterTerrainExclusion(maskPixels,8,8));}
 auto output=buffer(4*1024*1024,D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER,nullptr);D3D11_BUFFER_DESC outDesc{};output->GetDesc(&outDesc);outDesc.BindFlags=0;outDesc.Usage=D3D11_USAGE_STAGING;outDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> outputCPU;HR(device->CreateBuffer(&outDesc,nullptr,outputCPU.GetAddressOf()),"terrain stream readback");D3D11_QUERY_DESC queryDesc{D3D11_QUERY_SO_STATISTICS,0};ComPtr<ID3D11Query> query;HR(device->CreateQuery(&queryDesc,query.GetAddressOf()),"actual grass primitive count");
 c=base;c.modernMaterial[0]=2;c.modernMaterial[3]=43;c.alphaStage[0][3]=0;c.cameraWorld[0]=5;c.cameraWorld[1]=-5;c.cameraWorld[2]=8;
 DirectX::XMFLOAT4X4 cameraMatrix,projectionMatrix;DirectX::XMStoreFloat4x4(&cameraMatrix,DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(5,-5,8,1),DirectX::XMVectorSet(5,5,0,1),DirectX::XMVectorSet(0,0,1,0)));DirectX::XMStoreFloat4x4(&projectionMatrix,DirectX::XMMatrixOrthographicLH(12,12,.1f,100));std::memcpy(c.view,&cameraMatrix,64);std::memcpy(c.projection,&projectionMatrix,64);
 shadow={};for(unsigned i=0;i<4;++i)shadow.nearVP[i*5]=shadow.farVP[i*5]=1;shadow.nearVP[0]=shadow.nearVP[5]=.2f;shadow.nearVP[10]=-.05f;shadow.nearVP[12]=shadow.nearVP[13]=-1;shadow.nearVP[14]=.5f;shadow.farVP[0]=shadow.farVP[5]=.1f;shadow.farVP[10]=-.03f;shadow.farVP[12]=shadow.farVP[13]=-1;shadow.farVP[14]=.7f;
 context->VSSetShader(vs.Get(),nullptr,0);context->HSSetShader(hs.Get(),nullptr,0);context->DSSetShader(ds.Get(),nullptr,0);auto *terrainRaw=terrainVB.Get();context->IASetVertexBuffers(0,1,&terrainRaw,&stride,&offset);context->IASetIndexBuffer(localIB.Get(),DXGI_FORMAT_R32_UINT,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
 auto *raw=cb.Get();context->HSSetConstantBuffers(0,1,&raw);context->DSSetConstantBuffers(0,1,&raw);context->GSSetConstantBuffers(0,1,&raw);raw=sb.Get();context->GSSetConstantBuffers(2,1,&raw);context->DSSetConstantBuffers(2,1,&raw);context->HSSetConstantBuffers(2,1,&raw);raw=mapConstants.Get();context->DSSetConstantBuffers(3,1,&raw);context->GSSetConstantBuffers(3,1,&raw);context->DSSetSamplers(9,1,&rawSampler);
 D3D11_TEXTURE2D_DESC terrainDepthDesc{};terrainDepthDesc.Width=terrainDepthDesc.Height=32;terrainDepthDesc.ArraySize=terrainDepthDesc.MipLevels=terrainDepthDesc.SampleDesc.Count=1;terrainDepthDesc.Format=DXGI_FORMAT_R32_TYPELESS;terrainDepthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
 ComPtr<ID3D11Texture2D> terrainDepth,terrainDepthRead;ComPtr<ID3D11DepthStencilView> terrainDSV;HR(device->CreateTexture2D(&terrainDepthDesc,nullptr,terrainDepth.GetAddressOf()),"terrain shadow depth");D3D11_DEPTH_STENCIL_VIEW_DESC terrainDepthView{};terrainDepthView.Format=DXGI_FORMAT_D32_FLOAT;terrainDepthView.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;HR(device->CreateDepthStencilView(terrainDepth.Get(),&terrainDepthView,terrainDSV.GetAddressOf()),"terrain depth view");terrainDepthDesc.BindFlags=0;terrainDepthDesc.Usage=D3D11_USAGE_STAGING;terrainDepthDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&terrainDepthDesc,nullptr,terrainDepthRead.GetAddressOf()),"terrain depth staging");
 D3D11_DEPTH_STENCIL_DESC terrainDepthConfig{};terrainDepthConfig.DepthEnable=TRUE;terrainDepthConfig.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;terrainDepthConfig.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;ComPtr<ID3D11DepthStencilState> terrainDepthState;HR(device->CreateDepthStencilState(&terrainDepthConfig,terrainDepthState.GetAddressOf()),"terrain shadow depth state");
 ComPtr<ID3D11PixelShader> terrainOldDepthPS,terrainNewDepthPS;HR(device->CreatePixelShader(actualShadow->GetBufferPointer(),actualShadow->GetBufferSize(),nullptr,terrainOldDepthPS.GetAddressOf()),"full terrain depth PS");HR(device->CreatePixelShader(compactShadowPixel->GetBufferPointer(),compactShadowPixel->GetBufferSize(),nullptr,terrainNewDepthPS.GetAddressOf()),"paired compact depth PS");auto terrainTransparent=tex({128,128,128,64});unsigned terrainDepthCases=0,terrainColorDepthCases=0;bool terrainOpaqueDepth=false;
 unsigned terrainCases=0,terrainRasterCases=0;std::uint64_t terrainVerticesCompared=0;bool foundBlades=false;float terrainPixelError=0;
 for(unsigned projection=0;projection<3;++projection)for(unsigned ground=0;ground<2;++ground)for(unsigned obstruction=0;obstruction<4;++obstruction)for(unsigned blades=0;blades<2;++blades){
  c.viewport[2]=1280;c.viewport[3]=720;c.modernMaterial[3]=blades?43.f:11.f;context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);shadow.render[0]=projection==2?1.f:0.f;shadow.render[1]=projection?1.f:0.f;context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);
  ID3D11ShaderResourceView *resources[]={terrainHeight.Get(),ground?grassClass.Get():soilClass.Get(),cellView.Get(),obstruction?covered.Get():uncovered.Get()};context->DSSetShaderResources(11,4,resources);context->GSSetShaderResources(11,4,resources);
  auto bindOccupancy=[&](bool original){auto *view=filteredMasks[obstruction].Get();context->DSSetShaderResources(14,1,&view);context->GSSetShaderResources(14,1,&view);context->DSSetShader(original?priorDS.Get():ds.Get(),nullptr,0);context->HSSetShader(original?priorHS.Get():hs.Get(),nullptr,0);};
  auto readVertices=[&](ID3D11GeometryShader *geometry){bindOccupancy(geometry==oldGrassSO.Get());context->GSSetShader(geometry,nullptr,0);context->PSSetShader(nullptr,nullptr,0);auto *out=output.Get();UINT start=0;context->SOSetTargets(1,&out,&start);context->Begin(query.Get());context->DrawIndexed(3,0,0);context->End(query.Get());ID3D11Buffer *empty=nullptr;context->SOSetTargets(1,&empty,&start);context->CopyResource(outputCPU.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(outputCPU.Get(),0,D3D11_MAP_READ,0,&map),"procedural vertex bytes");D3D11_QUERY_DATA_SO_STATISTICS statistics{};HR(context->GetData(query.Get(),&statistics,sizeof(statistics),0),"completed primitive statistics");Check(statistics.NumPrimitivesWritten>0&&statistics.NumPrimitivesWritten==statistics.PrimitivesStorageNeeded,"nonempty without SO overflow");const auto bytes=statistics.NumPrimitivesWritten*3*outputStride;Check(bytes<=4*1024*1024,"bounded actual vertex output");std::vector<float> result(static_cast<std::size_t>(bytes/4));std::memcpy(result.data(),map.pData,static_cast<std::size_t>(bytes));context->Unmap(outputCPU.Get(),0);return result;};
  const auto before=readVertices(oldGrassSO.Get()),after=readVertices(newGrassSO.Get());Check(before==after,"all 36 retained attributes, every procedural vertex exact");terrainVerticesCompared+=before.size()/36;++terrainCases;for(std::size_t i=35;i<before.size();i+=36)foundBlades|=before[i]>.5f;
  if(projection==0){c.viewport[2]=c.viewport[3]=32;context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);context->OMSetRenderTargets(6,allTargets,nullptr);context->PSSetShader(ps.Get(),nullptr,0);context->GSSetShader(oldGrassRaster.Get(),nullptr,0);bindOccupancy(true);clearAll();draw(blades?43u:11u,normal.Get(),orm.Get());const auto original=readAll();context->GSSetShader(newGrassRaster.Get(),nullptr,0);context->PSSetShader(compactRasterPS.Get(),nullptr,0);bindOccupancy(false);clearAll();draw(blades?43u:11u,normal.Get(),orm.Get());const auto packed=readAll();for(std::size_t i=0;i<original.size();++i){terrainPixelError=std::max(terrainPixelError,std::abs(original[i]-packed[i]));if(original[i]!=packed[i]){std::cerr<<"MRT mismatch "<<i<<" prior="<<original[i]<<" compact="<<packed[i]<<" absolute="<<std::abs(original[i]-packed[i])<<std::endl;throw std::runtime_error("full terrain/grass MRT unchanged");}}++terrainRasterCases;}
  else{
   const auto priorAlpha=c.alphaStage[0][3];const auto priorReference=c.bridgeFlags[1];
   for(unsigned alphaMode=0;alphaMode<2;++alphaMode){
    c.alphaStage[0][3]=alphaMode?5u:0u;c.bridgeFlags[1]=.5f;
    auto depthResult=[&](ID3D11GeometryShader *geometry,ID3D11PixelShader *pixelShader){bindOccupancy(geometry==oldGrassRaster.Get());
     context->OMSetRenderTargets(0,nullptr,terrainDSV.Get());context->OMSetDepthStencilState(terrainDepthState.Get(),0);context->ClearDepthStencilView(terrainDSV.Get(),D3D11_CLEAR_DEPTH,1,0);context->GSSetShader(geometry,nullptr,0);context->PSSetShader(pixelShader,nullptr,0);draw(blades?43u:11u,normal.Get(),orm.Get(),alphaMode?terrainTransparent.Get():albedo.Get());
     context->CopyResource(terrainDepthRead.Get(),terrainDepth.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(terrainDepthRead.Get(),0,D3D11_MAP_READ,0,&map),"terrain paired shadow readback");std::vector<float> result;for(unsigned y=0;y<32;++y){auto *row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);result.insert(result.end(),row,row+32);}context->Unmap(terrainDepthRead.Get(),0);return result;
    };
    const auto beforeDepth=depthResult(oldGrassRaster.Get(),terrainOldDepthPS.Get()),afterDepth=depthResult(newGrassRaster.Get(),terrainNewDepthPS.Get());Check(beforeDepth==afterDepth,"paired terrain opaque/cutout shadow coverage exact");if(!alphaMode)terrainOpaqueDepth|=*std::min_element(afterDepth.begin(),afterDepth.end())<1;else Check(*std::min_element(afterDepth.begin(),afterDepth.end())==1,"paired terrain cutout rejects depth");++terrainDepthCases;
    if(!alphaMode){
     const auto originalC=c;
     c.lightDiffuse[0][0]=.13f;c.lightDirection[0][1]=.73f;c.globalAmbient[0]=.91f;
     c.materialDiffuse[3]=.12f;c.materialEmissive[0]=.8f;c.fogSettings[3]=1-c.fogSettings[3];
     c.textureMatrices[0][12]+=.37f;c.coordinateStage[0][0]=2u<<16;
     c.modernMaterial[1]=.91f;c.modernMaterial[2]=.71f;c.vehicleAge[0]=.99f;
     const auto relitDepth=depthResult(newGrassRaster.Get(),terrainNewDepthPS.Get());
     Check(relitDepth==afterDepth,"native color/lighting/UV/fog/wear cannot change opaque terrain depth");
     c=originalC;++terrainColorDepthCases;
    }
   }
   c.alphaStage[0][3]=priorAlpha;c.bridgeFlags[1]=priorReference;
  }

 }
 // Exact cache-validity policy for the real terrain resource set, in addition
 // to the actual opaque depth pixels above. Revision/ownership and native
 // camera/geometry/exclusion changes must never reuse obsolete terrain depth.
 DrawPacket terrainStamp;terrainStamp.terrain=true;terrainStamp.fvf=D3DFVF_XYZ|D3DFVF_NORMAL;
 terrainStamp.primitive=terrainStamp.sourcePrimitive=D3DPT_TRIANGLELIST;
 terrainStamp.geometry=std::make_shared<DecodedGeometry>();terrainStamp.geometry->indices={0,1,2};
 terrainStamp.constants=c;terrainStamp.constants.alphaStage[0][3]=0;
 terrainStamp.MutableStates().render[D3DRS_ZENABLE]=terrainStamp.MutableStates().render[D3DRS_ZWRITEENABLE]=1;
 terrainStamp.terrainHeight=terrainHeight;terrainStamp.terrainClass=grassClass;terrainStamp.terrainCells=cellView;
 terrainStamp.terrainDimensions[0]=terrainStamp.terrainDimensions[1]=1;terrainStamp.terrainDimensions[3]=10;
 terrainStamp.terrainShadowOwner=std::make_shared<int>(1);terrainStamp.sourceIdentity[1]=7;
 ShadowDrawStamp terrainA,terrainB;unsigned terrainStampChecks=0;
 auto stampCheck=[&](bool result,const char *label){Check(result,label);++terrainStampChecks;};
 stampCheck(CaptureShadowStamp(terrainStamp,terrainA,3),"opaque canonical terrain can be tracked");
 auto relit=terrainStamp;relit.constants.lightDiffuse[0][0]=.13f;relit.constants.lightDirection[0][1]=.73f;
 relit.constants.globalAmbient[0]=.91f;relit.constants.materialDiffuse[3]=.12f;relit.constants.materialEmissive[0]=.8f;
 relit.constants.fogSettings[3]=1-relit.constants.fogSettings[3];relit.constants.textureMatrices[0][12]+=.37f;
 relit.constants.coordinateStage[0][0]=2u<<16;relit.constants.modernMaterial[1]=.91f;
 relit.constants.modernMaterial[2]=.71f;relit.constants.vehicleAge[0]=.99f;
 // accum105: pixel-only bits above 65535 (field, hex, wet ground, snow, dust) keep the stamp.
 relit.constants.modernMaterial[3]=float(static_cast<unsigned>(relit.constants.modernMaterial[3])^(128u|65536u|131072u|262144u|524288u|1048576u));
 stampCheck(CaptureShadowStamp(relit,terrainB,3)&&terrainA==terrainB,"appearance changes retain exact opaque terrain coverage stamp");
 for(unsigned change=0;change<16;++change){auto q=terrainStamp;std::uint64_t exclusion=3;
  if(change==0)q.constants.world[12]+=.1f;if(change==1)q.constants.view[12]+=.1f;
  if(change==2)q.constants.projection[0]+=.1f;if(change==3)q.constants.viewport[2]+=1;
  if(change==4)q.constants.modernMaterial[0]=0;
  if(change==5)q.constants.modernMaterial[3]=float(static_cast<unsigned>(q.constants.modernMaterial[3])^32u);
  if(change==6)++q.sourceIdentity[1];if(change==7)q.terrainDimensions[0]+=1;
  if(change==8)q.terrainHeight=soilClass;if(change==9)q.terrainClass=soilClass;
  if(change==10)q.terrainCells=grassClass;if(change==11)++exclusion;
  if(change==12)q.sourcePrimitive=D3DPT_TRIANGLESTRIP;if(change==13)q.geometry=std::make_shared<DecodedGeometry>(*q.geometry);
  if(change==14)q.terrainShadowOwner=std::make_shared<int>(2);
  if(change==15)q.constants.modernMaterial[3]=float(static_cast<unsigned>(q.constants.modernMaterial[3])^8192u); // relief displaces the depth
  stampCheck(CaptureShadowStamp(q,terrainB,exclusion)&&!(terrainA==terrainB),"geometry/camera/map/source/coverage ownership changes invalidate terrain reuse");
 }
 for(unsigned unsafe=0;unsafe<7;++unsafe){auto q=terrainStamp;
  if(unsafe==0)q.terrainShadowOwner.reset();if(unsafe==1)q.nativeSource.skinned=true;
  if(unsafe==2)q.vertexShader=1;if(unsafe==3)q.nativeSource.sorted=true;
  if(unsafe==4)q.constants.alphaStage[0][3]=5;if(unsafe==5)q.MutableStates().render[D3DRS_ALPHABLENDENABLE]=1;
  if(unsafe==6)q.constants.modernMaterial[3]=std::numeric_limits<float>::quiet_NaN();
  stampCheck(!CaptureShadowStamp(q,terrainB,3),"untracked/custom/cutout terrain remains live");
 }
 ShadowReuseCache terrainCache;const float *terrainVP=shadow.nearVP;
 auto firstTerrain=terrainCache.Prepare({terrainA},terrainVP);stampCheck(!firstTerrain.hit&&!firstTerrain.stationary[0],"first terrain draw remains live");terrainCache.Commit();
 auto secondTerrain=terrainCache.Prepare({terrainA},terrainVP);stampCheck(!secondTerrain.hit&&secondTerrain.stationary[0],"confirmed terrain builds stationary layer");terrainCache.Commit();
 stampCheck(CaptureShadowStamp(relit,terrainB,3),"relit terrain stamp valid");
 auto warmTerrain=terrainCache.Prepare({terrainB},terrainVP);stampCheck(warmTerrain.hit&&warmTerrain.stationary[0],"relighting hits warm terrain shadow layer");terrainCache.DiscardPending();
 stampCheck(warmTerrain.terrainTracked==1&&warmTerrain.terrainSourceChanged==0&&warmTerrain.terrainCameraChanged==0&&warmTerrain.terrainOwnerChanged==0,"warm terrain telemetry reports no false geometry/camera invalidation");
 auto revisedTerrain=terrainStamp;++revisedTerrain.sourceIdentity[1];stampCheck(CaptureShadowStamp(revisedTerrain,terrainB,3),"changed native buffer revision tracked");
 auto sourcePlan=terrainCache.Prepare({terrainB},terrainVP);stampCheck(sourcePlan.terrainSourceChanged==1&&!sourcePlan.stationary[0],"native buffer revision diagnostic identifies exact changing dependency");
 stampCheck(CaptureShadowStamp(relit,terrainB,3),"restore warm coverage after revision diagnostic");
 terrainCache.Prepare({terrainB},terrainVP);terrainCache.DiscardPending();
 stampCheck(terrainCache.Prepare({terrainB},terrainVP).hit,"restore identical warm layer before isolated light matrix test");terrainCache.DiscardPending();
 auto movedLight=shadow; movedLight.nearVP[12]+=.01f;
 const auto movedLightPlan=terrainCache.Prepare({terrainB},movedLight.nearVP);
 stampCheck(!movedLightPlan.hit&&movedLightPlan.stationary[0],"moving shadow projection alone rebuilds stationary terrain");
 std::ofstream terrainStampReceipt(directory/(sizeof(void*)==8?"terrain-shadow-state-x64.json":"terrain-shadow-state-x86.json"));
 terrainStampReceipt<<"{\"passed\":true,\"actual_GPU_depth_cases\":"<<terrainColorDepthCases<<",\"pixel_exact\":true,\"invalidation_and_cache_checks\":"<<terrainStampChecks<<",\"geometry_or_material_quality_reduced\":false}\n";
 Check(bool(terrainStampReceipt),"terrain coverage state receipt written");
 // Compare genuine prior/new hull stages at all six clip planes, guard
 // bands and partial intersections. Pixel equality alone cannot prove culling:
 // actual hardware DS counters must also show eliminated invisible work.
 const auto beforeFrustumC=c;unsigned frustumCases=0,frustumCulled=0;std::uint64_t priorDomains=0,newDomains=0;
 const float offsets[][3]={{0,0,2},{100,0,2},{-100,0,2},{0,100,2},{0,-100,2},{0,0,-100},{0,0,200},{5.4f,0,2},{-9.4f,0,2},{0,0,-.8f},{0,0,99},{-3,-3,2}};
 c=base;c.modernMaterial[0]=2;c.modernMaterial[3]=43;c.alphaStage[0][3]=0;c.viewport[2]=1280;c.viewport[3]=720;
 for(unsigned i=0;i<16;++i)c.view[i]=0;for(unsigned i=0;i<4;++i)c.view[i*5]=1;
 DirectX::XMStoreFloat4x4(&projectionMatrix,DirectX::XMMatrixOrthographicLH(12,12,.1f,100));std::memcpy(c.projection,&projectionMatrix,64);
 auto frustumFile=File(directory/"frustum-reference.hlsl");Check(!frustumFile.empty(),"independent pre-culling terrain oracle");
 const std::string frustumText(frustumFile.begin(),frustumFile.end());const auto frustumSource=BuildProgramSource(frustumText.c_str(),false,false,{},{},textureTypes,sourceError);
 auto originalFrustumHS=compile(frustumSource,"TerrainHS","hs_5_0"),originalFrustumDS=compile(frustumSource,"TerrainDS","ds_5_0"),originalFrustumGS=compile(frustumSource,"TerrainGrassGS","gs_5_0");
 ComPtr<ID3D11HullShader> frustumHS;ComPtr<ID3D11DomainShader> frustumDS;ComPtr<ID3D11GeometryShader> frustumGS;
 HR(device->CreateHullShader(originalFrustumHS->GetBufferPointer(),originalFrustumHS->GetBufferSize(),nullptr,frustumHS.GetAddressOf()),"unculled reference HS");HR(device->CreateDomainShader(originalFrustumDS->GetBufferPointer(),originalFrustumDS->GetBufferSize(),nullptr,frustumDS.GetAddressOf()),"unculled reference DS");HR(device->CreateGeometryShader(originalFrustumGS->GetBufferPointer(),originalFrustumGS->GetBufferSize(),nullptr,frustumGS.GetAddressOf()),"unculled reference GS");
 D3D11_QUERY_DESC workDesc{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> workQuery;HR(device->CreateQuery(&workDesc,workQuery.GetAddressOf()),"frustum hardware work query");
 ID3D11ShaderResourceView *frustumResources[]={terrainHeight.Get(),grassClass.Get(),cellView.Get(),filteredMasks[0].Get()};context->DSSetShaderResources(11,4,frustumResources);context->GSSetShaderResources(11,4,frustumResources);
 for(unsigned projection=0;projection<3;++projection)for(const auto &frustumOffset:offsets){
  c.world[12]=frustumOffset[0];c.world[13]=frustumOffset[1];c.world[14]=frustumOffset[2];shadow.render[0]=projection==2?1.f:0.f;shadow.render[1]=projection?1.f:0.f;context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);
  auto frustumRaster=[&](bool original,D3D11_QUERY_DATA_PIPELINE_STATISTICS &work){
   context->HSSetShader(original?frustumHS.Get():hs.Get(),nullptr,0);context->DSSetShader(original?frustumDS.Get():ds.Get(),nullptr,0);context->GSSetShader(original?frustumGS.Get():newGrassRaster.Get(),nullptr,0);context->PSSetShader(original?ps.Get():compactRasterPS.Get(),nullptr,0);context->OMSetRenderTargets(6,allTargets,nullptr);clearAll();context->Begin(workQuery.Get());draw(43,normal.Get(),orm.Get());context->End(workQuery.Get());const auto pixels=readAll();HR(context->GetData(workQuery.Get(),&work,sizeof(work),0),"completed frustum pipeline statistics");return pixels;
  };
  D3D11_QUERY_DATA_PIPELINE_STATISTICS priorWork{},candidateWork{};const auto priorPixels=frustumRaster(true,priorWork),candidatePixels=frustumRaster(false,candidateWork);Check(priorPixels==candidatePixels,"all frustum/guard/shadow MRT pixels exact");Check(candidateWork.DSInvocations<=priorWork.DSInvocations,"frustum culling never increases tessellated work");
  priorDomains+=priorWork.DSInvocations;newDomains+=candidateWork.DSInvocations;if(priorWork.DSInvocations&&!candidateWork.DSInvocations)++frustumCulled;++frustumCases;
 }
 Check(frustumCulled>=12&&newDomains<priorDomains,"actual GPU discards invisible patches in color and shadow projections");
 std::ofstream frustumReceipt(directory/(sizeof(void*)==8?"frustum-terrain-x64.json":"frustum-terrain-x86.json"));frustumReceipt<<"{\"passed\":true,\"actual_GPU\":true,\"full_MRT_cases\":"<<frustumCases<<",\"fully_culled_patches\":"<<frustumCulled<<",\"prior_DS_invocations\":"<<priorDomains<<",\"candidate_DS_invocations\":"<<newDomains<<",\"pixel_exact\":true,\"visible_density_reduced\":false}\n";
 c=beforeFrustumC;
 Check(foundBlades&&terrainOpaqueDepth,"real grass geometry and opaque shadow depth exercised");
 std::ofstream terrainReceipt(directory/(sizeof(void*)==8?"compact-terrain-x64.json":"compact-terrain-x86.json"));terrainReceipt<<"{\"exclusion_oracle_values\":"<<exclusionValues<<",\"prior_domain_and_filtered_occupancy\":true,\"passed\":true,\"actual_GPU\":true,\"production_compact_GS\":true,\"stream_cases\":"<<terrainCases<<",\"vertices_compared\":"<<terrainVerticesCompared<<",\"attributes_per_vertex\":36,\"full_MRT_cases\":"<<terrainRasterCases<<",\"opaque_cutout_depth_cases\":"<<terrainDepthCases<<",\"maximum_pixel_error\":"<<terrainPixelError<<",\"blades_present\":true,\"missing_UV_reflection_fallback\":true,\"geometry_density_reduced\":false,\"game_FPS_measured\":false}\n";Check(bool(terrainReceipt),"terrain receipt written");
 context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);rawVB=vb.Get();context->IASetVertexBuffers(0,1,&rawVB,&stride,&offset);context->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);c=savedTerrainC;shadow=savedTerrainShadow;context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);
 }

 // The alpha branch must retain the same depth coverage for every comparison.
 auto shadowVertex=compile(candidate,"LegacyShadowVS","vs_5_0"),newShadow=compile(candidate,"LegacyShadowPS","ps_5_0"),oldShadow=compile(reference,"LegacyShadowPS","ps_5_0");
 ComPtr<ID3D11VertexShader> shadowVS;ComPtr<ID3D11PixelShader> shadowOld,shadowNew;HR(device->CreateVertexShader(shadowVertex->GetBufferPointer(),shadowVertex->GetBufferSize(),nullptr,shadowVS.GetAddressOf()),"shadow VS");HR(device->CreatePixelShader(oldShadow->GetBufferPointer(),oldShadow->GetBufferSize(),nullptr,shadowOld.GetAddressOf()),"old depth PS");HR(device->CreatePixelShader(newShadow->GetBufferPointer(),newShadow->GetBufferSize(),nullptr,shadowNew.GetAddressOf()),"new depth PS");
 td.Width=td.Height=32;td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> depth,depthRead;ComPtr<ID3D11DepthStencilView> dsv;HR(device->CreateTexture2D(&td,nullptr,depth.GetAddressOf()),"depth target");HR(device->CreateDepthStencilView(depth.Get(),nullptr,dsv.GetAddressOf()),"depth view");td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&td,nullptr,depthRead.GetAddressOf()),"depth readback");
 D3D11_DEPTH_STENCIL_DESC depthDesc{};depthDesc.DepthEnable=TRUE;depthDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthDesc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;ComPtr<ID3D11DepthStencilState> depthState;HR(device->CreateDepthStencilState(&depthDesc,depthState.GetAddressOf()),"depth state");context->OMSetRenderTargets(0,nullptr,dsv.Get());context->OMSetDepthStencilState(depthState.Get(),0);context->VSSetShader(shadowVS.Get(),nullptr,0);
 auto *shadowRaw=sb.Get();context->VSSetConstantBuffers(2,1,&shadowRaw);
 auto transparent=tex({128,128,128,64});unsigned shadowCases=0;bool opaqueWrites=false,cutoutRejects=false;
 for(unsigned alpha=0;alpha<=8;++alpha)for(unsigned transparentMode=0;transparentMode<2;++transparentMode){c=base;c.alphaStage[0][3]=alpha;c.bridgeFlags[1]=.5f;context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);auto *baseView=transparentMode?transparent.Get():albedo.Get();context->PSSetShaderResources(0,1,&baseView);
  auto depthPixels=[&](ID3D11PixelShader *shader){context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->PSSetShader(shader,nullptr,0);context->DrawIndexed(3,0,0);context->CopyResource(depthRead.Get(),depth.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(depthRead.Get(),0,D3D11_MAP_READ,0,&map),"alpha depth readback");std::vector<float> result;for(unsigned y=0;y<32;++y){auto *row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);result.insert(result.end(),row,row+32);}context->Unmap(depthRead.Get(),0);return result;};
  const auto a=depthPixels(shadowOld.Get()),b=depthPixels(shadowNew.Get());Check(a==b,"all alpha/opaque depth coverage exact");if(alpha==0)opaqueWrites|=*std::min_element(b.begin(),b.end())<1;if(alpha==5&&transparentMode)cutoutRejects=*std::min_element(b.begin(),b.end())==1;++shadowCases;
 }
 Check(opaqueWrites&&cutoutRejects,"opaque writes and cutout rejects actual depth");
 // Real depth values and coverage for the position-only production shader,
 // including shared geometry, both cascades, world and per-instance poses.
 auto opaqueCode=compile(candidate+OpaqueShadowShaderSuffix,"LegacyOpaqueShadowVS","vs_5_0");
 ComPtr<ID3D11ShaderReflection> opaqueReflection;HR(D3DReflect(opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize(),IID_PPV_ARGS(opaqueReflection.GetAddressOf())),"opaque VS reflection");
 D3D11_SHADER_DESC opaqueDescription{};HR(opaqueReflection->GetDesc(&opaqueDescription),"opaque VS description");Check(opaqueDescription.OutputParameters==1,"opaque depth VS exports position only");
 ComPtr<ID3D11VertexShader> opaqueVS;HR(device->CreateVertexShader(opaqueCode->GetBufferPointer(),opaqueCode->GetBufferSize(),nullptr,opaqueVS.GetAddressOf()),"actual production opaque VS");
 // The compiler retains declared but unused LegacyInput semantics. Keep the
 // compatible full native layout; reflection proves only position/instance
 // data is read, so extra declarations do not add shader computation.
 for(unsigned parameter=0;parameter<opaqueDescription.InputParameters;++parameter){D3D11_SIGNATURE_PARAMETER_DESC input{};HR(opaqueReflection->GetInputParameterDesc(parameter,&input),"opaque input signature");Check(!input.ReadWriteMask||!_stricmp(input.SemanticName,"POSITION")||input.SystemValueType==D3D_NAME_INSTANCE_ID,"opaque VS reads only position and instance ID");}
 DrawPacket policy;policy.primitive=D3DPT_TRIANGLELIST;Check(OpaqueShadowDepthPacket(policy),"solid triangles select opaque path");
 for(unsigned unsafe=0;unsafe<6;++unsafe){auto q=policy;if(unsafe==0)q.terrain=true;if(unsafe==1)q.vertexShader=1;if(unsafe==2)q.outputOverlay=true;if(unsafe==3)q.primitive=D3DPT_POINTLIST;if(unsafe==4)q.constants.geometryFlags[0]=1;if(unsafe==5)q.constants.alphaStage[0][3]=5;Check(!OpaqueShadowDepthPacket(q),"unsafe or cutout caster retains original path");}
 struct ShadowInstance{float world[16],normal[16];};ShadowInstance shadowInstances[2]{};
 D3D11_BUFFER_DESC instanceDesc{};instanceDesc.ByteWidth=sizeof(shadowInstances);instanceDesc.StructureByteStride=sizeof(ShadowInstance);instanceDesc.Usage=D3D11_USAGE_DEFAULT;instanceDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;instanceDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
 ComPtr<ID3D11Buffer> shadowInstanceBuffer;HR(device->CreateBuffer(&instanceDesc,nullptr,shadowInstanceBuffer.GetAddressOf()),"test per-instance shadow poses");D3D11_SHADER_RESOURCE_VIEW_DESC instanceViewDesc{};instanceViewDesc.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;instanceViewDesc.Buffer.NumElements=2;ComPtr<ID3D11ShaderResourceView> shadowInstanceView;HR(device->CreateShaderResourceView(shadowInstanceBuffer.Get(),&instanceViewDesc,shadowInstanceView.GetAddressOf()),"shadow instance poses view");auto *instanceSRV=shadowInstanceView.Get();context->VSSetShaderResources(15,1,&instanceSRV);
 D3D11_QUERY_DESC opaqueQueryDesc{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> opaqueQuery;HR(device->CreateQuery(&opaqueQueryDesc,opaqueQuery.GetAddressOf()),"opaque depth pipeline count");
 unsigned opaqueCases=0;std::uint64_t originalPSInvocations=0,opaquePSInvocations=0;bool opaqueNonempty=false;
 for(unsigned cascade=0;cascade<2;++cascade)for(unsigned modern=0;modern<2;++modern)for(unsigned pose=0;pose<4;++pose)for(unsigned instanced=0;instanced<2;++instanced){
  c=base;c.alphaStage[0][3]=0;c.modernMaterial[0]=float(modern);std::fill_n(c.world,16,0.f);c.world[0]=c.world[5]=c.world[10]=c.world[15]=1;
  const float angle=.21f*pose;c.world[0]=c.world[5]=std::cos(angle);c.world[1]=std::sin(angle);c.world[4]=-std::sin(angle);c.world[12]=.07f*pose;c.world[13]=-.04f*pose;c.world[14]=.2f+.1f*pose;
  for(unsigned instance=0;instance<2;++instance){std::copy_n(c.world,16,shadowInstances[instance].world);std::copy_n(c.normalWorld,16,shadowInstances[instance].normal);shadowInstances[instance].world[12]+=.15f*instance;shadowInstances[instance].world[14]+=.03f*instance;}
  context->UpdateSubresource(shadowInstanceBuffer.Get(),0,nullptr,shadowInstances,0,0);settings[0]=instanced;context->UpdateSubresource(is.Get(),0,nullptr,settings,0,0);context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
  shadow={};for(unsigned axis=0;axis<4;++axis)shadow.nearVP[axis*5]=shadow.farVP[axis*5]=1;shadow.farVP[0]=shadow.farVP[5]=.8f;shadow.farVP[10]=.5f;shadow.render[0]=float(cascade);context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);
  auto captureOpaque=[&](bool fast){context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->IASetInputLayout(layout.Get());context->VSSetShader(fast?opaqueVS.Get():shadowVS.Get(),nullptr,0);context->PSSetShader(fast?nullptr:shadowOld.Get(),nullptr,0);context->Begin(opaqueQuery.Get());context->DrawIndexedInstanced(3,instanced?2:1,0,0,0);context->End(opaqueQuery.Get());context->CopyResource(depthRead.Get(),depth.Get());D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(depthRead.Get(),0,D3D11_MAP_READ,0,&mapped),"exact opaque depth readback");std::vector<float> result;for(unsigned y=0;y<32;++y){auto *row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch);result.insert(result.end(),row,row+32);}context->Unmap(depthRead.Get(),0);D3D11_QUERY_DATA_PIPELINE_STATISTICS counted{};HR(context->GetData(opaqueQuery.Get(),&counted,sizeof(counted),0),"opaque pipeline stats complete");(fast?opaquePSInvocations:originalPSInvocations)+=counted.PSInvocations;return result;};
  const auto before=captureOpaque(false),after=captureOpaque(true);Check(before==after,"world/instanced cascade opaque depth values and coverage exact");opaqueNonempty|=*std::min_element(after.begin(),after.end())<1;++opaqueCases;
 }
 Check(opaqueNonempty&&originalPSInvocations>0&&opaquePSInvocations==0,"actual depth-only draw eliminates all opaque PS invocations");
 std::ofstream opaqueReceipt(directory/(sizeof(void*)==8?"opaque-shadow-x64.json":"opaque-shadow-x86.json"));opaqueReceipt<<"{\"passed\":true,\"actual_GPU\":true,\"full_depth_cases\":"<<opaqueCases<<",\"pixel_exact\":true,\"original_PS_invocations\":"<<originalPSInvocations<<",\"candidate_PS_invocations\":"<<opaquePSInvocations<<",\"VS_output_parameters\":"<<opaqueDescription.OutputParameters<<",\"instancing_exercised\":true,\"fallback_checks\":6,\"quality_reduced\":false}\n";Check(bool(opaqueReceipt),"opaque shadow receipt written");


 // Terrain79: reproduce double legacy light modulation, then verify the real
 // production LegacyPS is independent of old clouds and lightmap texels.
 {
 context->ClearState();c=base;c.modernMaterial[0]=2;c.coordinateStage[1][0]=1;
 shadow={};weather={};context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);context->UpdateSubresource(weatherCB.Get(),0,nullptr,&weather,0,0);
 auto *legacyConstants=cb.Get();context->VSSetConstantBuffers(0,1,&legacyConstants);context->PSSetConstantBuffers(0,1,&legacyConstants);
 auto *shadowConstants=sb.Get();context->PSSetConstantBuffers(2,1,&shadowConstants);auto *weatherConstants=weatherCB.Get();context->PSSetConstantBuffers(7,1,&weatherConstants);
 context->PSSetConstantBuffers(13,1,&controlRaw);UINT noInstances[4]{};context->UpdateSubresource(is.Get(),0,nullptr,noInstances,0,0);
 auto *instanceConstants=is.Get();context->VSSetConstantBuffers(4,1,&instanceConstants);
 context->OMSetRenderTargets(6,allTargets,nullptr);context->RSSetViewports(1,&viewport);context->RSSetState(raster.Get());
 context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetIndexBuffer(localIB.Get(),DXGI_FORMAT_R32_UINT,0);context->VSSetShader(vs.Get(),nullptr,0);
 ID3D11SamplerState *terrainSamplers[4]={sampler.Get(),sampler.Get(),sampler.Get(),sampler.Get()};context->PSSetSamplers(0,4,terrainSamplers);environment.Bind(context.Get());
 std::array<D3DRESOURCETYPE,8> terrainTypes{};for(unsigned i=0;i<4;++i)terrainTypes[i]=D3DRTYPE_TEXTURE;
 const std::string terrainAssembly="ps_1_1\ntex t0\ntex t1\ntex t2\ntex t3\nlrp r0, v0.w, t1, t0\nmul r0, r0, v0\nmul r0, r0, t2\nmul r0, r0, t3\n";
 const auto terrainSource=BuildProgramSource(LegacyShaderSource,false,true,{},terrainAssembly,terrainTypes,sourceError);
 const auto terrainReference=BuildProgramSource(oldSource.c_str(),false,true,{},terrainAssembly,terrainTypes,sourceError);
 auto newTerrain=compile(terrainSource,"LegacyPS","ps_5_0"),oldTerrain=compile(terrainReference,"LegacyPS","ps_5_0");
 ComPtr<ID3D11PixelShader> terrainPS,oldTerrainPS;HR(device->CreatePixelShader(newTerrain->GetBufferPointer(),newTerrain->GetBufferSize(),nullptr,terrainPS.GetAddressOf()),"new production terrain PS");HR(device->CreatePixelShader(oldTerrain->GetBufferPointer(),oldTerrain->GetBufferSize(),nullptr,oldTerrainPS.GetAddressOf()),"old production terrain PS reproducer");
 const auto baseProbeSource=terrainSource+"\nfloat4 Terrain79BasePS(LegacyOutput input):SV_TARGET{return float4(TerrainBaseAlbedo(input,(uint)ModernMaterial.w),1);}\n";
 auto baseProbe=compile(baseProbeSource,"Terrain79BasePS","ps_5_0");ComPtr<ID3D11PixelShader> baseProbePS;HR(device->CreatePixelShader(baseProbe->GetBufferPointer(),baseProbe->GetBufferSize(),nullptr,baseProbePS.GetAddressOf()),"production base albedo probe");
 auto secondColor=tex({48,160,80,255}),oldLight=tex({55,55,55,255}),whiteLight=tex({255,255,255,255});
 unsigned albedoCases=0,lightCases=0,normalCases=0;float lightLeak=0,maxBaseError=0,maxNormalError=0;
 auto linear=[](float x){x/=255;return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);};
 for(float alpha:{0.f,.35f,1.f})for(bool blend:{false,true}){
  FixedVertex terrainInput[3];std::memcpy(terrainInput,verts,sizeof(terrainInput));
  for(auto &v:terrainInput){v.diffuse[3]=alpha;v.uv[1][0]=-v.uv[0][1];v.uv[1][1]=v.uv[0][0];}
  auto terrainVB=buffer(sizeof(terrainInput),D3D11_BIND_VERTEX_BUFFER,terrainInput);auto *terrainVBPointer=terrainVB.Get();UINT zero=0;context->IASetVertexBuffers(0,1,&terrainVBPointer,&stride,&zero);
  terrainNativeExtras={secondColor.Get(),oldLight.Get(),oldLight.Get()};const unsigned flags=8u|(blend?16u:0u);
  context->PSSetShader(baseProbePS.Get(),nullptr,0);clearAll();const auto observedBase=draw(flags,normal.Get(),orm.Get());
  for(unsigned channel=0;channel<3;++channel){const unsigned second[]={48,160,80};const float expected=linear(128)*(1-(blend?alpha:0))+linear(static_cast<float>(second[channel]))*(blend?alpha:0);maxBaseError=std::max(maxBaseError,std::abs(expected-observedBase[channel]));Check(std::abs(expected-observedBase[channel])<2e-6f,"authored base color blends in linear light without old illumination");}++albedoCases;
  context->PSSetShader(terrainPS.Get(),nullptr,0);clearAll();draw(flags,normal.Get(),orm.Get());const auto darkLegacy=readAll();
  terrainNativeExtras[1]=terrainNativeExtras[2]=whiteLight.Get();clearAll();draw(flags,normal.Get(),orm.Get());const auto brightLegacy=readAll();Check(darkLegacy==brightLegacy,"all six modern terrain MRTs ignore obsolete cloud/lightmap modulation");++lightCases;
  context->PSSetShader(oldTerrainPS.Get(),nullptr,0);clearAll();draw(flags,normal.Get(),orm.Get());const auto oldBright=readAll();terrainNativeExtras[1]=terrainNativeExtras[2]=oldLight.Get();clearAll();draw(flags,normal.Get(),orm.Get());const auto oldDark=readAll();for(unsigned i=0;i<oldDark.size();++i)lightLeak=std::max(lightLeak,std::abs(oldDark[i]-oldBright[i]));
  context->PSSetShader(terrainPS.Get(),nullptr,0);clearAll();draw(8|16|1,tilted.Get(),orm.Get());const auto normalPixels=readAll();
  // Source map (1,1/255,1/255); UV1=(-UV0.y,UV0.x) rotates its +X
  // direction to -world Y. Normalize each chart, then blend world normals.
  const float tiny=1.f/255,scaleNormal=1/std::sqrt(1+2*tiny*tiny);
  float expected[3]={(1-alpha+alpha*tiny)*scaleNormal,((1-alpha)*tiny-alpha)*scaleNormal,tiny*scaleNormal};const float lengthNormal=std::sqrt(expected[0]*expected[0]+expected[1]*expected[1]+expected[2]*expected[2]);
  for(unsigned channel=0;channel<3;++channel){expected[channel]/=lengthNormal;const auto actual=normalPixels[3*32*32*4+(16*32+16)*4+channel];maxNormalError=std::max(maxNormalError,std::abs(actual-expected[channel]));if(std::abs(actual-expected[channel])>=2e-5f)std::cerr<<"normal alpha="<<alpha<<" channel="<<channel<<" actual="<<actual<<" expected="<<expected[channel]<<"\n";Check(std::abs(actual-expected[channel])<2e-5f,"rotated chart normal blends in world space with original orientation");}++normalCases;
 }
 Check(lightLeak>.01f,"original double-light terrain failure actually reproduced on GPU");
 std::ofstream receipt(directory/(sizeof(void*)==8?"terrain-material79-x64.json":"terrain-material79-x86.json"));receipt<<"{\"passed\":true,\"actual_production_GPU\":true,\"linear_albedo_cases\":"<<albedoCases<<",\"light_independence_full_MRT_cases\":"<<lightCases<<",\"rotated_normal_cases\":"<<normalCases<<",\"original_light_leak_reproduced\":"<<lightLeak<<",\"maximum_albedo_error\":"<<maxBaseError<<",\"maximum_normal_error\":"<<maxNormalError<<",\"nonterrain_full_MRT_cases\":"<<cases<<",\"quality_reduced\":false}\n";Check(bool(receipt),"terrain material receipt written");terrainNativeExtras={};
 }

 std::cout<<"{\"passed\":true,\"actual_GPU\":true,\"production_LegacyPS\":true,\"full_MRT_cases\":"<<cases<<",\"RGBA_values_compared\":"<<values<<",\"maximum_absolute_error\":"<<maxError<<",\"opaque_alpha_depth_cases\":"<<shadowCases<<",\"checks\":"<<checks<<",\"game_FPS_measured\":false}\n";return 0;
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}

