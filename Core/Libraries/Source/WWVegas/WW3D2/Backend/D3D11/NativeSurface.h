// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeImage.h"
#include <cstring>
namespace GenZH::Modern {
enum class SurfaceLayout { BGRA8,BGRX8,BGRA1555,BGR565,BGRA4444 };
inline bool CopySurfaceRGBA(const std::uint8_t *bytes,std::size_t available,std::uint32_t width,std::uint32_t height,std::uint32_t pitch,SurfaceLayout layout,ImageRGBA &out){
 const std::uint32_t pixelSize=layout==SurfaceLayout::BGRA8||layout==SurfaceLayout::BGRX8?4:2;
 if(!bytes||!width||!height||width>8192||height>8192||pitch<width*pixelSize||available<static_cast<std::size_t>(height-1)*pitch+width*pixelSize)return false;
 ImageRGBA image;image.width=width;image.height=height;image.pixels.resize(static_cast<std::size_t>(width)*height*4);
 for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){
  const auto *in=bytes+static_cast<std::size_t>(y)*pitch+x*pixelSize;auto *p=image.pixels.data()+(static_cast<std::size_t>(y)*width+x)*4;
  if(pixelSize==4){p[0]=in[2];p[1]=in[1];p[2]=in[0];p[3]=layout==SurfaceLayout::BGRA8?in[3]:255;}
  else{std::uint16_t value=0;std::memcpy(&value,in,2);
   auto five=[](unsigned v){return static_cast<std::uint8_t>((v<<3)|(v>>2));};
   if(layout==SurfaceLayout::BGRA1555){p[0]=five((value>>10)&31);p[1]=five((value>>5)&31);p[2]=five(value&31);p[3]=value&0x8000?255:0;}
   else if(layout==SurfaceLayout::BGR565){p[0]=five((value>>11)&31);const unsigned green=(value>>5)&63;p[1]=static_cast<std::uint8_t>((green<<2)|(green>>4));p[2]=five(value&31);p[3]=255;}
   else{p[0]=static_cast<std::uint8_t>(((value>>8)&15)*17);p[1]=static_cast<std::uint8_t>(((value>>4)&15)*17);p[2]=static_cast<std::uint8_t>((value&15)*17);p[3]=static_cast<std::uint8_t>((value>>12)*17);}
  }
 }
 out=std::move(image);return true;
}
}
