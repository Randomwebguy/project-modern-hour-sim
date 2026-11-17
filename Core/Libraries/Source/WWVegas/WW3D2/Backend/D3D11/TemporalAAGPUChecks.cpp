// SPDX-License-Identifier: GPL-3.0-or-later
// taa100: offline checks of the temporal AA resolve on the actual GPU with the
// production TemporalAAGPU and TemporalAA.hlsl. A CPU ray cast of a synthetic
// RTS view (striped ground with thin grid lines, a building, a moving unit)
// supplies HDR colour and hardware depth; jittered frames are rendered through
// the production ApplyTemporalJitter projection.
//  1. Halton jitter: 8 distinct sub-pixel offsets in (-.5,.5); ApplyTemporalJitter
//     moves every projected point by exactly the jitter (perspective, ortho).
//  2. Static camera: the resolve converges toward a 64-sample reference on edge
//     pixels, keeps flat areas, and is temporally stable (no shimmer).
//  3. Camera pan (slow and fast): no ghosting, edges still better than one sample.
//  4. Moving unit (no per-object motion vectors): the previous-depth disocclusion
//     test and the variance clip remove its trail.
//  Also reported: the error of the exact 8-position jitter mean (the bound of
//  an 8-sample cycle). GENZH_TAA_* variables override settings for sweeps only.
//  5. A camera cut and Invalidate() resolve exactly like a fresh history.
//  6. Pixels outside the camera viewport pass through bit-identical; a firefly
//     does not persist; no NaN/Inf.
//  7. GPU time of the resolve (with the copy back) at 1920x1080.
#include "TemporalAAGPU.h"
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace GenZH::Modern;using namespace DirectX;using Microsoft::WRL::ComPtr;
static unsigned checks=0;
static bool sweep=false; // GENZH_TAA_SWEEP: report failed checks and continue (parameter sweeps only)
static void Check(bool ok,const char *what){++checks;if(!ok){if(sweep){std::cerr<<"FAIL "<<what<<std::endl;return;}throw std::runtime_error(what);}}
static void HR(HRESULT hr,const char *what){if(FAILED(hr)){char text[256];std::snprintf(text,sizeof(text),"%s (0x%08lx)",what,static_cast<unsigned long>(hr));throw std::runtime_error(text);}++checks;}

// Fixture: target 680x400 with the camera viewport at (20,20) 640x360.
constexpr UINT TW=680,TH=400,VX=20,VY=20,VW=640,VH=360;
struct Color {float r=0,g=0,b=0;};
struct Camera {XMFLOAT3 eye,target;float unitX;};
static float Fract(float v){return v-std::floor(v);}
static Color Ground(float x,float y){
 // Diagonal HDR stripes and thin dark grid lines: the aliasing-prone content.
 const bool stripe=Fract((x*.8f+y*.6f)/9.f)<.5f;Color c=stripe?Color{1.6f,1.45f,1.2f}:Color{.10f,.12f,.08f};
 if(Fract(x/24.f)<.06f||Fract(y/24.f)<.06f)c={.02f,.02f,.025f};
 return c;
}
struct Box {float lo[3],hi[3];Color albedo;};
static bool Slab(const Box &b,const XMFLOAT3 &o,const XMFLOAT3 &d,float &t,int &axis,int &sign){
 float t0=0,t1=1e9f;axis=-1;sign=0;const float oo[3]={o.x,o.y,o.z},dd[3]={d.x,d.y,d.z};
 for(int a=0;a<3;++a){
  if(std::abs(dd[a])<1e-9f){if(oo[a]<b.lo[a]||oo[a]>b.hi[a])return false;continue;}
  float ta=(b.lo[a]-oo[a])/dd[a],tb=(b.hi[a]-oo[a])/dd[a];int s=-1;if(ta>tb){std::swap(ta,tb);s=1;}
  if(ta>t0){t0=ta;axis=a;sign=s;}t1=std::min(t1,tb);if(t0>t1)return false;
 }
 t=t0;return axis>=0;
}
struct Hit {Color color;XMFLOAT3 position;bool unit=false;};
static Hit Cast(const XMFLOAT3 &o,const XMFLOAT3 &d,float unitX){
 const Box boxes[2]={{{30,10,0},{70,50,40},{1.2f,1.1f,1.0f}},{{unitX,-40,0},{unitX+14,-26,10},{1.8f,.3f,.2f}}};
 Hit h;float best=d.z<0?-o.z/d.z:1e9f;h.color=Ground(o.x+d.x*best,o.y+d.y*best);
 const float sun[3]={.4f,.3f,.866f};
 for(int i=0;i<2;++i){float t;int axis,sign;if(Slab(boxes[i],o,d,t,axis,sign)&&t<best){best=t;const float light=.25f+.75f*std::max(0.f,sign*sun[axis]);
   h.color={boxes[i].albedo.r*light,boxes[i].albedo.g*light,boxes[i].albedo.b*light};h.unit=i==1;}}
 h.position={o.x+d.x*best,o.y+d.y*best,o.z+d.z*best};return h;
}
struct Matrices {XMFLOAT4X4 view,projection,vp,inverseVP;};
static Matrices Build(const Camera &c,float jx,float jy){
 Matrices m;XMStoreFloat4x4(&m.view,XMMatrixLookAtLH(XMLoadFloat3(&c.eye),XMLoadFloat3(&c.target),XMVectorSet(0,0,1,0)));
 XMStoreFloat4x4(&m.projection,XMMatrixPerspectiveFovLH(XMConvertToRadians(50),float(VW)/VH,1,5000));
 ApplyTemporalJitter(&m.projection._11,jx,jy,float(VW),float(VH));
 const XMMATRIX vp=XMMatrixMultiply(XMLoadFloat4x4(&m.view),XMLoadFloat4x4(&m.projection));XMStoreFloat4x4(&m.vp,vp);XMStoreFloat4x4(&m.inverseVP,XMMatrixInverse(nullptr,vp));return m;
}
struct Image {std::vector<float> rgba;std::vector<float> depth;std::vector<unsigned char> unit,edge;};
// Renders the frame (centre sample per pixel through the jittered projection) or,
// with n>1, an n*n box-filtered reference of the unjittered camera.
static Image Render(const Camera &c,float jx,float jy,int n=1){
 const Matrices m=Build(c,jx,jy);const XMMATRIX inverse=XMLoadFloat4x4(&m.inverseVP),vp=XMLoadFloat4x4(&m.vp);
 Image image;image.rgba.resize(std::size_t(TW)*TH*4);image.depth.assign(std::size_t(TW)*TH,1.f);image.unit.assign(std::size_t(TW)*TH,0);image.edge.assign(std::size_t(TW)*TH,0);
 for(UINT y=0;y<TH;++y)for(UINT x=0;x<TW;++x){const std::size_t i=std::size_t(y)*TW+x;
  if(x<VX||y<VY||x>=VX+VW||y>=VY+VH){float *p=&image.rgba[i*4];p[0]=.3f+x*1e-3f;p[1]=.2f+y*1e-3f;p[2]=.1f;p[3]=1;continue;} // letterbox/HUD pattern
  Color sum;float lowLuma=1e9f,highLuma=-1e9f;
  for(int sy=0;sy<n;++sy)for(int sx=0;sx<n;++sx){
   const float px=float(x-VX)+(n==1?.5f:(sx+.5f)/n),py=float(y-VY)+(n==1?.5f:(sy+.5f)/n);
   const XMVECTOR ndc=XMVectorSet(px/VW*2-1,1-py/VH*2,0,1);
   XMFLOAT3 o,f;XMStoreFloat3(&o,XMVector3TransformCoord(ndc,inverse));XMStoreFloat3(&f,XMVector3TransformCoord(XMVectorSetZ(ndc,1),inverse));
   XMFLOAT3 d;XMStoreFloat3(&d,XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&f),XMLoadFloat3(&o))));
   const Hit h=Cast(o,d,c.unitX);sum.r+=h.color.r;sum.g+=h.color.g;sum.b+=h.color.b;
   const float luma=(h.color.r+2*h.color.g+h.color.b)/4;const float tone=luma/(1+luma);lowLuma=std::min(lowLuma,tone);highLuma=std::max(highLuma,tone);
   if(n==1){XMFLOAT4 clip;XMStoreFloat4(&clip,XMVector4Transform(XMVectorSet(h.position.x,h.position.y,h.position.z,1),vp));image.depth[i]=clip.z/clip.w;image.unit[i]=h.unit;}
  }
  const float inv=1.f/(n*n);float *p=&image.rgba[i*4];p[0]=sum.r*inv;p[1]=sum.g*inv;p[2]=sum.b*inv;p[3]=1;image.edge[i]=highLuma-lowLuma>.1f;
 }
 return image;
}
static TemporalAAFrame FrameOf(const Camera &c,float jx,float jy){
 const Matrices m=Build(c,0,0);TemporalAAFrame f;std::memcpy(f.inverseViewProjection,&m.inverseVP,64);std::memcpy(f.viewProjection,&m.vp,64);
 f.viewport[0]=VX;f.viewport[1]=VY;f.viewport[2]=VW;f.viewport[3]=VH;f.eye[0]=c.eye.x;f.eye[1]=c.eye.y;f.eye[2]=c.eye.z;f.jitter[0]=jx;f.jitter[1]=jy;return f;
}
// Parameter sweeps only (GENZH_TAA_SHARPNESS / GENZH_TAA_BLEND); unset = production defaults.
static float sharpenSetting=0; // the TAA quality checks measure the unsharpened resolve; section 8 sharpens
static void Tune(TemporalAAGPU &taa){taa.settings.sharpen=sharpenSetting;char v[32]{};if(GetEnvironmentVariableA("GENZH_TAA_SHARPNESS",v,sizeof(v)))taa.settings.sharpness=float(std::atof(v));
 if(GetEnvironmentVariableA("GENZH_TAA_BLEND",v,sizeof(v)))taa.settings.blend=float(std::atof(v));
 if(GetEnvironmentVariableA("GENZH_TAA_TONEMAPPED",v,sizeof(v)))taa.settings.tonemapped=v[0]=='1';
 if(GetEnvironmentVariableA("GENZH_TAA_GAMMA",v,sizeof(v)))taa.settings.gamma=float(std::atof(v));
 if(GetEnvironmentVariableA("GENZH_TAA_LUMA",v,sizeof(v)))taa.settings.lumaWeight=float(std::atof(v));
 if(GetEnvironmentVariableA("GENZH_TAA_DISOCCLUSION",v,sizeof(v)))taa.settings.disocclusionFootprints=float(std::atof(v)),taa.settings.disocclusion=std::atof(v)>0;
 if(GetEnvironmentVariableA("GENZH_TAA_MOTION",v,sizeof(v)))taa.settings.motionBlend=float(std::atof(v));
 if(GetEnvironmentVariableA("GENZH_TAA_MOTION_SCALE",v,sizeof(v)))taa.settings.motionScale=float(std::atof(v));}
struct Gpu {
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Texture2D> color,depth,staging;ComPtr<ID3D11ShaderResourceView> colorView,depthView;ComPtr<ID3D11RenderTargetView> colorTarget;UINT width=0,height=0;
 void Create(UINT w,UINT h){
  width=w;height=h;D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  color.Reset();depth.Reset();staging.Reset();colorView.Reset();depthView.Reset();colorTarget.Reset();d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
  HR(device->CreateTexture2D(&d,nullptr,color.GetAddressOf()),"colour");HR(device->CreateShaderResourceView(color.Get(),nullptr,colorView.GetAddressOf()),"colour view");HR(device->CreateRenderTargetView(color.Get(),nullptr,colorTarget.GetAddressOf()),"colour target");
  d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&d,nullptr,staging.GetAddressOf()),"staging");
  // The scene depth format of the game (SceneDepthGPU): D24S8 read as R24 unorm.
  d.Format=DXGI_FORMAT_R24G8_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
  HR(device->CreateTexture2D(&d,nullptr,depth.GetAddressOf()),"depth");HR(device->CreateShaderResourceView(depth.Get(),&sv,depthView.GetAddressOf()),"depth view");
 }
 void Upload(const Image &image){
  std::vector<PackedVector::HALF> half(image.rgba.size());for(std::size_t i=0;i<half.size();++i)half[i]=PackedVector::XMConvertFloatToHalf(image.rgba[i]);
  context->UpdateSubresource(color.Get(),0,nullptr,half.data(),width*8,0);std::vector<std::uint32_t> packed(image.depth.size());for(std::size_t i=0;i<packed.size();++i)packed[i]=std::uint32_t(std::clamp(double(image.depth[i]),0.0,1.0)*16777215.0+.5);
  context->UpdateSubresource(depth.Get(),0,nullptr,packed.data(),width*4,0);
 }
 std::vector<PackedVector::HALF> Read(){
  context->CopyResource(staging.Get(),color.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"map");
  std::vector<PackedVector::HALF> out(std::size_t(width)*height*4);for(UINT y=0;y<height;++y)std::memcpy(&out[std::size_t(y)*width*4],static_cast<const unsigned char*>(map.pData)+std::size_t(y)*map.RowPitch,width*8);
  context->Unmap(staging.Get(),0);return out;
 }
 std::vector<PackedVector::HALF> Resolve(TemporalAAGPU &taa,const Image &image,const TemporalAAFrame &frame){Tune(taa);Upload(image);HR(taa.Resolve(context.Get(),color.Get(),colorView.Get(),depthView.Get(),frame,colorTarget.Get()),"resolve");return Read();}
};
static float Tone(float v){v=std::max(v,0.f);return v/(1+v);}
static float H(PackedVector::HALF h){return PackedVector::XMConvertHalfToFloat(h);}
struct Error {double edge=0,flat=0,all=0,maxAll=0;std::size_t edges=0,flats=0;};
// Mean absolute tonemapped error per channel against the reference, viewport only.
static Error Compare(const std::vector<PackedVector::HALF> &out,const Image &reference,const std::vector<unsigned char> *mask=nullptr){
 Error e;std::size_t count=0;
 for(UINT y=VY;y<VY+VH;++y)for(UINT x=VX;x<VX+VW;++x){const std::size_t i=std::size_t(y)*TW+x;if(mask&&!(*mask)[i])continue;
  double v=0;for(int k=0;k<3;++k)v+=std::abs(Tone(H(out[i*4+k]))-Tone(reference.rgba[i*4+k]))/3;
  e.all+=v;++count;e.maxAll=std::max(e.maxAll,v);if(reference.edge[i]){e.edge+=v;++e.edges;}else{e.flat+=v;++e.flats;}
 }
 e.all/=std::max<std::size_t>(count,1);e.edge/=std::max<std::size_t>(e.edges,1);e.flat/=std::max<std::size_t>(e.flats,1);return e;
}
static std::vector<PackedVector::HALF> Halves(const Image &image){std::vector<PackedVector::HALF> h(image.rgba.size());for(std::size_t i=0;i<h.size();++i)h[i]=PackedVector::XMConvertFloatToHalf(image.rgba[i]);return h;}
static bool Finite(const std::vector<PackedVector::HALF> &out){for(auto h:out)if(!std::isfinite(H(h)))return false;return true;}
static double Flicker(const std::vector<PackedVector::HALF> &a,const std::vector<PackedVector::HALF> &b,const Image &reference){
 double sum=0;std::size_t n=0;for(UINT y=VY;y<VY+VH;++y)for(UINT x=VX;x<VX+VW;++x){const std::size_t i=std::size_t(y)*TW+x;if(!reference.edge[i])continue;
  for(int k=0;k<3;++k)sum+=std::abs(Tone(H(a[i*4+k]))-Tone(H(b[i*4+k])))/3;++n;}
 return sum/std::max<std::size_t>(n,1);
}
static void SavePPM(const std::filesystem::path &path,const std::vector<PackedVector::HALF> &out){
 std::ofstream f(path,std::ios::binary);f<<"P6\n"<<VW<<' '<<VH<<"\n255\n";
 for(UINT y=VY;y<VY+VH;++y)for(UINT x=VX;x<VX+VW;++x)for(int k=0;k<3;++k){const float v=std::pow(Tone(H(out[(std::size_t(y)*TW+x)*4+k])),1/2.2f);f.put(static_cast<char>(std::clamp(int(v*255+.5f),0,255)));}
}
static bool OutsideIdentical(const std::vector<PackedVector::HALF> &out,const std::vector<PackedVector::HALF> &input){
 for(UINT y=0;y<TH;++y)for(UINT x=0;x<TW;++x){if(x>=VX&&y>=VY&&x<VX+VW&&y<VY+VH)continue;const std::size_t i=(std::size_t(y)*TW+x)*4;if(std::memcmp(&out[i],&input[i],6))return false;}
 return true;
}

int main(int argc,char **argv){try{
 const std::filesystem::path out=argc>1?argv[1]:"taa-checks";std::filesystem::create_directories(out);
 {char v[8]{};sweep=GetEnvironmentVariableA("GENZH_TAA_SWEEP",v,sizeof(v))>0;}
 // 1. Jitter sequence and projection offset.
 double jitterWorst=0,jitterMean[2]{};
 {std::array<std::array<float,2>,8> seen{};
  for(unsigned f=0;f<8;++f){seen[f]=TemporalJitterPixels(f);Check(seen[f][0]>-.5f&&seen[f][0]<.5f&&seen[f][1]>-.5f&&seen[f][1]<.5f,"jitter inside the pixel");jitterMean[0]+=seen[f][0]/8;jitterMean[1]+=seen[f][1]/8;
   for(unsigned g=0;g<f;++g)Check(seen[f]!=seen[g],"8 distinct jitter offsets");Check(TemporalJitterPixels(f+8)==seen[f],"8-frame cycle");}
  Check(std::abs(jitterMean[0])<.07&&std::abs(jitterMean[1])<.07,"jitter sequence is centred");
  for(int ortho=0;ortho<2;++ortho){
   XMFLOAT4X4 view,projection,jittered;XMStoreFloat4x4(&view,XMMatrixLookAtLH(XMVectorSet(10,-300,260,1),XMVectorSet(0,0,0,1),XMVectorSet(0,0,1,0)));
   XMStoreFloat4x4(&projection,ortho?XMMatrixOrthographicLH(800,450,1,5000):XMMatrixPerspectiveFovLH(XMConvertToRadians(50),16.f/9,1,5000));
   for(unsigned f=0;f<8;++f){const auto j=TemporalJitterPixels(f);jittered=projection;ApplyTemporalJitter(&jittered._11,j[0],j[1],1920,1080);
    for(int p=0;p<64;++p){const XMVECTOR world=XMVectorSet(float((p*37)%200-100),float((p*53)%240-120),float((p*11)%40),1);
     auto pixel=[&](const XMFLOAT4X4 &proj){XMFLOAT4 c;XMStoreFloat4(&c,XMVector4Transform(world,XMMatrixMultiply(XMLoadFloat4x4(&view),XMLoadFloat4x4(&proj))));return XMFLOAT2((c.x/c.w*.5f+.5f)*1920,(-c.y/c.w*.5f+.5f)*1080);};
     const XMFLOAT2 a=pixel(projection),b=pixel(jittered);jitterWorst=std::max({jitterWorst,double(std::abs(b.x-a.x-j[0])),double(std::abs(b.y-a.y-j[1]))});}}
  }
  Check(jitterWorst<2e-3,"projection jitter moves every point by exactly the offset");}
 Gpu gpu;{D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0},fl{};
  HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,gpu.device.GetAddressOf(),&fl,gpu.context.GetAddressOf()),"hardware device");}
 gpu.Create(TW,TH);
 // 2. Static camera.
 const Camera still{{0,-300,260},{0,0,0},-60};
 const Image reference=Render(still,0,0,8),aliased=Render(still,0,0);
 const Error aliasedError=Compare(Halves(aliased),reference);
 // Lower bound of an 8-position jitter: the exact mean of the 8 jittered frames.
 Image ideal=aliased;std::fill(ideal.rgba.begin(),ideal.rgba.end(),0.f);
 for(unsigned f=0;f<8;++f){const auto j=TemporalJitterPixels(f);const Image frame=Render(still,j[0],j[1]);for(std::size_t i=0;i<ideal.rgba.size();++i)ideal.rgba[i]+=frame.rgba[i]/8;}
 const Error idealError=Compare(Halves(ideal),reference);
 TemporalAAGPU taa;HR(taa.Create(gpu.device.Get(),TW,TH),"temporal create");
 std::vector<PackedVector::HALF> result,previous;double rawFlicker=0;std::vector<PackedVector::HALF> rawPrevious;bool passThrough=true,finite=true;double fireflyError=0,fireflyPeak=0;
 const UINT fx=VX+VW/2+7,fy=VY+VH/2+5;
 for(unsigned f=0;f<32;++f){const auto j=TemporalJitterPixels(f);Image frame=Render(still,j[0],j[1]);
  if(f==20){float *p=&frame.rgba[(std::size_t(fy)*TW+fx)*4];p[0]=p[1]=p[2]=5000;} // a single-frame firefly
  const auto input=Halves(frame);previous=result;result=gpu.Resolve(taa,frame,FrameOf(still,j[0],j[1]));
  passThrough=passThrough&&OutsideIdentical(result,input);finite=finite&&Finite(result);
  if(f==20){const std::size_t i=(std::size_t(fy)*TW+fx)*4;fireflyPeak=Tone(H(result[i]));}
  if(f==21){const std::size_t i=(std::size_t(fy)*TW+fx)*4;for(int k=0;k<3;++k)fireflyError=std::max(fireflyError,double(std::abs(Tone(H(result[i+k]))-Tone(reference.rgba[i+k]))));}
  const auto raw=Halves(frame);if(f>=24&&f!=20&&f!=21)rawFlicker+=Flicker(raw,rawPrevious,reference)/8;rawPrevious=raw;
 }
 const Error staticError=Compare(result,reference);const double taaFlicker=Flicker(result,previous,reference);
 SavePPM(out/"static-reference.ppm",Halves(reference));SavePPM(out/"static-aliased.ppm",Halves(aliased));SavePPM(out/"static-taa.ppm",result);
 std::cerr<<"static: aliased edge "<<aliasedError.edge<<" flat "<<aliasedError.flat<<", ideal8 edge "<<idealError.edge<<" flat "<<idealError.flat<<", taa edge "<<staticError.edge<<" flat "<<staticError.flat<<", edges "<<staticError.edges
          <<", flicker raw "<<rawFlicker<<" taa "<<taaFlicker<<", firefly "<<fireflyError<<" peak "<<fireflyPeak<<std::endl;
 Check(passThrough,"pixels outside the camera viewport pass through bit-identical");Check(finite,"no NaN/Inf");
 Check(staticError.edge<.6*aliasedError.edge,"static edges converge toward the supersampled reference");
 Check(staticError.flat<.02,"flat areas are kept");
 Check(taaFlicker<.35*rawFlicker,"resolved edges are temporally stable");
 Check(fireflyError<.15,"a single-frame firefly does not persist");
 // 5. Camera cut and Invalidate(): bit-identical to a fresh history.
 {const Camera jump{{400,-200,300},{380,40,0},-60};const auto j=TemporalJitterPixels(32);const Image frame=Render(jump,j[0],j[1]);
  const auto cuts=taa.Stats().cuts;const auto cut=gpu.Resolve(taa,frame,FrameOf(jump,j[0],j[1]));Check(taa.Stats().cuts==cuts+1,"camera cut detected");
  TemporalAAGPU fresh;HR(fresh.Create(gpu.device.Get(),TW,TH),"fresh create");const auto first=gpu.Resolve(fresh,frame,FrameOf(jump,j[0],j[1]));
  Check(cut==first,"a camera cut resolves like a fresh history");
  taa.Invalidate();const auto invalidated=gpu.Resolve(taa,frame,FrameOf(jump,j[0],j[1]));Check(invalidated==first,"Invalidate() resolves like a fresh history");}
 // 3. Camera pans: slow (~1.3 px/frame) and fast (~12 px/frame), 24 frames each.
 struct Pan {const char *name;float dx,dy;Error error,aliased;};Pan pans[2]={{"slow",1.1f,.7f,{},{}},{"fast",10.f,6.f,{},{}}};
 for(auto &pan:pans){TemporalAAGPU moving;HR(moving.Create(gpu.device.Get(),TW,TH),"pan create");Camera c=still;std::vector<PackedVector::HALF> last;
  for(unsigned f=0;f<24;++f){c.eye.x=still.eye.x+pan.dx*f;c.eye.y=still.eye.y+pan.dy*f;c.target.x=still.target.x+pan.dx*f;c.target.y=still.target.y+pan.dy*f;
   const auto j=TemporalJitterPixels(f);last=gpu.Resolve(moving,Render(c,j[0],j[1]),FrameOf(c,j[0],j[1]));}
  Check(moving.Stats().cuts==0,"a pan is not a cut");const Image panReference=Render(c,0,0,8);pan.error=Compare(last,panReference);pan.aliased=Compare(Halves(Render(c,0,0)),panReference);
  SavePPM(out/(std::string("pan-")+pan.name+"-taa.ppm"),last);if(sweep)SavePPM(out/(std::string("pan-")+pan.name+"-reference.ppm"),Halves(panReference));
  std::cerr<<"pan "<<pan.name<<": aliased edge "<<pan.aliased.edge<<" flat "<<pan.aliased.flat<<", taa edge "<<pan.error.edge<<" flat "<<pan.error.flat<<std::endl;
  Check(pan.error.flat<.03,"pan keeps flat areas (no smear)");Check(pan.error.edge<pan.aliased.edge,"pan edges stay better than one sample");}
 // 4. Moving unit (camera-only reprojection): its trail must be clipped away.
 double trailError=0,trailMax=0;std::size_t trailPixels=0;Error unitError;
 {TemporalAAGPU moving;HR(moving.Create(gpu.device.Get(),TW,TH),"unit create");Camera c=still;std::vector<PackedVector::HALF> last;std::vector<unsigned char> trail(std::size_t(TW)*TH,0);Image frame;
  for(unsigned f=0;f<24;++f){c.unitX=-100+4.f*f;const auto j=TemporalJitterPixels(f);frame=Render(c,j[0],j[1]);
   if(f>=18&&f<23)for(std::size_t i=0;i<trail.size();++i)trail[i]|=frame.unit[i];last=gpu.Resolve(moving,frame,FrameOf(c,j[0],j[1]));}
  const Image unitReference=Render(c,0,0,8);const Image centre=Render(c,0,0);for(std::size_t i=0;i<trail.size();++i)trail[i]=trail[i]&&!centre.unit[i]&&!unitReference.edge[i];
  for(std::size_t i=0;i<trail.size();++i)trailPixels+=trail[i];const Error e=Compare(last,unitReference,&trail);trailError=e.all;trailMax=e.maxAll;unitError=Compare(last,unitReference);
  SavePPM(out/"unit-taa.ppm",last);}
 std::cerr<<"unit: trail pixels "<<trailPixels<<" error "<<trailError<<" max "<<trailMax<<", edge "<<unitError.edge<<" flat "<<unitError.flat<<std::endl;
 Check(trailPixels>200,"fixture has a unit trail");Check(trailError<.03,"moving unit leaves no ghost trail");
 // 8. post102: CAS of the output (the history stays unsharpened). Gradient energy (crispness)
 // must rise, the mean must hold and no pixel may overshoot its unsharpened 3x3 range (halos).
 double casFlicker[2]={};
 auto casRun=[&](float sharpen,bool pan){sharpenSetting=sharpen;TemporalAAGPU t;HR(t.Create(gpu.device.Get(),TW,TH),"CAS create");Camera c=still;std::vector<PackedVector::HALF> last,before;
  for(unsigned f=0;f<24;++f){if(pan){c.eye.x=still.eye.x+10.f*f;c.eye.y=still.eye.y+6.f*f;c.target.x=still.target.x+10.f*f;c.target.y=still.target.y+6.f*f;}
   const auto j=TemporalJitterPixels(f);before.swap(last);last=gpu.Resolve(t,Render(c,j[0],j[1]),FrameOf(c,j[0],j[1]));}
  if(!pan)casFlicker[sharpen>0?1:0]=Flicker(last,before,reference);
  Check(sharpen<=0||t.Stats().sharpened==24,"every output frame sharpened");sharpenSetting=0;return last;};
 auto toneLuma=[&](const std::vector<PackedVector::HALF> &p,UINT x,UINT y){const std::size_t i=(std::size_t(y)*TW+x)*4;return (Tone(H(p[i]))+2*Tone(H(p[i+1]))+Tone(H(p[i+2])))/4;};
 struct Crisp {double gradient=0,mean=0,overshoot=0,edges=0,halo=0;};
 char casChoice[32]={};const float casStrength=GetEnvironmentVariableA("GENZH_TAA_CAS",casChoice,sizeof(casChoice))?static_cast<float>(std::atof(casChoice)):.35f;
 auto crisp=[&](const std::vector<PackedVector::HALF> &plain,const std::vector<PackedVector::HALF> &sharp){Crisp r[2];std::size_t n=0,over=0,edges=0;double halo=0;
  for(UINT y=VY+1;y+1<VY+VH;++y)for(UINT x=VX+1;x+1<VX+VW;++x){++n;
   for(int k=0;k<2;++k){const auto &p=k?sharp:plain;r[k].gradient+=std::abs(toneLuma(p,x+1,y)-toneLuma(p,x,y))+std::abs(toneLuma(p,x,y+1)-toneLuma(p,x,y));r[k].mean+=toneLuma(p,x,y);}
   // Halos are judged at strong edges (plain 3x3 range > .2): elsewhere sharpening a local
   // extremum past its neighbours is the intended effect, not ringing.
   float lo=1e9f,hi=-1e9f;for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const float v=toneLuma(plain,x+dx,y+dy);lo=std::min(lo,v);hi=std::max(hi,v);}
   if(hi-lo>.2f){++edges;const float v=toneLuma(sharp,x,y);halo+=std::max(0.f,std::max(lo-v,v-hi));if(v<lo-.02f||v>hi+.02f)++over;}}
  Crisp out;out.gradient=r[1].gradient/std::max(r[0].gradient,1e-9);out.mean=r[1].mean/std::max(r[0].mean,1e-9);out.overshoot=double(over)/std::max<std::size_t>(edges,1);out.edges=double(edges)/std::max<std::size_t>(n,1);out.halo=halo/std::max<std::size_t>(edges,1);return out;};
 const auto staticCas=crisp(casRun(0,false),casRun(casStrength,false)),panCas=crisp(casRun(0,true),casRun(casStrength,true));
 {const auto plain=casRun(0,false),sharp=casRun(casStrength,false);SavePPM(out/"cas-off.ppm",plain);SavePPM(out/"cas-on.ppm",sharp);}
 std::cerr<<"CAS static: gradient x"<<staticCas.gradient<<" mean x"<<staticCas.mean<<" edge overshoot "<<staticCas.overshoot<<" halo "<<staticCas.halo<<" (edges "<<staticCas.edges<<") flicker "<<casFlicker[0]<<" -> "<<casFlicker[1]<<"; fast pan: gradient x"<<panCas.gradient<<" mean x"<<panCas.mean<<" overshoot "<<panCas.overshoot<<std::endl;
 Check(staticCas.gradient>1.05&&panCas.gradient>1.05,"CAS sharpens the resolved image");
 Check(std::abs(staticCas.mean-1)<.01&&std::abs(panCas.mean-1)<.01,"CAS keeps the mean");
 // Sharpening necessarily lifts local extrema past their neighbours; what must stay small is
 // the mean excursion at strong edges (halos) and the added frame-to-frame shimmer.
 Check(staticCas.halo<.015&&panCas.halo<.015,"CAS halos stay small (mean excursion past the local range at strong edges)");
 Check(casFlicker[1]<1.25*casFlicker[0],"CAS adds little shimmer on a static jittered view");
 // 7. GPU time at 1920x1080 (fixture GPU, not the game): minimum of rounds.
 double resolveMs=1e9,casMs=1e9;
 for(int sharpenRun=0;sharpenRun<2;++sharpenRun){double &timedMs=sharpenRun?casMs:resolveMs;sharpenSetting=sharpenRun?casStrength:0;
  gpu.Create(1920,1080);TemporalAAGPU timed;HR(timed.Create(gpu.device.Get(),1920,1080),"timed create");Tune(timed);sharpenSetting=0;
  Image frame;frame.rgba.assign(std::size_t(1920)*1080*4,.5f);frame.depth.assign(std::size_t(1920)*1080,.5f);gpu.Upload(frame);
  TemporalAAFrame f=FrameOf(still,.1f,-.2f);f.viewport[0]=f.viewport[1]=0;f.viewport[2]=1920;f.viewport[3]=1080;
  HR(timed.Resolve(gpu.context.Get(),gpu.color.Get(),gpu.colorView.Get(),gpu.depthView.Get(),f),"warm resolve");
  ComPtr<ID3D11Query> disjoint,start,end;D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};HR(gpu.device->CreateQuery(&q,disjoint.GetAddressOf()),"query");q.Query=D3D11_QUERY_TIMESTAMP;
  HR(gpu.device->CreateQuery(&q,start.GetAddressOf()),"query");HR(gpu.device->CreateQuery(&q,end.GetAddressOf()),"query");
  for(int round=0;round<30;++round){gpu.context->Begin(disjoint.Get());gpu.context->End(start.Get());
   for(int r=0;r<10;++r)timed.Resolve(gpu.context.Get(),gpu.color.Get(),gpu.colorView.Get(),gpu.depthView.Get(),f,gpu.colorTarget.Get());
   gpu.context->End(end.Get());gpu.context->End(disjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};while(gpu.context->GetData(disjoint.Get(),&d,sizeof(d),0)!=S_OK){}
   UINT64 a=0,b=0;while(gpu.context->GetData(start.Get(),&a,8,0)!=S_OK){}while(gpu.context->GetData(end.Get(),&b,8,0)!=S_OK){}if(!d.Disjoint)timedMs=std::min(timedMs,double(b-a)/double(d.Frequency)*1000/10);}
  Check(timedMs<1e8,"timed resolve");}
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"jitter_projection_error_px\":"<<jitterWorst<<",\"jitter_mean\":["<<jitterMean[0]<<','<<jitterMean[1]<<"]"
  <<",\"static\":{\"aliased_edge_error\":"<<aliasedError.edge<<",\"taa_edge_error\":"<<staticError.edge<<",\"edge_error_ratio\":"<<staticError.edge/aliasedError.edge<<",\"aliased_flat_error\":"<<aliasedError.flat<<",\"taa_flat_error\":"<<staticError.flat
  <<",\"edge_pixels\":"<<staticError.edges<<",\"raw_jitter_flicker\":"<<rawFlicker<<",\"taa_flicker\":"<<taaFlicker<<",\"firefly_peak_tone\":"<<fireflyPeak<<",\"firefly_next_frame_error\":"<<fireflyError<<"}"
  <<",\"pans\":[";
 for(int i=0;i<2;++i)std::cout<<(i?",":"")<<"{\"pan\":\""<<pans[i].name<<"\",\"aliased_edge_error\":"<<pans[i].aliased.edge<<",\"taa_edge_error\":"<<pans[i].error.edge<<",\"taa_flat_error\":"<<pans[i].error.flat<<"}";
 std::cout<<"],\"moving_unit\":{\"trail_pixels\":"<<trailPixels<<",\"trail_error\":"<<trailError<<",\"trail_error_max\":"<<trailMax<<",\"edge_error\":"<<unitError.edge<<",\"flat_error\":"<<unitError.flat<<"}"
  <<",\"cas\":{\"static_gradient\":"<<staticCas.gradient<<",\"static_mean\":"<<staticCas.mean<<",\"static_overshoot\":"<<staticCas.overshoot
  <<",\"static_halo\":"<<staticCas.halo<<",\"pan_gradient\":"<<panCas.gradient<<",\"pan_mean\":"<<panCas.mean<<",\"pan_overshoot\":"<<panCas.overshoot
  <<",\"pan_halo\":"<<panCas.halo<<",\"edge_fraction\":"<<staticCas.edges<<",\"flicker_off\":"<<casFlicker[0]<<",\"flicker_on\":"<<casFlicker[1]<<",\"strength\":"<<casStrength<<"}"<<",\"gpu_ms_1080p_resolve\":"<<resolveMs<<",\"gpu_ms_1080p_resolve_cas\":"<<casMs<<",\"resets\":"<<taa.Stats().resets<<"}"<<std::endl;
 return 0;
}catch(const std::exception &e){std::cout<<"{\"passed\":false,\"checks\":"<<checks<<",\"error\":\""<<e.what()<<"\"}"<<std::endl;return 1;}}
