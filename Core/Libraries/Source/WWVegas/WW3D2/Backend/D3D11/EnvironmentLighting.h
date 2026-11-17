// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Offline distant-IBL preparation. No game clock, random source, device or
// simulation dependency. GGX split-sum and three-band diffuse irradiance SH.
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
namespace GenZH::Modern {
namespace IBL {
constexpr float Pi=3.14159265358979323846f;
using Vec=std::array<float,3>;
inline Vec Scale(Vec v,float k){for(auto &x:v)x*=k;return v;}
inline Vec Add(Vec a,Vec b){for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline float Dot(Vec a,Vec b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline Vec Unit(Vec a){return Scale(a,1/std::sqrt(std::max(Dot(a,a),1e-20f)));}
inline Vec Cross(Vec a,Vec b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline Vec CubeDirection(unsigned face,float u,float v){
 switch(face){case 0:return Unit({1,-v,-u});case 1:return Unit({-1,-v,u});case 2:return Unit({u,1,v});case 3:return Unit({u,-1,-v});case 4:return Unit({u,-v,1});default:return Unit({-u,-v,-1});}
}
inline std::array<float,9> SH(Vec n){return {.2820947918f,.4886025119f*n[1],.4886025119f*n[2],.4886025119f*n[0],1.092548431f*n[0]*n[1],1.092548431f*n[1]*n[2],.3153915653f*(3*n[2]*n[2]-1),1.092548431f*n[0]*n[2],.5462742153f*(n[0]*n[0]-n[1]*n[1])};}
inline float RadicalInverse(std::uint32_t bits){
 bits=(bits<<16)|(bits>>16);bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
 bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);return static_cast<float>(bits)*2.3283064365386963e-10f;
}
inline Vec SampleGGX(unsigned i,unsigned samples,float roughness){
 const float a=roughness*roughness,phi=2*Pi*static_cast<float>(i)/samples,xi=RadicalInverse(i);
 const float z=std::sqrt((1-xi)/(1+(a*a-1)*xi)),r=std::sqrt(std::max(0.f,1-z*z));return {r*std::cos(phi),r*std::sin(phi),z};
}
inline float SmithVisibility(float nv,float nl,float alpha){
 const float a2=alpha*alpha;
 const float gv=nl*std::sqrt(nv*nv*(1-a2)+a2),gl=nv*std::sqrt(nl*nl*(1-a2)+a2);
 return .5f/std::max(gv+gl,1e-12f);
}
// Double-precision visible-GGX integration (Heitz 2018). Bounded G2/G1
// weights avoid the grazing-angle variance of sampling the full NDF.
inline std::array<float,2> IntegrateBRDF(float nv,float roughness,unsigned samples){
 constexpr double pi=3.14159265358979323846;
 const double cosine=std::clamp(double(nv),1e-4,1.),vx=std::sqrt(1-cosine*cosine);
 const double alpha=double(roughness)*roughness,a2=alpha*alpha;
 const double stretchedLength=std::sqrt(alpha*alpha*vx*vx+cosine*cosine);
 const double xh=alpha*vx/stretchedLength,zh=cosine/stretchedLength;
 const double rootV=std::sqrt(cosine*cosine*(1-a2)+a2);
 double sum[2]{};
 for(unsigned i=0;i<samples;++i){
  const double radius=std::sqrt(double(i)/samples),phi=2*pi*RadicalInverse(i);
  const double t1=radius*std::cos(phi),blend=.5*(1+zh);
  const double t2=(1-blend)*std::sqrt(std::max(0.,1-t1*t1))+blend*radius*std::sin(phi);
  const double diskZ=std::sqrt(std::max(0.,1-t1*t1-t2*t2));
  const double nx=alpha*(-zh*t2+xh*diskZ),ny=alpha*t1,nz=std::max(0.,xh*t2+zh*diskZ);
  const double inverse=1/std::sqrt(std::max(nx*nx+ny*ny+nz*nz,1e-30));
  const double hx=nx*inverse,nh=nz*inverse,vh=std::max(0.,vx*hx+cosine*nh);
  const double nl=std::max(0.,2*vh*nh-cosine);if(nl<=0)continue;
  const double gv=nl*rootV,gl=cosine*std::sqrt(nl*nl*(1-a2)+a2);
  const double visibility=.5/std::max(gv+gl,1e-30);
  const double weight=2*nl*(cosine+rootV)*visibility,f=std::pow(1-vh,5.);
  sum[0]+=(1-f)*weight;sum[1]+=f*weight;
 }
 return {static_cast<float>(sum[0]/samples),static_cast<float>(sum[1]/samples)};
}

struct Data {
 unsigned size=0,mips=0,lutSize=0;
 std::array<std::array<float,4>,9> sh{}; // E/pi coefficients, not raw radiance SH.
 std::vector<std::array<float,4>> radiance; // face-major, then roughness mip.
 std::vector<std::array<float,2>> brdf;
 std::size_t Bytes()const{return radiance.size()*16+brdf.size()*8;}
};
inline std::size_t CubeTexels(unsigned size,unsigned mips){std::size_t n=0;for(unsigned m=0;m<mips;++m)n+=std::size_t(std::max(1u,size>>m))*std::max(1u,size>>m);return n*6;}
inline float SolidAngle(float u,float v){return std::atan2(u*v,std::sqrt(u*u+v*v+1));}
template<class Radiance>Data Build(Radiance sample,unsigned size=32,unsigned lutSize=64,unsigned samples=256){
 Data out;out.size=size;out.lutSize=lutSize;for(unsigned n=size;n;n>>=1)++out.mips;
 out.radiance.reserve(CubeTexels(size,out.mips));out.brdf.resize(std::size_t(lutSize)*lutSize);
 for(unsigned face=0;face<6;++face)for(unsigned mip=0;mip<out.mips;++mip){const unsigned side=std::max(1u,size>>mip);const float roughness=static_cast<float>(mip)/(out.mips-1);
  for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto n=CubeDirection(face,2*(x+.5f)/side-1,2*(y+.5f)/side-1);Vec color{};float total=0;
   if(!mip)color=sample(n);
   else {const auto t=Unit(Cross(std::abs(n[2])<.999f?Vec{0,0,1}:Vec{1,0,0},n)),b=Cross(n,t);
    for(unsigned i=0;i<samples;++i){const auto hLocal=SampleGGX(i,samples,roughness);const auto h=Add(Add(Scale(t,hLocal[0]),Scale(b,hLocal[1])),Scale(n,hLocal[2]));const auto l=Unit(Add(Scale(h,2*Dot(n,h)),Scale(n,-1)));const float weight=std::max(0.f,Dot(n,l));
     if(weight>0){color=Add(color,Scale(sample(l),weight));total+=weight;}}
    color=Scale(color,1/std::max(total,1e-6f));}
   out.radiance.push_back({color[0],color[1],color[2],1});}}
 // Exact cube-texel solid angle, not equal-area cube assumptions.
 for(unsigned face=0;face<6;++face)for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){
  const float u0=2*static_cast<float>(x)/size-1,v0=2*static_cast<float>(y)/size-1,u1=2*static_cast<float>(x+1)/size-1,v1=2*static_cast<float>(y+1)/size-1;
  const float angle=SolidAngle(u1,v1)-SolidAngle(u0,v1)-SolidAngle(u1,v0)+SolidAngle(u0,v0);const auto n=CubeDirection(face,(u0+u1)*.5f,(v0+v1)*.5f),color=sample(n);const auto basis=SH(n);
  for(unsigned i=0;i<9;++i)for(unsigned channel=0;channel<3;++channel)out.sh[i][channel]+=color[channel]*basis[i]*angle;}
 for(unsigned i=0;i<9;++i)for(unsigned channel=0;channel<3;++channel)out.sh[i][channel]*=i==0?1:i<4?2.f/3:1.f/4;
 for(unsigned y=0;y<lutSize;++y)for(unsigned x=0;x<lutSize;++x)out.brdf[std::size_t(y)*lutSize+x]=IntegrateBRDF(1e-4f+(1-1e-4f)*(static_cast<float>(x)/(lutSize-1))*(static_cast<float>(x)/(lutSize-1)),static_cast<float>(y)/(lutSize-1),samples);
 return out;
}
inline Vec Diffuse(const Data &data,Vec n){Vec color{};auto basis=SH(n);for(unsigned i=0;i<9;++i)for(unsigned c=0;c<3;++c)color[c]+=data.sh[i][c]*basis[i];return color;}
inline Vec DistantSky(Vec n){
 const float sky=std::clamp(n[2]*.5f+.5f,0.f,1.f);const Vec ground{.48f,.4f,.31f},zenith{.8f,1.1f,1.6f};
 // Normalized distant fill only; punctual sun stays in the direct-light pass.
 return Add(Scale(ground,1-sky),Scale(zenith,sky));
}
inline bool Valid(const Data &d){
 if(d.size<2||d.size>256||(d.size&(d.size-1))||d.lutSize<2||d.lutSize>512)return false;
 unsigned mips=0;for(unsigned n=d.size;n;n>>=1)++mips;
 if(d.mips!=mips||d.radiance.size()!=CubeTexels(d.size,d.mips)||d.brdf.size()!=std::size_t(d.lutSize)*d.lutSize)return false;
 for(auto &v:d.sh)for(auto x:v)if(!std::isfinite(x)||std::abs(x)>1e6f)return false;
 for(auto &v:d.radiance)for(auto x:v)if(!std::isfinite(x)||x<0||x>1e6f)return false;
 for(auto &v:d.brdf)for(auto x:v)if(!std::isfinite(x)||x<0||x>2)return false;return true;
}
inline std::uint64_t Hash(const void *data,std::size_t size){const auto *p=static_cast<const unsigned char*>(data);std::uint64_t hash=14695981039346656037ull;for(std::size_t i=0;i<size;++i){hash^=p[i];hash*=1099511628211ull;}return hash;}
struct Header {std::uint32_t magic=0x49424c32,version=46,size=0,mips=0,lutSize=0,reserved=0;std::uint64_t bytes=0,hash=0;};
static_assert(sizeof(Header)==40);
inline std::vector<unsigned char> Payload(const Data &d){std::vector<unsigned char> out(sizeof(d.sh)+d.Bytes());auto *p=out.data();std::memcpy(p,d.sh.data(),sizeof(d.sh));p+=sizeof(d.sh);std::memcpy(p,d.radiance.data(),d.radiance.size()*16);p+=d.radiance.size()*16;std::memcpy(p,d.brdf.data(),d.brdf.size()*8);return out;}
inline bool Save(const std::filesystem::path &path,const Data &d){if(!Valid(d))return false;auto bytes=Payload(d);Header h;h.size=d.size;h.mips=d.mips;h.lutSize=d.lutSize;h.bytes=bytes.size();h.hash=Hash(bytes.data(),bytes.size());std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(&h),sizeof(h));f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return bool(f);}
inline bool Load(const std::filesystem::path &path,Data &out){
 std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return false;const auto length=f.tellg();f.seekg(0);Header h;if(!f.read(reinterpret_cast<char*>(&h),sizeof(h)))return false;
 if(h.magic!=0x49424c32||h.version!=46||h.reserved||h.size<2||h.size>256||(h.size&(h.size-1))||h.lutSize<2||h.lutSize>512)return false;
 unsigned mips=0;for(unsigned n=h.size;n;n>>=1)++mips;if(h.mips!=mips)return false;
 const auto expected=sizeof(out.sh)+CubeTexels(h.size,h.mips)*16+std::size_t(h.lutSize)*h.lutSize*8;
 if(h.bytes!=expected||static_cast<std::uint64_t>(length)!=sizeof(h)+h.bytes)return false;
 std::vector<unsigned char> bytes(expected);if(!f.read(reinterpret_cast<char*>(bytes.data()),bytes.size())||Hash(bytes.data(),bytes.size())!=h.hash)return false;
 Data d;d.size=h.size;d.mips=h.mips;d.lutSize=h.lutSize;d.radiance.resize(CubeTexels(h.size,h.mips));d.brdf.resize(std::size_t(h.lutSize)*h.lutSize);
 const auto *p=bytes.data();std::memcpy(d.sh.data(),p,sizeof(d.sh));p+=sizeof(d.sh);std::memcpy(d.radiance.data(),p,d.radiance.size()*16);p+=d.radiance.size()*16;std::memcpy(d.brdf.data(),p,d.brdf.size()*8);
 if(!Valid(d))return false;out=std::move(d);return true;
}
}
}
