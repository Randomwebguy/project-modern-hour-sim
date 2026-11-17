// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeImage.h"
#include <windows.h>
#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
using namespace GenZH::Modern;
static void Require(bool condition,const char *what){if(!condition)throw std::runtime_error(what);}
static std::string Header(unsigned width,unsigned height,unsigned type=2,unsigned bits=32,unsigned origin=32){
    std::string h(18,'\0');h[2]=static_cast<char>(type);h[12]=static_cast<char>(width);h[13]=static_cast<char>(width>>8);h[14]=static_cast<char>(height);h[15]=static_cast<char>(height>>8);h[16]=static_cast<char>(bits);h[17]=static_cast<char>(origin);return h;
}
static bool Decode(const std::string &bytes,ImageRGBA &image){std::istringstream input(bytes,std::ios::binary);std::string error;return DecodeTGA(input,image,error);}
static std::uint32_t CRC(const std::vector<std::uint8_t> &bytes){
    std::array<std::uint32_t,256> table{};
    for(unsigned i=0;i<256;++i){std::uint32_t c=i;for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1)?0xedb88320u:0);table[i]=c;}
    std::uint32_t c=0xffffffffu;for(auto b:bytes)c=table[(c^b)&255]^(c>>8);return c^0xffffffffu;
}
static void Fixtures(){
    const std::array<std::array<std::uint8_t,4>,4> expected={{{11,21,31,41},{12,22,32,42},{13,23,33,43},{14,24,34,44}}};
    for(unsigned origin: {0u,16u,32u,48u}){
        std::string file=Header(2,2,2,32,origin);
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x){auto sx=(origin&16)?1-x:x,sy=(origin&32)?y:1-y;const auto &p=expected[sy*2+sx];for(unsigned c: {2u,1u,0u,3u})file+=static_cast<char>(p[c]);}
        ImageRGBA image;Require(Decode(file,image)&&image.width==2&&image.height==2,"TGA origin decode");
        Require(std::memcmp(image.pixels.data(),expected.data(),16)==0,"TGA orientation or RGBA mismatch");
        Require(!Decode(file.substr(0,file.size()-1),image)&&image.pixels.empty(),"truncated image accepted or partial image leaked");
    }
    ImageRGBA image;
    std::string rgb=Header(1,1,2,24);rgb+=char(31);rgb+=char(21);rgb+=char(11);Require(Decode(rgb,image)&&image.pixels==std::vector<std::uint8_t>({11,21,31,255}),"RGB alpha decode");
    std::string gray=Header(2,1,11,8);gray+='\x81';gray+=char(77);Require(Decode(gray,image)&&image.pixels==std::vector<std::uint8_t>({77,77,77,255,77,77,77,255}),"grayscale RLE decode");
    std::string rle=Header(3,1,10,24);rle+='\x81';rle+=char(3);rle+=char(2);rle+=char(1);rle+=char(0);rle+=char(6);rle+=char(5);rle+=char(4);
    Require(Decode(rle,image)&&image.pixels==std::vector<std::uint8_t>({1,2,3,255,1,2,3,255,4,5,6,255}),"literal/repeated RLE decode");
    std::string bad=Header(1,1,10,24);bad+='\x81';bad+=char(1);bad+=char(2);bad+=char(3);Require(!Decode(bad,image),"oversized RLE packet accepted");
    Require(!Decode(Header(65535,65535),image),"dimension overflow accepted");
    Require(!Decode(Header(0,1),image),"zero width accepted");
    Require(!Decode(Header(1,1,2,16),image),"unsupported pixel format accepted");
    bad=Header(1,1);bad[1]=1;Require(!Decode(bad,image),"palette map accepted");
    bad=Header(1,1);bad[0]=4;Require(!Decode(bad,image),"truncated ID accepted");
}
int wmain(int argc,wchar_t **argv){try{
    Fixtures();bool highAddress=false;
#if defined(_WIN64)
    static_assert(sizeof(void*)==8);
    void *memory=VirtualAlloc(reinterpret_cast<void*>(0x190000000ull),65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    Require(memory!=nullptr,"x64 allocation above 4GiB failed");
    auto *p=static_cast<volatile unsigned char*>(memory);p[0]=0x5a;p[65535]=0xa5;Require(p[0]==0x5a&&p[65535]==0xa5,"high address write/read failed");
    VirtualFree(memory,0,MEM_RELEASE);highAddress=true;
#endif
    std::cout<<"{\"pointer_bytes\":"<<sizeof(void*)<<",\"allocation_above_4GiB\":"<<(highAddress?"true":"false")<<",\"fixtures_passed\":true,\"images\":[";
    bool first=true;unsigned count=0;
    if(argc>1)for(const auto &entry:std::filesystem::directory_iterator(argv[1])){
        if(entry.path().extension()!=L".tga")continue;
        ImageRGBA image;std::string error;Require(LoadTGA(entry.path(),image,error),error.c_str());
        if(!first)std::cout<<',';first=false;++count;
        std::cout<<"{\"file\":\""<<entry.path().filename().u8string()<<"\",\"width\":"<<image.width<<",\"height\":"<<image.height<<",\"rgba_crc32\":"<<CRC(image.pixels)<<'}';
    }
    std::cout<<"],\"image_count\":"<<count<<"}\n";return 0;
}catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
