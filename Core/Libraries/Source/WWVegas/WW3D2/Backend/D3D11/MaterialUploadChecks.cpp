// SPDX-License-Identifier: GPL-3.0-or-later
#include "MaterialStreaming.h"
#include <iostream>
#include <stdexcept>
#include <chrono>
using namespace GenZH::Modern;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,pixels=0;
void Check(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
std::vector<unsigned char> Read(const std::filesystem::path &p){
 std::ifstream f(p,std::ios::binary|std::ios::ate);Check(bool(f),"fixture source missing");
 const auto n=f.tellg();Check(n>=148,"fixture DDS truncated");std::vector<unsigned char> b(static_cast<std::size_t>(n));
 f.seekg(0);Check(bool(f.read(reinterpret_cast<char*>(b.data()),n)),"fixture read failed");return b;
}
std::vector<unsigned char> DDS(unsigned w,unsigned h,DXGI_FORMAT format){
 unsigned mips=1;for(auto n=std::max(w,h);n>1;n>>=1)++mips;
 const unsigned block=format==DXGI_FORMAT_BC4_UNORM?8u:16u;std::size_t bytes=148;
 for(unsigned n=0;n<mips;++n)bytes+=std::size_t(std::max(1u,(std::max(1u,w>>n)+3)/4))*std::max(1u,(std::max(1u,h>>n)+3)/4)*block;
 std::vector<unsigned char> b(bytes);
 auto set=[&](unsigned at,unsigned v){std::memcpy(b.data()+at,&v,4);};
 set(0,0x20534444);set(4,124);set(12,h);set(16,w);set(28,mips);set(76,32);set(84,0x30315844);set(128,format);set(132,3);set(140,1);return b;
}
void Exact(ID3D11Device *d,ID3D11DeviceContext *c,ID3D11ShaderResourceView *v,const ImageRGBA &image){
 Check(v!=nullptr,"authored map missing");ComPtr<ID3D11Resource> resource;v->GetResource(&resource);
 ComPtr<ID3D11Texture2D> t;Check(SUCCEEDED(resource.As(&t)),"authored map not 2D");D3D11_TEXTURE2D_DESC desc{};t->GetDesc(&desc);
 Check(desc.Width==image.width&&desc.Height==image.height&&desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM,"authored dimensions/format changed");
 desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> staging;Check(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&staging)),"GPU readback creation failed");c->CopyResource(staging.Get(),t.Get());
 D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"GPU readback failed");
 bool exact=true;for(unsigned y=0;y<image.height;++y)exact=exact&&std::memcmp(static_cast<const unsigned char*>(map.pData)+std::size_t(y)*map.RowPitch,image.pixels.data()+std::size_t(y)*image.width*4,std::size_t(image.width)*4)==0;
 c->Unmap(staging.Get(),0);Check(exact,"authored RGBA including alpha was changed");++pixels;
}
}
int wmain(int argc,wchar_t **argv){try{
 Check(argc==2,"materials directory required");const std::filesystem::path root(argv[1]);
 std::string error;PreparedDDS decoded;
 for(unsigned w:{1u,2u,3u,4u,5u,8u,12u})for(unsigned h:{1u,2u,3u,4u,6u,8u})for(auto format:{DXGI_FORMAT_BC4_UNORM,DXGI_FORMAT_BC7_UNORM,DXGI_FORMAT_BC7_UNORM_SRGB}){
  const bool expected=(w%4==0&&h%4==0);Check(DecodePreparedDDS(DDS(w,h,format),decoded,error)==expected,"BC top-level/mip validation mismatch");
  if(expected)Check(decoded.levels.back().SysMemSlicePitch==(format==DXGI_FORMAT_BC4_UNORM?8u:16u),"small lower mip rejected or damaged");
  else Check(decoded.levels.empty()&&!error.empty(),"invalid DDS retained a payload");
 }
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{},wanted=D3D_FEATURE_LEVEL_11_0;
 Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&wanted,1,D3D11_SDK_VERSION,&device,&level,&context)),"hardware D3D11 device creation failed");
 unsigned reproduced=0;
 for(const wchar_t *stem:{L"zhr_dz37_zh36_white",L"zhr_dz39_zh39_glass"}){
  const auto source=root/(std::wstring(stem)+L"_albedo.tga");const auto dds=root/L"runtime-dds"/(std::wstring(stem)+L"_albedo.dds");
  Check(IsPreparedCurrent(source,dds),"actual failing DDS cache stamp differs");auto raw=Read(dds);
  Check(!DecodePreparedDDS(raw,decoded,error),"actual unusable DDS accepted");
  D3D11_TEXTURE2D_DESC old{};old.Width=old.Height=old.MipLevels=old.ArraySize=old.SampleDesc.Count=1;old.Format=DXGI_FORMAT_BC7_UNORM;old.Usage=D3D11_USAGE_IMMUTABLE;old.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{raw.data()+148,16,16};ComPtr<ID3D11Texture2D> rejected;
  Check(device->CreateTexture2D(&old,&data,&rejected)==E_INVALIDARG,"original GPU failure was not reproduced");++reproduced;
  std::atomic<bool> cancel{false};MaterialRequest request;request.root=root;request.stem=stem;auto prepared=std::make_unique<PreparedMaterial>();
  Check(PrepareMaterial(request,*prepared,cancel)&&!prepared->fromDDS&&!prepared->finished&&prepared->error.empty(),"original authored source was not prepared");
  ImageRGBA original;Check(LoadTGA(source,original,error),"authored TGA read failed");MaterialUpload upload(std::move(prepared));
  for(unsigned n=0;n<20&&!upload.Ready();++n)Check(SUCCEEDED(upload.Pump(device.Get(),context.Get())),"authored GPU upload failed");
  Check(upload.Ready(),"authored upload did not finish");Exact(device.Get(),context.Get(),upload.View().Get(),original);
  MaterialStreaming streaming;const MaterialViews *views=nullptr;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(!views&&std::chrono::steady_clock::now()<deadline){streaming.BeginFrame();views=streaming.FindOrRequest(root,stem);Check(SUCCEEDED(streaming.Pump(device.Get(),context.Get())),"material streaming API failed");if(!views)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  Check(views&&streaming.LastError().empty()&&streaming.Idle(),"complete authored material failed to become ready");
  const MaterialLookup lookup(root,stem);Check(lookup.Valid(),"canonical material lookup rejected valid name");
  for(unsigned n=0;n<10000;++n)Check(streaming.FindOrRequest(lookup)==views,"canonical lookup lost complete material or async readiness");
  Check(!MaterialLookup(root,L"../escape").Valid()&&!MaterialLookup(root,L"..").Valid()&&!MaterialLookup({},stem).Valid(),"unsafe material lookup accepted");
  const wchar_t *suffixes[]={L"_albedo.tga",L"_normal.tga",L"_orm.tga",L"_height.tga",L"_emissive.tga",L"_wear_albedo.tga",L"_wear_normal.tga",L"_wear_orm.tga"};
  for(unsigned i=0;i<8;++i){const auto file=root/(std::wstring(stem)+suffixes[i]);if(std::filesystem::exists(file)){Check(LoadTGA(file,original,error),"complete map source read failed");Exact(device.Get(),context.Get(),views->maps[i].Get(),original);}else Check(!views->maps[i],"unexpected map substituted");}
  streaming.Reset();Check(streaming.Entries()==0,"reset retained lookup-owned GPU maps");
  Check(!streaming.FindOrRequest(lookup)&&streaming.Queued()==1,"canonical lookup did not re-request after reset");
  Check(!streaming.FindOrRequest(lookup)&&streaming.Queued()==1,"pending lookup duplicated a queued material");
 }
 // Keep valid BC7 chains, including their 2x2 and 1x1 mips, on the GPU path.
 const auto valid=root/L"runtime-dds/Z26Armor_normal.dds";Check(DecodePreparedDDS(Read(valid),decoded,error),"existing valid BC7 rejected");
 D3D11_TEXTURE2D_DESC desc{};desc.Width=decoded.width;desc.Height=decoded.height;desc.MipLevels=decoded.mips;desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=decoded.format;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture2D> retained;Check(SUCCEEDED(device->CreateTexture2D(&desc,decoded.levels.data(),&retained)),"valid full mip chain GPU upload failed");
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"pointer_bits\":"<<sizeof(void*)*8<<",\"driver\":\"hardware\",\"original_GPU_failures_reproduced\":"<<reproduced<<",\"RGBA_GPU_pixel_comparisons\":"<<pixels<<",\"complete_materials_ready\":2,\"valid_BC7_full_mips_retained\":true,\"quality_reduced\":false}\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';std::cout<<"{\"passed\":false}\n";return 1;}}
