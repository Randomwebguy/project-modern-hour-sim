// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeImage.h"
#include "ShaderBytecodeCache.h"
#include "LoadTelemetry.h"
namespace GenZH::Modern {
inline void TrimTerrainTiles(const std::filesystem::path &directory,const std::filesystem::path &keep,std::uint64_t budget=512ull*1024*1024){
 struct File{std::filesystem::path path;std::filesystem::file_time_type modified;std::uint64_t bytes;};std::vector<File> files;std::uint64_t total=0;std::error_code error;
 for(std::filesystem::directory_iterator it(directory,error),end;!error&&it!=end;it.increment(error)){
  const auto path=it->path();if(path.extension()!=L".tile"||path.filename().string().rfind("v17-",0)!=0||it->is_symlink(error))continue;
  std::ifstream stream(path,std::ios::binary);std::uint32_t magic=0;if(!stream.read(reinterpret_cast<char*>(&magic),4)||magic!=0x3731545a)continue;
  const auto bytes=it->file_size(error);if(error)break;const auto modified=it->last_write_time(error);if(error)break;
  total+=bytes;files.push_back({path,modified,bytes});
 }
 std::sort(files.begin(),files.end(),[](const File &a,const File &b){return a.modified<b.modified;});
 for(const auto &file:files){if(total<=budget)break;if(file.path==keep)continue;if(std::filesystem::remove(file.path,error))total-=file.bytes;error.clear();}
}
// Cache the exact resized CPU tile, rather than decoding a 4K TGA on every map.
inline std::string TerrainTileKey(const std::filesystem::path &source,UINT size){
 WIN32_FILE_ATTRIBUTE_DATA attr{};if(!GetFileAttributesExW(source.c_str(),GetFileExInfoStandard,&attr))return {};
 auto name=std::filesystem::absolute(source).lexically_normal().generic_u8string();for(char &c:name)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
 std::ostringstream key;key<<"tile17 bilinear clamp RGBA8 "<<size<<' '<<attr.nFileSizeHigh<<':'<<attr.nFileSizeLow<<' '<<attr.ftLastWriteTime.dwHighDateTime<<':'<<attr.ftLastWriteTime.dwLowDateTime<<' '<<name;return key.str();
}
inline bool ReadTerrainTile(const std::filesystem::path &path,const std::string &key,UINT size,std::vector<std::uint8_t> &pixels){
 struct Header{std::uint32_t magic,keyBytes,width,height;std::uint64_t hash;};
 pixels.clear();if(!size||size>4096||key.size()>32768)return false;
 std::ifstream file(path,std::ios::binary);Header h{};if(!file.read(reinterpret_cast<char*>(&h),sizeof(h))||h.magic!=0x3731545a||h.keyBytes!=key.size()||h.width!=size||h.height!=size)return false;
 std::string saved(h.keyBytes,'\0');if(!file.read(saved.data(),h.keyBytes)||saved!=key)return false;
 pixels.resize(static_cast<std::size_t>(size)*size*4);
 if(!file.read(reinterpret_cast<char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()))||file.peek()!=EOF||ShaderHash(pixels.data(),pixels.size())!=h.hash){pixels.clear();return false;}return true;
}
inline void SaveTerrainTile(const std::filesystem::path &path,const std::string &key,UINT size,const std::vector<std::uint8_t> &pixels){
 if(path.empty()||!size||size>4096||key.size()>32768||pixels.size()!=static_cast<std::size_t>(size)*size*4)return;
 struct Header{std::uint32_t magic,keyBytes,width,height;std::uint64_t hash;};
 const Header h{0x3731545a,static_cast<UINT>(key.size()),size,size,ShaderHash(pixels.data(),pixels.size())};
 std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error)return;
 auto temporary=path;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetCurrentThreadId())+L".tmp";
 {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);if(!file)return;file.write(reinterpret_cast<const char*>(&h),sizeof(h));file.write(key.data(),h.keyBytes);file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));if(!file)return;}
 MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
 TrimTerrainTiles(path.parent_path(),path);
}
inline bool LoadTerrainTile(const std::filesystem::path &source,UINT size,std::vector<std::uint8_t> &pixels,std::string &error){
 GenZHLoad::Scope timing("terrain_tile",source.filename().string());
 const auto key=TerrainTileKey(source,size);if(key.empty())return false;
 wchar_t folder[32768]{};DWORD n=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",folder,32768);std::filesystem::path cache;
 if(n&&n<32768){std::ostringstream name;name<<"v17-"<<std::hex<<ShaderHash(key.data(),key.size())<<".tile";cache=std::filesystem::path(folder)/L"terrain-tile-cache"/name.str();}
 if(!cache.empty()&&ReadTerrainTile(cache,key,size,pixels)){timing.hit=true;timing.bytes=pixels.size();return true;}
 ImageRGBA original,resized;{GenZHLoad::Scope decode("texture_decode",source.filename().string());if(!LoadTGA(source,original,error))return false;decode.bytes=original.pixels.size();}
 if(!ResizeRGBA(original,size,size,resized)){error="invalid terrain resize";return false;}
 // Recheck the source stamp; never persist a tile under a stale key.
 pixels=std::move(resized.pixels);timing.bytes=pixels.size();
 if(!cache.empty()&&key==TerrainTileKey(source,size))SaveTerrainTile(cache,key,size,pixels);return true;
}
}
