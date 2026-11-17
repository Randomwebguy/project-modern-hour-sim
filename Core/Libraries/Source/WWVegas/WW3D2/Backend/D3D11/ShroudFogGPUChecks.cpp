// SPDX-License-Identifier: GPL-3.0-or-later
// perf92: offline checks and shader previews of the fog of war pass on the
// actual GPU, with the production AtmosphereGPU and Atmosphere.hlsl. A CPU ray
// cast of a synthetic RTS scene (relief, structures inside visible, explored
// and unexplored cells) supplies scene colour and hardware-style depth.
//  1. The closed-form world->uv equals inv(view)*offset*scale on camera space.
//  2. The flat mode equals scene * bilinear(native shroud) on every surface.
//  3. Volume mode (semi-transparent war fog): visible-only rays leave the pixel
//     bit-identical; every fogged surface keeps at least 10% of the scene;
//     unexplored surfaces at most 60%, out-of-sight ones at most 85%; no NaN/Inf.
//  4. Previews (raw float RGBA) for several lights/weathers, and GPU time.
//  5. perf93: the fixture uses Zero Hour data (FogAlpha = ClearAlpha: the native
//     texture only knows unexplored cells) and the per-cell vision for the fog;
//     the vision fade follows the exponential rates and converges to the cells.
//  6. SetD3D11ShroudMapping writes (and bumps the revision) only on change;
//     only the exact native shroud pass state is classified as replaceable.
#include "AtmosphereGPU.h"
#include "ShroudMapping.h"
#include "DeviceBridge.h"
#include "AtmosphereDrawPolicy.h"
#include <array>
#include <cstring>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace GenZH::Modern;using namespace DirectX;
static unsigned checks=0;
static void Check(bool ok,const char *what){++checks;if(!ok)throw std::runtime_error(what);}
static void HR(HRESULT hr,const char *what){if(FAILED(hr)){char text[256];std::snprintf(text,sizeof(text),"%s (0x%08lx)",what,static_cast<unsigned long>(hr));throw std::runtime_error(text);}++checks;}
struct Box {float x0,y0,x1,y1,top;float albedo[3];};
static const Box boxes[]={{40,20,85,60,46,{.42f,.40f,.37f}},{-250,140,-205,175,62,{.38f,.36f,.33f}},{185,-95,225,-55,38,{.45f,.30f,.25f}},{-40,-175,-10,-140,24,{.30f,.33f,.36f}},{90,230,140,280,55,{.40f,.39f,.36f}}};
static float Ground(float x,float y){return 18*std::sin(x/97.f)*std::cos(y/131.f)+9*std::sin((x+y)/53.f);}
struct Hit {bool hit=false;float t=0;float n[3]{0,0,1};float albedo[3]{};};
static Hit Cast(const XMFLOAT3 &o,const XMFLOAT3 &d,float limit){
 Hit best;best.t=limit;
 for(const auto &b:boxes){ // slab test against the box down to the lowest ground
  float t0=0,t1=limit;int axis=-1,sign=0;const float lo[3]={b.x0,b.y0,-40},hi[3]={b.x1,b.y1,b.top},oo[3]={o.x,o.y,o.z},dd[3]={d.x,d.y,d.z};bool ok=true;
  for(int a=0;a<3&&ok;++a){
   if(std::abs(dd[a])<1e-8f){ok=oo[a]>=lo[a]&&oo[a]<=hi[a];continue;}
   float ta=(lo[a]-oo[a])/dd[a],tb=(hi[a]-oo[a])/dd[a];int s=-1;if(ta>tb){std::swap(ta,tb);s=1;}
   if(ta>t0){t0=ta;axis=a;sign=s;}t1=std::min(t1,tb);ok=t0<=t1;
  }
  if(ok&&axis>=0&&t0<best.t){const float z=o.z+d.z*t0;if(z>=Ground(o.x+d.x*t0,o.y+d.y*t0)){best.hit=true;best.t=t0;best.n[0]=best.n[1]=best.n[2]=0;best.n[axis]=float(sign);std::copy_n(b.albedo,3,best.albedo);}}
 }
 // Height field between z=+30 and z=-30, then bisection.
 if(d.z<-1e-5f){
  float t=std::max(0.f,(o.z-30)/-d.z),end=std::min(best.t,(o.z+30)/-d.z);float previous=t;
  auto f=[&](float s){return o.z+d.z*s-Ground(o.x+d.x*s,o.y+d.y*s);};
  for(;t<end;t+=.5f){if(f(t)<0){float a=previous,b=t;for(int i=0;i<24;++i){float m=(a+b)*.5f;(f(m)<0?b:a)=m;}
    const float x=o.x+d.x*b,y=o.y+d.y*b,e=.25f;const float gx=(Ground(x+e,y)-Ground(x-e,y))/(2*e),gy=(Ground(x,y+e)-Ground(x,y-e))/(2*e);
    const float l=std::sqrt(gx*gx+gy*gy+1);best.hit=true;best.t=b;best.n[0]=-gx/l;best.n[1]=-gy/l;best.n[2]=1/l;
    const float v=.5f+.5f*std::sin(x*.071f+std::sin(y*.043f)*2.1f)*std::cos(y*.057f);
    const float grass[3]={.13f,.17f,.07f},soil[3]={.27f,.21f,.13f};for(int c=0;c<3;++c)best.albedo[c]=grass[c]+(soil[c]-grass[c])*v;break;}
   previous=t;}
 }
 return best;
}
// Private-data store of a texture, as the native resource keeps it.
struct StubTexture final:IDirect3DBaseTexture8 {
 std::vector<char> bytes;GUID key{};unsigned writes=0;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void **out)override{if(out)*out=nullptr;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return 1;}
 ULONG STDMETHODCALLTYPE Release()override{return 1;}
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,const void *data,DWORD size,DWORD)override{key=guid;bytes.assign(static_cast<const char*>(data),static_cast<const char*>(data)+size);++writes;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,void *data,DWORD *size)override{
  if(bytes.empty()||!IsEqualGUID(guid,key))return D3DERR_NOTFOUND;if(!size)return E_POINTER;
  if(!data||*size<bytes.size()){*size=static_cast<DWORD>(bytes.size());return data?D3DERR_MOREDATA:S_OK;}
  std::memcpy(data,bytes.data(),bytes.size());*size=static_cast<DWORD>(bytes.size());return S_OK;}
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID)override{bytes.clear();return S_OK;}
 DWORD STDMETHODCALLTYPE SetPriority(DWORD)override{return 0;}
 DWORD STDMETHODCALLTYPE GetPriority()override{return 0;}
 void STDMETHODCALLTYPE PreLoad()override{}
 D3DRESOURCETYPE STDMETHODCALLTYPE GetType()override{return D3DRTYPE_TEXTURE;}
 DWORD STDMETHODCALLTYPE SetLOD(DWORD)override{return 0;}
 DWORD STDMETHODCALLTYPE GetLOD()override{return 0;}
 DWORD STDMETHODCALLTYPE GetLevelCount()override{return 1;}
};
static void RegistrationChecks(){
 StubTexture texture;D3D11ShroudMapping m;m.cellWidth=m.cellHeight=20;m.textureWidth=m.textureHeight=64;m.cellsX=m.cellsY=40;m.fogLevel=127/255.f;
 const auto before=GetD3D11ShroudRevision();SetD3D11ShroudMapping(&texture,m);
 Check(texture.writes==1&&GetD3D11ShroudRevision()==before+1,"first mapping written");
 for(int i=0;i<5;++i)SetD3D11ShroudMapping(&texture,m);
 Check(texture.writes==1&&GetD3D11ShroudRevision()==before+1,"identical mapping not rewritten");
 D3D11ShroudMapping read;DWORD size=sizeof(read);Check(SUCCEEDED(texture.GetPrivateData(D3D11ShroudMappingGUID,&read,&size))&&size==sizeof(read)&&!std::memcmp(&read,&m,sizeof(m)),"mapping round trip");
 m.fogLevel=100/255.f;SetD3D11ShroudMapping(&texture,m);Check(texture.writes==2&&GetD3D11ShroudRevision()==before+2,"changed mapping written");
 SetD3D11ShroudMapping(nullptr,m);Check(GetD3D11ShroudRevision()==before+2,"no texture, no write");
 std::array<DWORD,256> r{};r[D3DRS_ZENABLE]=1;r[D3DRS_ZFUNC]=D3DCMP_EQUAL;r[D3DRS_ALPHABLENDENABLE]=1;r[D3DRS_SRCBLEND]=D3DBLEND_ZERO;r[D3DRS_DESTBLEND]=D3DBLEND_SRCCOLOR;
 Check(NativeShroudPassStates(r,false,false),"native shroud pass recognised");
 Check(!NativeShroudPassStates(r,true,false)&&!NativeShroudPassStates(r,false,true),"programmable passes kept");
 const std::pair<D3DRENDERSTATETYPE,DWORD> changes[]={{D3DRS_ZENABLE,0},{D3DRS_ZFUNC,D3DCMP_LESSEQUAL},{D3DRS_ALPHABLENDENABLE,0},{D3DRS_SRCBLEND,D3DBLEND_SRCALPHA},{D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA},{D3DRS_DESTBLEND,D3DBLEND_ONE}};
 for(const auto &change:changes){auto s=r;s[change.first]=change.second;Check(!NativeShroudPassStates(s,false,false),"other passes kept");}
 // perf94: HUD-like draws (pretransformed or orthographic) get the world passes beneath them.
 struct Constants {float geometryFlags[4]{};float projection[16]{};};struct Packet {bool terrain=false,outputOverlay=false;Constants constants;};
 Packet world;world.constants.projection[0]=world.constants.projection[5]=1;world.constants.projection[10]=1;world.constants.projection[11]=1;
 Check(!ScreenSpacePacket(world),"perspective world draw is not screen space");
 Packet ortho=world;ortho.constants.projection[11]=0;ortho.constants.projection[15]=1;Check(ScreenSpacePacket(ortho),"orthographic draw is screen space");
 Packet rhw=world;rhw.constants.geometryFlags[0]=1;Check(ScreenSpacePacket(rhw),"pretransformed draw is screen space");
 Packet terrain=ortho;terrain.terrain=true;Check(!ScreenSpacePacket(terrain),"terrain never screen space");
 Packet overlay=rhw;overlay.outputOverlay=true;Check(!ScreenSpacePacket(overlay),"output overlays are drawn after the frame anyway");
 D3D11ShroudMapping bad=m;bad.cellsX=63;Check(!ValidShroudMapping(bad),"texture smaller than cells+border rejected");
 bad=m;bad.cellWidth=0;Check(!ValidShroudMapping(bad),"zero cell rejected");bad=m;bad.version=2;Check(!ValidShroudMapping(bad),"unknown version rejected");
}
struct Light {const char *name;WeatherMode weather;float sun[3],color[3],ambient[3];};
struct Image {UINT width=0,height=0;std::vector<float> rgba;};
static void SaveRaw(const std::filesystem::path &path,const Image &image){
 std::ofstream f(path,std::ios::binary);const std::uint32_t header[2]={image.width,image.height};f.write(reinterpret_cast<const char*>(header),8);
 f.write(reinterpret_cast<const char*>(image.rgba.data()),std::streamsize(image.rgba.size()*sizeof(float)));
}
int main(int argc,char **argv){try{
 Check(argc==2,"usage: shroud-fogchecks output-directory");const std::filesystem::path out=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(out);
 SetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",(out/L"profile").c_str());std::filesystem::create_directories(out/L"profile");
 RegistrationChecks();
 // 1. Closed form against the native texture-matrix composition.
 D3D11ShroudMapping mapping;mapping.cellWidth=20;mapping.cellHeight=20;mapping.originX=0;mapping.originY=0;mapping.cellsX=40;mapping.cellsY=40;mapping.textureWidth=64;mapping.textureHeight=64;
 mapping.clearLevel=1;mapping.fogLevel=127/255.f;mapping.shroudLevel=0;
 {std::mt19937 rng(92);std::uniform_real_distribution<float> u(-500,500);double worst=0;
  for(int i=0;i<2000;++i){
   const XMVECTOR eye=XMVectorSet(u(rng),u(rng),200+std::abs(u(rng)),1),at=XMVectorSet(u(rng),u(rng),0,1);const XMMATRIX view=XMMatrixLookAtLH(eye,at,XMVectorSet(0,0,1,0));
   const XMMATRIX texture=XMMatrixInverse(nullptr,view)*XMMatrixTranslation(mapping.cellWidth-mapping.originX,mapping.cellHeight-mapping.originY,0)*XMMatrixScaling(1/(mapping.cellWidth*mapping.textureWidth),1/(mapping.cellHeight*mapping.textureHeight),1);
   const XMVECTOR p=XMVectorSet(u(rng),u(rng),u(rng)*.1f,1);XMFLOAT4 t;XMStoreFloat4(&t,XMVector4Transform(XMVector4Transform(p,view),texture));
   float su,sv;ShroudUV(mapping,XMVectorGetX(p),XMVectorGetY(p),su,sv);worst=std::max(worst,double(std::max(std::abs(t.x-su),std::abs(t.y-sv))));
  }
  Check(worst<2e-5,"closed-form uv equals inv(view)*offset*scale");}
 Check(ValidShroudMapping(mapping),"valid mapping");
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL fl{};const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};
 HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,device.GetAddressOf(),&fl,context.GetAddressOf()),"hardware device");
 const UINT width=1920,height=1080;
 // RTS camera: 52 degrees down, looking north over the scene centre.
 const float pitch=XMConvertToRadians(52),distance=520;const XMVECTOR target=XMVectorSet(0,30,0,1);
 const XMVECTOR eye=XMVectorAdd(target,XMVectorSet(0,-distance*std::cos(pitch),distance*std::sin(pitch),0));
 const XMMATRIX view=XMMatrixLookAtLH(eye,target,XMVectorSet(0,0,1,0)),projection=XMMatrixPerspectiveFovLH(XMConvertToRadians(45),float(width)/height,5,3000);
 const XMMATRIX viewProjection=view*projection,inverse=XMMatrixInverse(nullptr,viewProjection);
 // Vision: visible disc at the centre, out-of-sight ring and corridor, unexplored
 // elsewhere. As in Zero Hour (FogAlpha = ClearAlpha) the native shroud texture
 // shows every explored cell clear; only the vision distinguishes out of sight.
 mapping.fogLevel=1;
 std::vector<float> level(std::size_t(mapping.cellsX)*mapping.cellsY);std::vector<std::uint8_t> vision(level.size());
 for(UINT y=0;y<mapping.cellsY;++y)for(UINT x=0;x<mapping.cellsX;++x){
  // Map is centred: cell (20,20) holds world (0,0).
  const float wx=(x+.5f)*mapping.cellWidth-400,wy=(y+.5f)*mapping.cellHeight-400,r=std::sqrt(wx*wx+(wy-20)*(wy-20));
  const std::size_t i=std::size_t(y)*mapping.cellsX+x;vision[i]=r<150?0:(r<260||(wx>120&&wx<330&&wy>-140&&wy<40))?128:255;level[i]=vision[i]==255?0.f:1.f;}
 mapping.originX=-400;mapping.originY=-400; // draw origin: the map's lower-left corner in world space
 std::vector<std::uint32_t> texels(std::size_t(mapping.textureWidth)*mapping.textureHeight,0xff000000u); // border level: unexplored
 for(UINT y=0;y<mapping.cellsY;++y)for(UINT x=0;x<mapping.cellsX;++x){const auto v=static_cast<std::uint32_t>(std::lround(level[std::size_t(y)*mapping.cellsX+x]*255));
  texels[std::size_t(y+1)*mapping.textureWidth+x+1]=0xff000000u|(v<<16)|(v<<8)|v;}
 // Bilinear vision cover as the vision texture holds it (border unexplored).
 auto cpuCover=[&](float wx,float wy){float u,v;ShroudUV(mapping,wx,wy,u,v);
  const float fx=u*mapping.textureWidth-.5f,fy=v*mapping.textureHeight-.5f;const int x0=int(std::floor(fx)),y0=int(std::floor(fy));const float ax=fx-x0,ay=fy-y0;
  auto at=[&](int x,int y){x=std::clamp(x,0,int(mapping.textureWidth)-1)-1;y=std::clamp(y,0,int(mapping.textureHeight)-1)-1;
   return x<0||y<0||x>=int(mapping.cellsX)||y>=int(mapping.cellsY)?1.f:vision[std::size_t(y)*mapping.cellsX+x]/255.f;};
  return (at(x0,y0)*(1-ax)+at(x0+1,y0)*ax)*(1-ay)+(at(x0,y0+1)*(1-ax)+at(x0+1,y0+1)*ax)*ay;};
 auto cpuSample=[&](float wx,float wy){float u,v;ShroudUV(mapping,wx,wy,u,v);
  const float fx=u*mapping.textureWidth-.5f,fy=v*mapping.textureHeight-.5f;const int x0=int(std::floor(fx)),y0=int(std::floor(fy));const float ax=fx-x0,ay=fy-y0;
  auto at=[&](int x,int y){x=std::clamp(x,0,int(mapping.textureWidth)-1);y=std::clamp(y,0,int(mapping.textureHeight)-1);return float(texels[std::size_t(y)*mapping.textureWidth+x]&255)/255.f;};
  return (at(x0,y0)*(1-ax)+at(x0+1,y0)*ax)*(1-ay)+(at(x0,y0+1)*(1-ax)+at(x0+1,y0+1)*ax)*ay;};
 // CPU ray cast: depth, world position and albedo/normal for every pixel.
 std::vector<float> depth(std::size_t(width)*height,1.f);std::vector<Hit> hits(depth.size());std::vector<XMFLOAT3> world(depth.size());
 {std::vector<std::thread> workers;const unsigned count=std::max(2u,std::thread::hardware_concurrency());
  for(unsigned w=0;w<count;++w)workers.emplace_back([&,w]{for(UINT y=w;y<height;y+=count)for(UINT x=0;x<width;++x){
   const float nx=(x+.5f)/width*2-1,ny=1-(y+.5f)/height*2;XMFLOAT3 a,b;XMStoreFloat3(&a,XMVector3TransformCoord(XMVectorSet(nx,ny,0,1),inverse));XMStoreFloat3(&b,XMVector3TransformCoord(XMVectorSet(nx,ny,1,1),inverse));
   XMFLOAT3 d{b.x-a.x,b.y-a.y,b.z-a.z};const float l=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);d={d.x/l,d.y/l,d.z/l};
   const auto hit=Cast(a,d,l);const std::size_t i=std::size_t(y)*width+x;hits[i]=hit;if(!hit.hit)continue;
   const XMFLOAT3 p{a.x+d.x*hit.t,a.y+d.y*hit.t,a.z+d.z*hit.t};world[i]=p;XMFLOAT4 clip;XMStoreFloat4(&clip,XMVector4Transform(XMVectorSet(p.x,p.y,p.z,1),viewProjection));depth[i]=clip.z/clip.w;}});
  for(auto &t:workers)t.join();}
 auto texture2D=[&](UINT w,UINT h,DXGI_FORMAT format,UINT bind,const void *data,UINT rowPitch,ComPtr<ID3D11Texture2D> &texture){D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;desc.BindFlags=bind;
  D3D11_SUBRESOURCE_DATA init{data,rowPitch,0};HR(device->CreateTexture2D(&desc,data?&init:nullptr,texture.GetAddressOf()),"texture");};
 ComPtr<ID3D11Texture2D> depthTexture,shroudTexture,sceneTexture,staging;ComPtr<ID3D11ShaderResourceView> depthView,shroudView;ComPtr<ID3D11RenderTargetView> sceneTarget;
 texture2D(width,height,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,depth.data(),width*4,depthTexture);HR(device->CreateShaderResourceView(depthTexture.Get(),nullptr,depthView.GetAddressOf()),"depth view");
 // As CompatibilityRenderer::GetTexture uploads it: RGBA8, full generated mip chain.
 {D3D11_TEXTURE2D_DESC desc{};desc.Width=mapping.textureWidth;desc.Height=mapping.textureHeight;desc.MipLevels=0;desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;HR(device->CreateTexture2D(&desc,nullptr,shroudTexture.GetAddressOf()),"shroud texture");
  HR(device->CreateShaderResourceView(shroudTexture.Get(),nullptr,shroudView.GetAddressOf()),"shroud view");
  context->UpdateSubresource(shroudTexture.Get(),0,nullptr,texels.data(),mapping.textureWidth*4,0);context->GenerateMips(shroudView.Get());}
 // Cells whose 7x7 neighbourhood is entirely visible: the blurred shape cover is zero there.
 std::vector<std::uint8_t> clearCell(std::size_t(mapping.cellsX)*mapping.cellsY,0);
 for(int y=0;y<int(mapping.cellsY);++y)for(int x=0;x<int(mapping.cellsX);++x){bool clear=true;
  for(int oy=-3;oy<=3&&clear;++oy)for(int ox=-3;ox<=3&&clear;++ox){const int cx=x+ox,cy=y+oy;clear=cx>=0&&cy>=0&&cx<int(mapping.cellsX)&&cy<int(mapping.cellsY)&&vision[std::size_t(cy)*mapping.cellsX+cx]==0;}
  clearCell[std::size_t(y)*mapping.cellsX+x]=clear;}
 auto clearAt=[&](float wx,float wy){const int cx=int(std::floor((wx-mapping.originX)/mapping.cellWidth)),cy=int(std::floor((wy-mapping.originY)/mapping.cellHeight));
  return cx>=0&&cy>=0&&cx<int(mapping.cellsX)&&cy<int(mapping.cellsY)&&clearCell[std::size_t(cy)*mapping.cellsX+cx];};
 texture2D(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_RENDER_TARGET,nullptr,0,sceneTexture);HR(device->CreateRenderTargetView(sceneTexture.Get(),nullptr,sceneTarget.GetAddressOf()),"scene target");
 {D3D11_TEXTURE2D_DESC desc{};sceneTexture->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&desc,nullptr,staging.GetAddressOf()),"staging");}
 auto readBack=[&]{Image image;image.width=width;image.height=height;image.rgba.resize(std::size_t(width)*height*4);context->CopyResource(staging.Get(),sceneTexture.Get());
  D3D11_MAPPED_SUBRESOURCE m{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"map");
  for(UINT y=0;y<height;++y)std::memcpy(image.rgba.data()+std::size_t(y)*width*4,static_cast<const std::uint8_t*>(m.pData)+std::size_t(y)*m.RowPitch,std::size_t(width)*16);context->Unmap(staging.Get(),0);return image;};
 XMFLOAT4X4 viewRows,projectionRows;XMStoreFloat4x4(&viewRows,view);XMStoreFloat4x4(&projectionRows,projection);
 const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
 const Light lights[]={{"midday",WeatherMode::Clear,{.35f,-.30f,.89f},{1.00f,.97f,.90f},{.30f,.31f,.33f}},
                       {"evening",WeatherMode::Clear,{-.80f,.25f,.30f},{1.00f,.70f,.42f},{.22f,.20f,.24f}},
                       {"overcast-rain",WeatherMode::Rain,{.30f,.40f,.87f},{.70f,.72f,.75f},{.33f,.35f,.38f}},
                       {"night",WeatherMode::Clear,{.20f,.50f,.84f},{.16f,.20f,.32f},{.08f,.10f,.16f}}};
 std::ofstream report(out/L"shroud-fog-checks.json");report<<"{\"previews\":[";bool firstPreview=true;double timedMs=-1,flatMs=-1;
 for(const auto &light:lights){
  AtmosphereGPU atmosphere;HR(atmosphere.Create(device.Get(),width,height,light.weather),"atmosphere");
  ShadowConstants shadows{};
  // Several prepared frames advance the weather controller to a settled state.
  for(int frame=0;frame<3;++frame){atmosphere.BeginFrame();Check(atmosphere.Prepare(context.Get(),&viewRows.m[0][0],&projectionRows.m[0][0],light.sun,light.color,shadows,nullptr,viewport,1.0/60,light.ambient)==S_OK,"atmosphere prepared");}
  Check(atmosphere.UpdateShroudVision(context.Get(),vision.data(),mapping.cellsX,mapping.cellsY,255,1,mapping.textureWidth,mapping.textureHeight,1.0/60)&&atmosphere.ShroudVision(),"vision texture prepared");
  // Scene radiance: the native lighting form (sun * N.L + ambient) * albedo.
  auto scene=[&](int mode){std::vector<float> rgba(std::size_t(width)*height*4);const float s=std::sqrt(light.sun[0]*light.sun[0]+light.sun[1]*light.sun[1]+light.sun[2]*light.sun[2]);
   for(std::size_t i=0;i<depth.size();++i){float *c=&rgba[i*4];c[3]=1;
    if(mode==1){c[0]=c[1]=c[2]=0;continue;}if(mode==2){c[0]=c[1]=c[2]=1;continue;}
    if(!hits[i].hit){c[0]=c[1]=c[2]=0;continue;}const auto &h=hits[i];const float nl=std::max(0.f,(h.n[0]*light.sun[0]+h.n[1]*light.sun[1]+h.n[2]*light.sun[2])/s);
    for(int k=0;k<3;++k)c[k]=h.albedo[k]*(light.color[k]*nl+light.ambient[k]);}
   context->UpdateSubresource(sceneTexture.Get(),0,nullptr,rgba.data(),width*16,0);return rgba;};
  auto draw=[&](bool flat){Check(atmosphere.DrawShroudFog(context.Get(),sceneTarget.Get(),depthView.Get(),shroudView.Get(),mapping,flat,flat?nullptr:atmosphere.ShroudVision()),"fog pass drawn");return readBack();};
  const auto base=scene(0);Image original;original.width=width;original.height=height;original.rgba=base;
  const auto flat=draw(true);scene(0);const auto fog=draw(false);
  for(float v:fog.rgba)Check(std::isfinite(v),"finite fog output");
  // 2. Flat mode = scene * bilinear native shroud.
  double flatWorst=0;
  for(std::size_t i=0;i<depth.size();++i){if(depth[i]>=.999999f)continue;const float s=cpuSample(world[i].x,world[i].y);
   for(int k=0;k<3;++k)flatWorst=std::max(flatWorst,double(std::abs(flat.rgba[i*4+k]-base[i*4+k]*s))/std::max(.02f,base[i*4+k]));}
  Check(flatWorst<.02,"flat mode equals scene * native shroud");
  // 3. Volume invariants with black and white scenes.
  scene(1);const auto black=draw(false);scene(2);const auto white=draw(false);
  const float fogCover=128/255.f; // out-of-sight cells in the vision texture
  const XMFLOAT3 eyePoint{XMVectorGetX(eye),XMVectorGetY(eye),XMVectorGetZ(eye)};
  std::uint64_t untouched=0,hidden=0,veiled=0;double hiddenWorst=0,veilWorst=0,hiddenLeast=1,veilLeast=1;
  for(std::size_t i=0;i<depth.size();++i){if(depth[i]>=.999999f)continue;const auto &p=world[i];
   const float cover=cpuCover(p.x,p.y);
   if(cover>=.999f){++hidden;for(int k=0;k<3;++k){const double kept=white.rgba[i*4+k]-black.rgba[i*4+k];hiddenWorst=std::max(hiddenWorst,kept);hiddenLeast=std::min(hiddenLeast,kept);}}
   else if(std::abs(cover-fogCover)<.002f){++veiled;for(int k=0;k<3;++k){const double kept=white.rgba[i*4+k]-black.rgba[i*4+k];veilWorst=std::max(veilWorst,kept);veilLeast=std::min(veilLeast,kept);}}
   // Rays whose whole layer segment lies over cells with three visible cells of margin.
   const float top=p.z+150/*ShroudFogTop*/,dz=p.z-eyePoint.z;if(dz>=0)continue;const float f=std::clamp((top-eyePoint.z)/dz,0.f,1.f);
   bool clear=true;for(int s=0;s<=16&&clear;++s){const float a=f+(1-f)*s/16.f;clear=clearAt(eyePoint.x+(p.x-eyePoint.x)*a,eyePoint.y+(p.y-eyePoint.y)*a);}
   if(clear){++untouched;Check(std::memcmp(&fog.rgba[i*4],&base[i*4],16)==0,"visible ray leaves the pixel bit-identical");}}
  Check(hidden>1000&&veiled>1000&&untouched>1000,"fixture covers unexplored, explored and visible surfaces");
  Check(hiddenWorst<=.6+2e-3,"unexplored surface keeps at most 60% of the scene");Check(hiddenLeast>=.1-2e-3&&veilLeast>=.1-2e-3,"fogged surfaces stay semi-transparent (at least 10%)");Check(veilWorst<=.85+2e-3,"out-of-sight surface keeps at most 85% of the scene");
  SaveRaw(out/(std::string(light.name)+"-scene.rgba32f"),original);SaveRaw(out/(std::string(light.name)+"-native.rgba32f"),flat);SaveRaw(out/(std::string(light.name)+"-fog.rgba32f"),fog);
  // 4. GPU time of the volume pass at 1920x1080 (fixture GPU, not the game).
  // Other GPU work (a running game) inflates single timings: alternate the
  // volume pass with the flat pass (one texture fetch) and keep each minimum.
  if(timedMs<0){scene(0);ComPtr<ID3D11Query> disjoint,start,end;D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};HR(device->CreateQuery(&q,disjoint.GetAddressOf()),"query");q.Query=D3D11_QUERY_TIMESTAMP;
   HR(device->CreateQuery(&q,start.GetAddressOf()),"query");HR(device->CreateQuery(&q,end.GetAddressOf()),"query");
   auto timed=[&](bool flatPass){context->Begin(disjoint.Get());context->End(start.Get());for(int r=0;r<10;++r)atmosphere.DrawShroudFog(context.Get(),sceneTarget.Get(),depthView.Get(),shroudView.Get(),mapping,flatPass,flatPass?nullptr:atmosphere.ShroudVision());context->End(end.Get());context->End(disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};while(context->GetData(disjoint.Get(),&d,sizeof(d),0)!=S_OK){}UINT64 a=0,b=0;while(context->GetData(start.Get(),&a,8,0)!=S_OK){}while(context->GetData(end.Get(),&b,8,0)!=S_OK){}
    return d.Disjoint?1e9:double(b-a)/double(d.Frequency)*1000/10;};
   timedMs=flatMs=1e9;for(int round=0;round<40;++round){timedMs=std::min(timedMs,timed(false));flatMs=std::min(flatMs,timed(true));}}
  report<<(firstPreview?"":",")<<"{\"light\":\""<<light.name<<"\",\"flat_relative_error\":"<<flatWorst<<",\"unexplored_pixels\":"<<hidden<<",\"unexplored_scene_fraction_max\":"<<hiddenWorst<<",\"unexplored_scene_fraction_min\":"<<hiddenLeast
        <<",\"explored_pixels\":"<<veiled<<",\"explored_scene_fraction\":"<<veilWorst<<",\"explored_scene_fraction_min\":"<<veilLeast<<",\"visible_untouched_pixels\":"<<untouched<<"}";firstPreview=false;
 }
 // 5. Vision fade: one step follows the exponential rates (clearing ~0.4 s,
 // fogging ~1.5 s), and many steps converge to the new cell states.
 double fadeWorst=0,convergedWorst=0;{
  AtmosphereGPU atmosphere;HR(atmosphere.Create(device.Get(),width,height,WeatherMode::Clear),"fade atmosphere");
  Check(atmosphere.UpdateShroudVision(context.Get(),vision.data(),mapping.cellsX,mapping.cellsY,255,1,mapping.textureWidth,mapping.textureHeight,1.0/60),"vision snap");
  auto changed=vision;for(UINT y=0;y<mapping.cellsY;++y)for(UINT x=0;x<mapping.cellsX;++x){auto &c=changed[std::size_t(y)*mapping.cellsX+x];
   if(x<20)c=c==255?128:(c==128?0:c);else if(c==0)c=128;}
  ComPtr<ID3D11Texture2D> state;{D3D11_TEXTURE2D_DESC d{};d.Width=mapping.textureWidth;d.Height=mapping.textureHeight;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16_FLOAT;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&d,nullptr,state.GetAddressOf()),"vision staging");}
  auto readVision=[&]{ComPtr<ID3D11Resource> resource;atmosphere.ShroudVision()->GetResource(resource.GetAddressOf());context->CopySubresourceRegion(state.Get(),0,0,0,0,resource.Get(),0,nullptr);
   D3D11_MAPPED_SUBRESOURCE m{};HR(context->Map(state.Get(),0,D3D11_MAP_READ,0,&m),"vision map");std::vector<float> v(std::size_t(mapping.textureWidth)*mapping.textureHeight);
   for(UINT y=0;y<mapping.textureHeight;++y)for(UINT x=0;x<mapping.textureWidth;++x)v[std::size_t(y)*mapping.textureWidth+x]=PackedVector::XMConvertHalfToFloat(reinterpret_cast<const PackedVector::HALF*>(static_cast<const std::uint8_t*>(m.pData)+std::size_t(y)*m.RowPitch)[x]);
   context->Unmap(state.Get(),0);return v;};
  auto cell=[&](const std::vector<std::uint8_t> &cells,UINT x,UINT y){return x<1||y<1||x>mapping.cellsX||y>mapping.cellsY?1.f:cells[std::size_t(y-1)*mapping.cellsX+x-1]/255.f;};
  const auto before=readVision();const double dt=.1;
  Check(atmosphere.UpdateShroudVision(context.Get(),changed.data(),mapping.cellsX,mapping.cellsY,255,2,mapping.textureWidth,mapping.textureHeight,dt),"vision fade step");
  const auto after=readVision();const auto rates=MakeShroudFadeConstants(dt,false);
  for(UINT y=0;y<mapping.textureHeight;++y)for(UINT x=0;x<mapping.textureWidth;++x){const std::size_t i=std::size_t(y)*mapping.textureWidth+x;const float goal=cell(changed,x,y),previous=before[i];
   float next=previous+(goal-previous)*(goal<previous?rates.rates[0]:rates.rates[1]);if(std::abs(next-goal)<.002f)next=goal;fadeWorst=std::max(fadeWorst,double(std::abs(after[i]-next)));}
  for(int step=0;step<80;++step)atmosphere.UpdateShroudVision(context.Get(),changed.data(),mapping.cellsX,mapping.cellsY,255,2,mapping.textureWidth,mapping.textureHeight,.25);
  const auto converged=readVision();
  for(UINT y=0;y<mapping.textureHeight;++y)for(UINT x=0;x<mapping.textureWidth;++x)convergedWorst=std::max(convergedWorst,double(std::abs(converged[std::size_t(y)*mapping.textureWidth+x]-cell(changed,x,y))));
  Check(fadeWorst<2e-3,"vision fade step follows the exponential rates");Check(convergedWorst<1e-3,"vision converges to the cell states");
  Check(rates.rates[0]>rates.rates[1]&&rates.rates[1]>0,"fog clears faster than it returns");
 }
 DXGI_ADAPTER_DESC adapter{};{ComPtr<IDXGIDevice> d;ComPtr<IDXGIAdapter> a;if(SUCCEEDED(device.As(&d))&&SUCCEEDED(d->GetAdapter(a.GetAddressOf())))a->GetDesc(&adapter);}
 char name[128]{};WideCharToMultiByte(CP_UTF8,0,adapter.Description,-1,name,sizeof(name),nullptr,nullptr);
 report<<"],\"vision_fade_error\":"<<fadeWorst<<",\"vision_converged_error\":"<<convergedWorst<<",\"gpu\":\""<<name<<"\",\"volume_pass_ms_1080p\":"<<timedMs<<",\"flat_pass_ms_1080p\":"<<flatMs<<",\"checks\":"<<checks<<",\"passed\":true}\n";
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"volume_pass_ms_1080p\":"<<timedMs<<",\"flat_pass_ms_1080p\":"<<flatMs<<"}\n";return 0;
}catch(const std::exception &e){std::cout<<"{\"passed\":false,\"error\":\""<<e.what()<<"\",\"checks\":"<<checks<<"}\n";return 1;}}
