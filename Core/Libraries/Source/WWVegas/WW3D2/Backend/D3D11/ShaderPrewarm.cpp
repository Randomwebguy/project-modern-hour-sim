// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProgramSource.h"
#include "SortedPaletteShaderSource.h"
#include "TerrainCompactShaderSource.h"
#include "OpaqueShadowShaderSource.h"
#include "LegacyShaderSource.h"
#include "ShaderSource.h"
#include "AtmosphereShaderSource.h"
#include "SceneDepthShaderSource.h"
#include "GTAOShaderSource.h"
#include "TemporalAAShaderSource.h"
#include "BloomShaderSource.h"
#include "ShaderBytecodeCache.h"
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <atomic>
#include <future>
using namespace GenZH::Modern;
int main(int argc,char **argv){try{
 if(argc!=3)throw std::runtime_error("usage: shader-prewarm profile-directory legacy-assembly-directory");
 const auto profile=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(profile);
 SetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",profile.c_str());std::atomic<unsigned> calls{0},hits{0};
 auto compile=[&](const std::string &source,const char *entry,const char *target){
  Microsoft::WRL::ComPtr<ID3DBlob> blob;std::string error;bool hit=false;
  if(FAILED(CompileCachedShader(source,entry,target,blob,error,hit)))throw std::runtime_error(std::string(entry)+": "+error);++calls;if(hit)++hits;
 };
 for(const char *entry:{"VSMain","PSMain","PSShadow","VSPresent","PSTonemap","PSSceneAA","PSGlobalIllumination","PSGlobalCompose"})compile(ShaderSource,entry,entry[0]=='V'?"vs_5_0":"ps_5_0");
 for(const char *entry:{"CSSceneDepthCopy","CSSceneDepthReduce"})compile(SceneDepthShaderSource,entry,"cs_5_0");
 for(const char *entry:{"CSCloudShadow","CSAtmosphere","CSLocalClusters","VSAtmosphere","VSRain","PSCloudSky","PSRTSBackground","PSCloudResolve","PSCloudTemporal","PSRain","PSShroudFog","PSShroudFade","CSWeatherSky","CSWeatherSpecular","CSWeatherDiffuse"})compile(AtmosphereShaderSource,entry,entry[0]=='C'?"cs_5_0":entry[0]=='V'?"vs_5_0":"ps_5_0");
 compile(TemporalAAShaderSource,"VSTemporal","vs_5_0");compile(TemporalAAShaderSource,"PSTemporalResolve","ps_5_0");compile(TemporalAAShaderSource,"PSTemporalSharpen","ps_5_0");compile(BloomShaderSource,"VSBloom","vs_5_0");compile(BloomShaderSource,"PSBloomPrefilter","ps_5_0");compile(BloomShaderSource,"PSBloomDown","ps_5_0");compile(BloomShaderSource,"PSBloomUp","ps_5_0");
 for(const char *entry:{"CSGTAODepth","CSGTAOReduce","CSGTAO","CSGTAODenoise","VSGTAO","PSGTAO"})compile(GTAOShaderSource,entry,entry[0]=='C'?"cs_5_0":entry[0]=='V'?"vs_5_0":"ps_5_0");
 struct Variant{std::string name,vertex,pixel;};std::vector<Variant> variants{{"fixed",{}, {}}};
 for(const auto &file:std::filesystem::directory_iterator(argv[2])){
  const auto path=file.path();if(path.extension()!=".asm"||path.stem().extension()==".asm")continue;
  // Disassembly files next to .pso/.vso and captured water pixel assemblies.
  std::ifstream input(path);std::string assembly((std::istreambuf_iterator<char>(input)),{});
  bool vertex=assembly.find("vs.1.")!=std::string::npos||assembly.find("vs_1_")!=std::string::npos;
  bool pixel=assembly.find("ps.1.")!=std::string::npos||assembly.find("ps_1_")!=std::string::npos;
  if(vertex||pixel)variants.push_back({path.filename().string(),vertex?assembly:"",pixel?assembly:""});
 }
 std::ofstream manifest(profile/L"shader-prewarm-v17.tsv");manifest<<"variant\ttexture_mask\tentries\n";
 std::vector<std::pair<std::size_t,unsigned>> jobs;
 for(std::size_t v=0;v<variants.size();++v)for(unsigned mask:{0u,1u,3u,7u,15u,255u}){
  if(variants[v].name!="fixed"&&mask!=1&&mask!=3&&mask!=7&&!(mask==15&&variants[v].name=="terrainnoise2.pso.asm"))continue;
  jobs.push_back({v,mask});
 }
 std::atomic<std::size_t> next{0};std::vector<std::future<void>> workers;
 for(unsigned worker=0;worker<4;++worker)workers.push_back(std::async(std::launch::async,[&]{
  for(;;){const auto job=next.fetch_add(1);if(job>=jobs.size())break;
   const auto &variant=variants[jobs[job].first];const auto mask=jobs[job].second;
  std::array<D3DRESOURCETYPE,8> types{};for(unsigned i=0;i<8;++i)types[i]=mask&(1u<<i)?D3DRTYPE_TEXTURE:static_cast<D3DRESOURCETYPE>(0);
  std::string error,source=BuildProgramSource(LegacyShaderSource,!variant.vertex.empty(),!variant.pixel.empty(),variant.vertex,variant.pixel,types,error);
  if(source.empty())throw std::runtime_error(variant.name+": "+error);
  for(const char *entry:{"LegacyVS","LegacyPS","LegacyPointGS","LegacyShadowVS","LegacyShadowPS","TerrainHS","TerrainDS","TerrainGrassGS","TerrainGrassCompactGS"}){
   const std::string name(entry);const char *target=name=="TerrainHS"?"hs_5_0":name=="TerrainDS"?"ds_5_0":name=="LegacyPointGS"||(name=="TerrainGrassGS"||name=="TerrainGrassCompactGS")?"gs_5_0":name=="LegacyVS"||name=="LegacyShadowVS"?"vs_5_0":"ps_5_0";
   compile(source,entry,target);
  }
  for(const char *entry:{"TerrainCompactPS","TerrainCompactShadowPS"})compile(source+TerrainCompactShaderSuffix,entry,"ps_5_0");
  compile(source+OpaqueShadowShaderSuffix,"LegacyOpaqueShadowVS","vs_5_0");
  }
 }));
 for(auto &worker:workers)worker.get();
 const std::array<D3DRESOURCETYPE,8> canonicalTypes{};std::string paletteError;
 const auto paletteSource=BuildProgramSource(LegacyShaderSource,false,false,{},{},canonicalTypes,paletteError);
 if(paletteSource.empty())throw std::runtime_error(paletteError);
 compile(SortedPaletteVertexSource(paletteSource),"LegacyVS","vs_5_0");
 // terrain97 GPU grass: the exact source TerrainGrassGPU::Create compiles (stages 0/1 2D).
 std::array<D3DRESOURCETYPE,8> grassTypes{};grassTypes[0]=grassTypes[1]=D3DRTYPE_TEXTURE;std::string grassError;
 const auto grassSource=BuildProgramSource(LegacyShaderSource,false,false,{},{},grassTypes,grassError);if(grassSource.empty())throw std::runtime_error(grassError);
 compile(grassSource,"GrassCS","cs_5_0");compile(grassSource,"GrassVS","vs_5_0");compile(grassSource,"GrassInteractCS","cs_5_0");
 for(const auto &job:jobs)manifest<<variants[job.first].name<<'\t'<<job.second<<"\t12\n";
 std::cout<<"{\"passed\":true,\"compile_requests\":"<<calls.load()<<",\"cache_hits\":"<<hits.load()<<",\"compiled\":"<<calls.load()-hits.load()<<",\"variants\":"<<variants.size()<<",\"game_started\":false}\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}

