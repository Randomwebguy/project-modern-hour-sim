// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeImage.h"
#include <array>
#include <fstream>
#include <new>
#include <utility>
#include <algorithm>
#include <cmath>
namespace GenZH::Modern {
bool DecodeTGA(std::istream &input,ImageRGBA &result,std::string &error) {
    result={};error.clear();
    auto fail=[&](const char *why){error=why;return false;};
    std::array<std::uint8_t,18> header{};
    if(!input.read(reinterpret_cast<char*>(header.data()),header.size()))return fail("truncated TGA header");
    const bool gray=header[2]==3||header[2]==11,rle=header[2]==10||header[2]==11;
    if(header[1]!=0||!(gray||header[2]==2||header[2]==10))return fail("unsupported TGA image/color map type");
    const unsigned bits=header[16];
    if((gray&&bits!=8)||(!gray&&bits!=24&&bits!=32)||(header[17]&0xc0))return fail("unsupported TGA pixel/interleave format");
    ImageRGBA image;
    image.width=header[12]|(std::uint32_t(header[13])<<8);image.height=header[14]|(std::uint32_t(header[15])<<8);
    const std::uint64_t count=std::uint64_t(image.width)*image.height;
    if(!count||image.width>8192||image.height>8192||count>64ull*1024*1024)return fail("invalid or oversized TGA dimensions");
    if(header[0]&&!input.ignore(header[0]))return fail("truncated TGA image ID");
    try {image.pixels.resize(static_cast<std::size_t>(count)*4);}
    catch(const std::bad_alloc&){return fail("TGA allocation failed");}
    auto pixel=[&](std::array<std::uint8_t,4> &rgba){
        std::array<std::uint8_t,4> raw{};
        if(!input.read(reinterpret_cast<char*>(raw.data()),bits/8))return false;
        rgba=gray?std::array<std::uint8_t,4>{raw[0],raw[0],raw[0],255}:std::array<std::uint8_t,4>{raw[2],raw[1],raw[0],bits==32?raw[3]:std::uint8_t(255)};
        return true;
    };
    auto put=[&](std::uint64_t index,const std::array<std::uint8_t,4> &rgba){
        auto x=index%image.width,y=index/image.width;
        if(header[17]&0x10)x=image.width-1-x;
        if(!(header[17]&0x20))y=image.height-1-y;
        auto offset=static_cast<std::size_t>((y*image.width+x)*4);
        for(unsigned c=0;c<4;++c)image.pixels[offset+c]=rgba[c];
    };
    if(!rle){
        std::array<std::uint8_t,8192*4> row{};
        const unsigned channels=bits/8,rowBytes=image.width*channels;
        for(std::uint32_t y=0;y<image.height;++y){
            if(!input.read(reinterpret_cast<char*>(row.data()),rowBytes))return fail("truncated TGA row");
            for(std::uint32_t x=0;x<image.width;++x){
                const auto at=x*channels;
                std::array<std::uint8_t,4> rgba=gray?std::array<std::uint8_t,4>{row[at],row[at],row[at],255}:std::array<std::uint8_t,4>{row[at+2],row[at+1],row[at],channels==4?row[at+3]:std::uint8_t(255)};
                put(std::uint64_t(y)*image.width+x,rgba);
            }
        }
        result=std::move(image);return true;
    }
    std::uint64_t written=0;
    while(written<count){
        unsigned length=1;bool repeated=false;
        if(rle){int packet=input.get();if(packet==std::char_traits<char>::eof())return fail("truncated TGA RLE packet");length=(packet&127)+1;repeated=(packet&128)!=0;}
        if(length>count-written)return fail("TGA packet exceeds image bounds");
        std::array<std::uint8_t,4> rgba{};
        if(repeated&&!pixel(rgba))return fail("truncated TGA repeated pixel");
        for(unsigned i=0;i<length;++i){
            if(!repeated&&!pixel(rgba))return fail("truncated TGA pixels");
            put(written++,rgba);
        }
    }
    result=std::move(image);return true;
}
bool LoadTGA(const std::filesystem::path &path,ImageRGBA &result,std::string &error){
    std::ifstream file(path,std::ios::binary);
    if(!file){result={};error="cannot open TGA";return false;}
    return DecodeTGA(file,result,error);
}
bool ResizeRGBA(const ImageRGBA &source,std::uint32_t width,std::uint32_t height,ImageRGBA &result){
    if(!source.width||!source.height||source.width>8192||source.height>8192||!width||!height||width>8192||height>8192||
       source.pixels.size()!=static_cast<std::size_t>(source.width)*source.height*4)return false;
    if(source.width==width&&source.height==height){result=source;return true;}
    ImageRGBA target;target.width=width;target.height=height;target.pixels.resize(static_cast<std::size_t>(width)*height*4);
    struct Coordinate {std::uint32_t a,b;float fraction;};
    auto coordinates=[](std::uint32_t from,std::uint32_t to){std::vector<Coordinate> map(to);
        for(std::uint32_t i=0;i<to;++i){const float x=std::max(0.0f,std::min(static_cast<float>(from-1),(i+.5f)*from/to-.5f));
            const auto a=static_cast<std::uint32_t>(x);map[i]={a,std::min(from-1,a+1),x-a};}return map;};
    const auto xs=coordinates(source.width,width),ys=coordinates(source.height,height);
    for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){const auto &cx=xs[x],&cy=ys[y];
        for(unsigned c=0;c<4;++c){auto at=[&](std::uint32_t sx,std::uint32_t sy){return source.pixels[(static_cast<std::size_t>(sy)*source.width+sx)*4+c];};
            const float a=at(cx.a,cy.a)*(1-cx.fraction)+at(cx.b,cy.a)*cx.fraction;
            const float b=at(cx.a,cy.b)*(1-cx.fraction)+at(cx.b,cy.b)*cx.fraction;
            target.pixels[(static_cast<std::size_t>(y)*width+x)*4+c]=static_cast<std::uint8_t>(std::lround(a*(1-cy.fraction)+b*cy.fraction));}
    }
    result=std::move(target);return true;
}
}
