// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainTileCache.h"
#include <iostream>
#include <stdexcept>
using namespace GenZH::Modern;
int main(int argc,char **argv){try{
 if(argc!=3)throw std::runtime_error("usage: terrain-prewarm profile-directory materials-directory");
 const auto profile=std::filesystem::absolute(argv[1]),root=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(profile);
 SetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",profile.c_str());unsigned tiles=0,sources=0,hits=0;const auto started=GenZHLoad::Seconds();
 for(const auto &entry:std::filesystem::directory_iterator(root)){
  const auto source=entry.path();if(source.extension()!=L".tga"||source.filename().string().rfind("tm",0)!=0)continue;
  ImageRGBA image;std::string error;
  for(UINT size:{128u,256u,512u}){
   const auto key=TerrainTileKey(source,size);std::ostringstream name;name<<"v17-"<<std::hex<<ShaderHash(key.data(),key.size())<<".tile";
   const auto cache=profile/L"terrain-tile-cache"/name.str();std::vector<std::uint8_t> pixels;
   if(ReadTerrainTile(cache,key,size,pixels)){++hits;++tiles;continue;}
   if(image.pixels.empty()&&!LoadTGA(source,image,error))throw std::runtime_error(source.filename().string()+": "+error);
   ImageRGBA resized;if(!ResizeRGBA(image,size,size,resized))throw std::runtime_error("terrain resize");
   if(key!=TerrainTileKey(source,size))throw std::runtime_error("source changed during preparation");
   SaveTerrainTile(cache,key,size,resized.pixels);if(!ReadTerrainTile(cache,key,size,pixels)||pixels!=resized.pixels)throw std::runtime_error("tile cache save verification");++tiles;
  }
  ++sources;
 }
 std::cout<<"{\"passed\":true,\"sources\":"<<sources<<",\"tiles\":"<<tiles<<",\"cache_hits\":"<<hits<<",\"milliseconds\":"<<(GenZHLoad::Seconds()-started)*1000<<",\"game_started\":false}\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
