// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <filesystem>
namespace GenZH::Modern {
constexpr unsigned CloudNoiseSide=128;
constexpr std::size_t CloudNoiseBytes=std::size_t(CloudNoiseSide)*CloudNoiseSide*CloudNoiseSide*2;
constexpr unsigned CloudNoiseMips=8;
constexpr std::size_t CloudNoiseMipBytes=4793490;
// Filter the immutable volume once at resource creation. Keep full-resolution
// billows close to the camera; distant ray footprints use energy-preserving mips.
inline std::array<std::vector<std::uint8_t>,CloudNoiseMips> CloudNoisePyramid(std::vector<std::uint8_t> base){
 std::array<std::vector<std::uint8_t>,CloudNoiseMips> levels;levels[0]=std::move(base);
 for(unsigned mip=1;mip<CloudNoiseMips;++mip){const unsigned side=CloudNoiseSide>>mip,parent=side*2;levels[mip].resize(std::size_t(side)*side*side*2);
  for(unsigned z=0;z<side;++z)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned channel=0;channel<2;++channel){unsigned sum=0;
   for(unsigned i=0;i<8;++i){const unsigned px=x*2+(i&1),py=y*2+((i>>1)&1),pz=z*2+((i>>2)&1);sum+=levels[mip-1][(std::size_t(pz)*parent*parent+py*parent+px)*2+channel];}
   levels[mip][(std::size_t(z)*side*side+y*side+x)*2+channel]=static_cast<std::uint8_t>((sum+4)/8);
  }
 }
 return levels;
}
inline std::uint32_t CloudNoiseHash(std::uint32_t v){v^=v>>16;v*=0x7feb352d;v^=v>>15;v*=0x846ca68b;return v^(v>>16);}
inline std::uint32_t CloudCellHash(int x,int y,int z,unsigned period){
 const auto wrap=[&](int v){return static_cast<unsigned>(v)&(period-1);};return CloudNoiseHash(wrap(x)+period*(wrap(y)+period*wrap(z)));
}
inline float CloudGradientNoise(float x,float y,float z,unsigned period){
 const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y)),iz=static_cast<int>(std::floor(z));
 const float fx=x-ix,fy=y-iy,fz=z-iz;
 auto fade=[](float f){return f*f*f*(f*(f*6-15)+10);};const float ax=fade(fx),ay=fade(fy),az=fade(fz);float sum=0;
 constexpr float gradients[12][3]={{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},{0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1}};
 for(unsigned i=0;i<8;++i){const auto *g=gradients[CloudCellHash(ix+(i&1),iy+((i>>1)&1),iz+((i>>2)&1),period)%12];
  const float value=(g[0]*(fx-float(i&1))+g[1]*(fy-float((i>>1)&1))+g[2]*(fz-float((i>>2)&1)))*.70710678f;
  sum+=value*(i&1?ax:1-ax)*(i&2?ay:1-ay)*(i&4?az:1-az);
 }
 return sum;
}
inline float CloudWorleyNoise(float x,float y,float z,unsigned period){
 const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y)),iz=static_cast<int>(std::floor(z));float closest=4;
 for(int dz=-1;dz<=1;++dz)for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
  auto h=CloudCellHash(ix+dx,iy+dy,iz+dz,period);
  // A periodic jittered feature inside every cell; measure in unwrapped world.
  const float px=ix+dx+.1f+.8f*float(h&1023)/1023;
  const float py=iy+dy+.1f+.8f*float((h>>10)&1023)/1023;
  const float pz=iz+dz+.1f+.8f*float((h>>20)&1023)/1023;
  const float sx=px-x,sy=py-y,sz=pz-z;closest=(std::min)(closest,sx*sx+sy*sy+sz*sz);
 }
 return 1-std::clamp(std::sqrt(closest)/1.4f,0.0f,1.0f);
}
inline std::vector<std::uint8_t> GenerateCloudNoise(){
 std::vector<std::uint8_t> pixels(CloudNoiseBytes);
 for(unsigned z=0;z<CloudNoiseSide;++z)for(unsigned y=0;y<CloudNoiseSide;++y)for(unsigned x=0;x<CloudNoiseSide;++x){
  const float u=(x+.5f)/CloudNoiseSide,v=(y+.5f)/CloudNoiseSide,w=(z+.5f)/CloudNoiseSide;
  const float perlin=std::clamp(.5f+.8f*(.625f*CloudGradientNoise(u*4,v*4,w*4,4)+.25f*CloudGradientNoise(u*8,v*8,w*8,8)+.125f*CloudGradientNoise(u*16,v*16,w*16,16)),0.0f,1.0f);
  const float w4=CloudWorleyNoise(u*4,v*4,w*4,4),w8=CloudWorleyNoise(u*8,v*8,w*8,8),w16=CloudWorleyNoise(u*16,v*16,w*16,16),w32=CloudWorleyNoise(u*32,v*32,w*32,32);
  const float billows=.4f*w4+.4f*w8+.2f*w16;
  const float base=.25f*perlin+.75f*billows;
  const float detail=.7f*w16+.3f*w32;const auto at=(std::size_t(z)*CloudNoiseSide*CloudNoiseSide+y*CloudNoiseSide+x)*2;
  pixels[at]=static_cast<std::uint8_t>(std::clamp(base,0.0f,1.0f)*255+.5f);pixels[at+1]=static_cast<std::uint8_t>(std::clamp(detail,0.0f,1.0f)*255+.5f);
 }
 return pixels;
}
inline std::uint64_t CloudNoiseDigest(const std::vector<std::uint8_t> &pixels){std::uint64_t h=14695981039346656037ull;for(auto b:pixels)h=(h^b)*1099511628211ull;return h;}
struct CloudNoiseHeader {std::uint32_t magic=0x36334e43,version=36,side=CloudNoiseSide,channels=2;std::uint64_t hash=0;};
static_assert(sizeof(CloudNoiseHeader)==24);
inline bool LoadCloudNoise(const std::filesystem::path &path,std::vector<std::uint8_t> &pixels){
 pixels.clear();CloudNoiseHeader h;std::ifstream file(path,std::ios::binary);
 if(!file.read(reinterpret_cast<char*>(&h),sizeof(h))||h.magic!=0x36334e43||h.version!=36||h.side!=CloudNoiseSide||h.channels!=2)return false;
 pixels.resize(CloudNoiseBytes);if(!file.read(reinterpret_cast<char*>(pixels.data()),pixels.size())||file.peek()!=EOF||CloudNoiseDigest(pixels)!=h.hash){pixels.clear();return false;}return true;
}
inline bool SaveCloudNoise(const std::filesystem::path &path,const std::vector<std::uint8_t> &pixels){
 if(pixels.size()!=CloudNoiseBytes)return false;CloudNoiseHeader h;h.hash=CloudNoiseDigest(pixels);
 std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(&h),sizeof(h));file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());return bool(file);
}
}
