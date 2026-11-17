// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainPreparation.h"
#include "TerrainTileCache.h"
#include <cmath>
#include <cstring>
#include <limits>
namespace GenZH::Modern {
namespace {
constexpr std::uint32_t magic=0x4237545a;
struct Header {std::uint32_t magic,version,width,height,keyBytes,upgraded;std::uint64_t hashes[5];};
static_assert(sizeof(Header)==64);
std::filesystem::path CachePath(const TerrainRequest &request,const std::string &key){
 if(request.profile.empty())return {};std::ostringstream name;name<<"v17b-"<<std::hex<<ShaderHash(key.data(),key.size())<<".atlas";return request.profile/L"terrain-atlas-cache"/name.str();
}
bool ReadPixels(std::istream &file,std::vector<std::uint8_t> &pixels,std::size_t size,const std::atomic<bool> &cancel){
 pixels.resize(size);for(std::size_t at=0;at<size;){if(cancel)return false;const auto bytes=std::min(size-at,std::size_t(1024*1024));
  if(!file.read(reinterpret_cast<char*>(pixels.data()+at),static_cast<std::streamsize>(bytes)))return false;at+=bytes;}return true;
}
void TrimCache(const std::filesystem::path &directory,const std::filesystem::path &keep){
 struct File{std::filesystem::path path;std::filesystem::file_time_type time;std::uint64_t bytes;};std::vector<File> files;std::uint64_t total=0;std::error_code error;
 for(std::filesystem::directory_iterator it(directory,error),end;!error&&it!=end;it.increment(error)){
  const auto path=it->path();if(path.extension()!=L".atlas"||path.filename().string().rfind("v17b-",0)!=0||it->is_symlink(error))continue;
  Header header{};std::ifstream stream(path,std::ios::binary);if(!stream.read(reinterpret_cast<char*>(&header),sizeof(header))||header.magic!=magic||header.version!=1)continue;
  const auto size=it->file_size(error);if(error)break;const auto time=it->last_write_time(error);if(error)break;total+=size;files.push_back({path,time,size});
 }
 std::sort(files.begin(),files.end(),[](const File &a,const File &b){return a.time<b.time;});
 for(const auto &file:files){if(total<=1024ull*1024*1024)break;if(file.path==keep)continue;if(std::filesystem::remove(file.path,error))total-=file.bytes;error.clear();}
}
}
std::size_t PreparedTerrain::Bytes()const{std::size_t bytes=cells.size()*sizeof(D3D11TerrainCell)+classes.size()*16;for(const auto &surface:pixels)bytes+=surface.size();return bytes;}
bool ValidateTerrainRequest(const TerrainRequest &r,std::string &error){
 auto fail=[&](const char *why){error=why;return false;};const auto &a=r.atlas;
 if(a.version!=2||!a.width||!a.height||a.width>4096||a.height>4096||a.count>200||!a.cellsX||!a.cellsY||a.cellsX>1024||a.cellsY>1024||!std::isfinite(a.borderWorld))return fail("invalid canonical atlas dimensions");
 if(r.base.width!=a.width||r.base.height!=a.height||r.base.pixels.size()!=static_cast<std::size_t>(a.width)*a.height*4||r.cells.size()!=static_cast<std::size_t>(a.cellsX)*a.cellsY)return fail("inconsistent terrain snapshot");
 for(std::uint32_t i=0;i<a.count;++i){const auto &s=a.regions[i];
  if(!s.extent||s.extent>a.width||s.extent>a.height||s.x<4||s.y<4||static_cast<std::uint64_t>(s.x)+s.extent+4>a.width||static_cast<std::uint64_t>(s.y)+s.extent+4>a.height||s.texture[127]||static_cast<unsigned>(s.ground)>static_cast<unsigned>(D3D11Ground::Wood))return fail("invalid terrain material rectangle/name");
  const auto stem=std::filesystem::path(s.texture).filename().stem();if(stem.empty()||stem==L"."||stem==L"..")return fail("invalid terrain texture name");
 }
 for(const auto &cell:r.cells){for(const auto &uv:cell.coordinates)for(float n:uv)if(!std::isfinite(n))return fail("nonfinite terrain UV");for(float n:cell.alpha)if(!std::isfinite(n)||n<0||n>1)return fail("invalid terrain blend");}
 // Worst-case buffers: five output maps, cells, native snapshot, one decoded
 // 8K RGBA source and one resized tile. Never queue a second preparation.
 const unsigned scale=a.width<=2048&&a.height<=2048?2:1;
 const auto output=static_cast<std::size_t>(a.width)*a.height*scale*scale*4;
 const auto budget=std::max<std::uint64_t>(output*6+r.cells.size()*80+256ull*1024*1024,output+r.cells.size()*80+r.base.pixels.size());
 if(budget>768ull*1024*1024)return fail("terrain preparation exceeds 768 MiB worst-case budget");error.clear();return true;
}
static std::string MaterialSignature(const TerrainRequest &r){const auto &a=r.atlas;std::ostringstream key;
 const wchar_t *suffixes[]={L"_albedo.tga",L"_normal.tga",L"_orm.tga",L"_height.tga"};
 for(std::uint32_t i=0;i<a.count;++i){const auto &s=a.regions[i];const auto stem=std::filesystem::path(s.texture).filename().stem().wstring();
  key<<'\n'<<s.x<<' '<<s.y<<' '<<s.extent<<' '<<static_cast<unsigned>(s.ground)<<' ';
  for(const auto suffix:suffixes){const auto path=r.materials/(stem+suffix);const auto stamp=r.materials.empty()?std::string():TerrainTileKey(path,s.extent);key<<stamp.size()<<':'<<stamp<<' ';}
 }
 if(!r.materials.empty()){std::ifstream file(r.materials/L"runtime-dds"/L"manifest.json",std::ios::binary|std::ios::ate);
  if(file&&file.tellg()>0&&file.tellg()<4*1024*1024){const auto size=static_cast<std::size_t>(file.tellg());std::vector<char> manifest(size);file.seekg(0);if(file.read(manifest.data(),static_cast<std::streamsize>(size)))key<<" manifest "<<ShaderHash(manifest.data(),size);}}
 return key.str();
}
std::string TerrainCacheKey(const TerrainRequest &r){
 const auto &a=r.atlas;std::ostringstream key;key<<"atlas18 RGBA8 bilinear wrap-border4 normal128 ORM2552240 height128 classR meanGBA "<<a.width<<' '<<a.height<<' '<<a.count<<' '<<a.cellsX<<' '<<a.cellsY<<' '<<a.borderWorld<<' ';
 // Do not include changing sun/color/weather values. These are draw constants.
 key<<std::hex<<ShaderHash(r.base.pixels.data(),r.base.pixels.size())<<' '<<ShaderHash(r.cells.data(),r.cells.size()*sizeof(D3D11TerrainCell));
 key<<MaterialSignature(r);return key.str();
}
bool ReadTerrainAtlasCache(const std::filesystem::path &path,const std::string &key,std::uint32_t width,std::uint32_t height,PreparedTerrain &out,const std::atomic<bool> &cancel){
 if(!width||!height||width>4096||height>4096||key.size()>256*1024||cancel)return false;
 std::ifstream file(path,std::ios::binary);Header header{};
 if(!file.read(reinterpret_cast<char*>(&header),sizeof(header))||header.magic!=magic||header.version!=1||header.width!=width||header.height!=height||header.keyBytes!=key.size()||header.upgraded>200)return false;
 std::string saved(header.keyBytes,'\0');if(!file.read(saved.data(),header.keyBytes)||saved!=key)return false;
 PreparedTerrain result;result.width=width;result.height=height;result.upgraded=header.upgraded;result.key=key;
 const auto size=static_cast<std::size_t>(width)*height*4;
 for(unsigned i=0;i<5;++i)if(!ReadPixels(file,result.pixels[i],size,cancel)||ShaderHash(result.pixels[i].data(),size)!=header.hashes[i])return false;
 if(file.peek()!=EOF||cancel)return false;result.cacheHit=true;out=std::move(result);return true;
}
bool SaveTerrainAtlasCache(const std::filesystem::path &path,const PreparedTerrain &r,const std::atomic<bool> &cancel){
 if(path.empty()||!r.width||!r.height||r.width>4096||r.height>4096||r.key.size()>256*1024||cancel)return false;
 Header header{magic,1,r.width,r.height,static_cast<std::uint32_t>(r.key.size()),r.upgraded,{}};
 const auto size=static_cast<std::size_t>(r.width)*r.height*4;
 for(unsigned i=0;i<5;++i){if(r.pixels[i].size()!=size)return false;header.hashes[i]=ShaderHash(r.pixels[i].data(),size);}
 std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error)return false;
 auto temporary=path;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
 bool success=true;
 {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);if(!file)return false;file.write(reinterpret_cast<const char*>(&header),sizeof(header));file.write(r.key.data(),header.keyBytes);
  for(const auto &pixels:r.pixels)for(std::size_t at=0;at<size;){if(cancel){success=false;break;}const auto bytes=std::min(size-at,std::size_t(1024*1024));file.write(reinterpret_cast<const char*>(pixels.data()+at),static_cast<std::streamsize>(bytes));at+=bytes;}file.flush();success=success&&static_cast<bool>(file);}
 if(!success||cancel){DeleteFileW(temporary.c_str());return false;}
 if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());return false;}TrimCache(path.parent_path(),path);return true;
}
static void RecordTerrainMaterialProvenance(const TerrainRequest &request,const PreparedTerrain &prepared){
 // Once per prepared atlas, on the worker; never file I/O/hash scans per draw.
 const wchar_t *suffixes[]={L"_albedo.tga",L"_normal.tga",L"_orm.tga",L"_height.tga"};
 unsigned coverage[4]{};
 for(unsigned i=0;i<request.atlas.count;++i){const auto &region=request.atlas.regions[i];
  const auto stem=std::filesystem::path(region.texture).filename().stem().wstring();unsigned mask=0;
  for(unsigned map=0;map<4;++map){std::error_code error;
   if(!request.materials.empty()&&std::filesystem::is_regular_file(request.materials/(stem+suffixes[map]),error)){mask|=1u<<map;++coverage[map];}}
  std::ostringstream detail;detail<<"region="<<i<<" texture="<<region.texture<<" ground="<<static_cast<unsigned>(region.ground)<<" authored_mask="<<mask
   <<" x="<<region.x<<" y="<<region.y<<" native_extent="<<region.extent<<" atlas="<<prepared.width<<'x'<<prepared.height;
  GenZHLoad::Record("terrain_region_material_v79",detail.str(),0,0,prepared.cacheHit);
 }
 std::ostringstream detail;detail<<"regions="<<request.atlas.count<<" albedo="<<coverage[0]<<" normal="<<coverage[1]<<" orm="<<coverage[2]<<" height="<<coverage[3]
  <<" cache_key="<<std::hex<<ShaderHash(prepared.key.data(),prepared.key.size());
 for(unsigned map=0;map<5;++map)detail<<" map"<<map<<'='<<ShaderHash(prepared.pixels[map].data(),prepared.pixels[map].size());
 GenZHLoad::Record("terrain_material_set_v79",detail.str(),0,prepared.Bytes(),prepared.cacheHit);
}
std::vector<std::array<std::uint32_t,4>> BuildTerrainCellClasses(const std::vector<D3D11TerrainCell> &cells,std::uint32_t cellsX,const std::vector<std::uint8_t> &identity,std::uint32_t width,std::uint32_t height){
 std::vector<std::array<std::uint32_t,4>> classes;
 if(!width||!height||!cellsX||cells.size()%cellsX||identity.size()!=static_cast<std::size_t>(width)*height*4)return classes;
 // As the shader: Load(min(uint(frac(uv)*size),size-1)).
 auto texel=[&](float u,float v){const float fu=u-std::floor(u),fv=v-std::floor(v);
  const auto x=std::min(static_cast<std::uint32_t>(fu*static_cast<float>(width)),width-1),y=std::min(static_cast<std::uint32_t>(fv*static_cast<float>(height)),height-1);
  return identity.data()+(static_cast<std::size_t>(y)*width+x)*4;};
 classes.resize(cells.size());
 for(std::size_t i=0;i<cells.size();++i){const auto &c=cells[i];float uv[4]{};
  for(unsigned j=0;j<4;++j)uv[j]=(c.coordinates[0][j]+c.coordinates[1][j]+c.coordinates[2][j]+c.coordinates[3][j])*.25f;
  const float alpha=std::clamp((c.alpha[0]+c.alpha[1]+c.alpha[2]+c.alpha[3])*.25f,0.0f,1.0f);
  const std::uint8_t *a=texel(uv[0],uv[1]),*b=alpha>0?texel(uv[2],uv[3]):a;
  const auto ground=[](std::uint8_t g){return static_cast<std::uint32_t>(std::min<std::uint8_t>(g,15));};
  const auto colour=[](const std::uint8_t *p){return static_cast<std::uint32_t>(p[1])|(static_cast<std::uint32_t>(p[2])<<8)|(static_cast<std::uint32_t>(p[3])<<16);};
  classes[i]={ground(a[0])|(ground(b[0])<<8)|(static_cast<std::uint32_t>(std::lround(alpha*65535))<<16),colour(a),colour(b),0};}
 // Uniform neighbourhoods: with the warp at most half a cell, the field at a point
 // reads only its own cell's 3x3 neighbourhood, so there it is the cell's own class.
 const auto cellsY=static_cast<std::uint32_t>(cells.size()/cellsX);
 auto single=[&](const std::array<std::uint32_t,4> &r){return (r[0]>>16)==0;};
 for(std::uint32_t y=0;y<cellsY;++y)for(std::uint32_t x=0;x<cellsX;++x){auto &own=classes[static_cast<std::size_t>(y)*cellsX+x];bool uniform=single(own);
  for(int dy=-1;dy<=1&&uniform;++dy)for(int dx=-1;dx<=1&&uniform;++dx){
   const auto nx=static_cast<std::uint32_t>(std::clamp(static_cast<int>(x)+dx,0,static_cast<int>(cellsX)-1)),ny=static_cast<std::uint32_t>(std::clamp(static_cast<int>(y)+dy,0,static_cast<int>(cellsY)-1));
   const auto &r=classes[static_cast<std::size_t>(ny)*cellsX+nx];uniform=single(r)&&(r[0]&255)==(own[0]&255)&&r[1]==own[1];}
  own[3]=uniform?1u:0u;}
 return classes;
}
bool PrepareTerrain(TerrainRequest request,PreparedTerrain &out,const std::atomic<bool> &cancel){
 out={};GenZHLoad::Scope timing("terrain_prepare_worker","canonical atlas");
 if(!ValidateTerrainRequest(request,out.error)||cancel)return false;
 const auto climateStart=GenZHLoad::Seconds();const auto climate=AnalyzeTerrainClimate(request.atlas,request.cells);
 GenZHLoad::Record("weather_map_climate",std::to_string(static_cast<unsigned>(climate.kind))+" snow="+std::to_string(climate.snow)+" sand="+std::to_string(climate.sand)+" grass="+std::to_string(climate.grass),(GenZHLoad::Seconds()-climateStart)*1000,request.cells.size()*sizeof(D3D11TerrainCell));
 const auto &a=request.atlas;const unsigned scale=a.width<=2048&&a.height<=2048?2:1;
 const auto materialSignature=MaterialSignature(request);
 const auto key=TerrainCacheKey(request);const auto path=CachePath(request,key);
 if(!path.empty()&&ReadTerrainAtlasCache(path,key,a.width*scale,a.height*scale,out,cancel)){
  if(materialSignature!=MaterialSignature(request)){out={};out.error="terrain materials changed during cache read";return false;}
  out.climate=climate;out.cells=std::move(request.cells);out.classes=BuildTerrainCellClasses(out.cells,request.atlas.cellsX,out.pixels[4],out.width,out.height);
  timing.hit=true;timing.bytes=out.Bytes();RecordTerrainMaterialProvenance(request,out);return true;
 }
 if(cancel)return false;PreparedTerrain result;result.climate=climate;result.width=a.width*scale;result.height=a.height*scale;result.key=key;
 // terrain96: each region's mean native colour (linear average, stored sRGB)
 // goes into the class map's GBA. The world-space material library keeps it
 // as a tint, so the map's palette survives the photo-scanned ground layers.
 std::vector<std::array<std::uint8_t,3>> means(a.count);
 {std::array<double,256> linear{};for(unsigned i=0;i<256;++i){const double c=i/255.0;linear[i]=c<=.04045?c/12.92:std::pow((c+.055)/1.055,2.4);}
  for(unsigned region=0;region<a.count;++region){if(cancel)return false;const auto &s=a.regions[region];double sum[3]{};
   for(unsigned y=0;y<s.extent;++y){const auto *row=request.base.pixels.data()+((static_cast<std::size_t>(s.y)+y)*a.width+s.x)*4;
    for(unsigned x=0;x<s.extent;++x)for(unsigned c=0;c<3;++c)sum[c]+=linear[row[x*4+c]];}
   for(unsigned c=0;c<3;++c){const double m=sum[c]/(static_cast<double>(s.extent)*s.extent);
    const double e=m<=.0031308?m*12.92:1.055*std::pow(m,1/2.4)-.055;means[region][c]=static_cast<std::uint8_t>(std::clamp(e*255+.5,0.0,255.0));}}}
 ImageRGBA base;if(!ResizeRGBA(request.base,result.width,result.height,base)){out.error="terrain resize";return false;}
 result.pixels[0]=std::move(base.pixels);request.base.pixels.clear();request.base.pixels.shrink_to_fit();
 const std::array<std::array<std::uint8_t,4>,4> defaults{{{128,128,255,255},{255,224,0,255},{128,128,128,255},{0,0,0,255}}};
 const auto count=static_cast<std::size_t>(result.width)*result.height;
 for(unsigned map=1;map<5;++map){result.pixels[map].resize(count*4);for(std::size_t i=0;i<count;++i){if(i%65536==0&&cancel)return false;std::memcpy(result.pixels[map].data()+i*4,defaults[map-1].data(),4);}}
 const wchar_t *suffixes[]={L"_albedo.tga",L"_normal.tga",L"_orm.tga",L"_height.tga"};
 for(unsigned region=0;region<a.count;++region){const auto &s=a.regions[region];const unsigned size=s.extent*scale;const int border=static_cast<int>(4*scale);
  const auto stem=std::filesystem::path(s.texture).filename().stem().wstring();
  for(unsigned map=0;map<5;++map){if(cancel)return false;std::vector<std::uint8_t> tile;
   if(map<4&&!request.materials.empty()){const auto source=request.materials/(stem+suffixes[map]);std::error_code error;
    if(std::filesystem::is_regular_file(source,error)){std::string decodeError;if(!LoadTerrainTile(source,size,tile,decodeError)){out.error=decodeError;return false;}if(map==0)++result.upgraded;}}
   if(map<4&&tile.empty())continue;
   for(int y=-border;y<static_cast<int>(size)+border;++y){if(cancel)return false;
    for(int x=-border;x<static_cast<int>(size)+border;++x){const unsigned dx=s.x*scale+static_cast<unsigned>(x),dy=s.y*scale+static_cast<unsigned>(y);const auto dest=(static_cast<std::size_t>(dy)*result.width+dx)*4;
     if(map==4){result.pixels[4][dest]=static_cast<std::uint8_t>(s.ground);std::memcpy(result.pixels[4].data()+dest+1,means[region].data(),3);}
     else{const auto src=(static_cast<std::size_t>((y+static_cast<int>(size))%static_cast<int>(size))*size+static_cast<unsigned>((x+static_cast<int>(size))%static_cast<int>(size)))*4;std::memcpy(result.pixels[map].data()+dest,tile.data()+src,4);}
    }
   }
  }
 }
 // The base hash was captured before releasing pixels. Recheck source stamps
 // independently: no stale atlas may be saved after an edit during preparation.
 if(materialSignature!=MaterialSignature(request)){out.error="terrain materials changed during preparation";return false;}
 result.cells=std::move(request.cells);result.classes=BuildTerrainCellClasses(result.cells,a.cellsX,result.pixels[4],result.width,result.height);timing.bytes=result.Bytes();
 RecordTerrainMaterialProvenance(request,result);
 if(cancel)return false;if(!path.empty())SaveTerrainAtlasCache(path,result,cancel);
 if(cancel)return false;out=std::move(result);return true;
}
void TerrainPreparation::Run(){
 for(;;){std::unique_ptr<TerrainRequest> request;
  {std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return stop_||request_!=nullptr;});if(stop_)return;request=std::move(request_);running_=true;}
  auto result=std::make_unique<PreparedTerrain>();try{PrepareTerrain(std::move(*request),*result,cancel_);}catch(const std::exception &e){result->error=e.what();}catch(...){result->error="terrain preparation exception";}
  request.reset();
  {std::lock_guard<std::mutex> lock(mutex_);running_=false;if(cancel_||stop_){result.reset();busy_=false;}else ready_=std::move(result);}
 }
}
TerrainPreparation::~TerrainPreparation(){Stop();}
bool TerrainPreparation::Available(){std::lock_guard<std::mutex> lock(mutex_);return !busy_&&!stop_;}
bool TerrainPreparation::Submit(TerrainRequest request){std::lock_guard<std::mutex> lock(mutex_);if(busy_||stop_)return false;
 try{request_=std::make_unique<TerrainRequest>(std::move(request));if(!worker_.joinable())worker_=std::thread([this]{Run();});}
 catch(...){request_.reset();return false;}
 busy_=true;cancel_=false;wake_.notify_one();return true;}
std::unique_ptr<PreparedTerrain> TerrainPreparation::Take(){std::lock_guard<std::mutex> lock(mutex_);return std::move(ready_);}
void TerrainPreparation::Finish(){std::lock_guard<std::mutex> lock(mutex_);if(!running_){ready_.reset();busy_=false;}}
void TerrainPreparation::Cancel(){std::lock_guard<std::mutex> lock(mutex_);cancel_=true;if(!running_){ready_.reset();request_.reset();busy_=false;}wake_.notify_one();}
void TerrainPreparation::Stop(){{std::lock_guard<std::mutex> lock(mutex_);stop_=true;cancel_=true;request_.reset();ready_.reset();wake_.notify_one();}if(worker_.joinable())worker_.join();}
}
