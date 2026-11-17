// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainSurface.h"
#include "WeatherClimate.h"
#include "NativeImage.h"
#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <array>
namespace GenZH::Modern {
struct TerrainRequest {
 D3D11TerrainAtlas atlas;ImageRGBA base;
 std::vector<D3D11TerrainCell> cells;
 std::filesystem::path materials,profile;
};
struct PreparedTerrain {
 WeatherClimate climate;
 std::uint32_t width=0,height=0,upgraded=0;
 std::array<std::vector<std::uint8_t>,5> pixels;
 std::vector<D3D11TerrainCell> cells;
 // terrain101: per cell x = class A | class B<<8 | mean blend alpha (16-bit)<<16,
 // y/z = region colour A/B (sRGB bytes), w = 1 when every cell of its 3x3 neighbourhood
// has the same single class and region (the field there is that class alone).
 std::vector<std::array<std::uint32_t,4>> classes;
 std::string key,error;bool cacheHit=false;
 std::size_t Bytes()const;
};
bool ValidateTerrainRequest(const TerrainRequest &request,std::string &error);
std::string TerrainCacheKey(const TerrainRequest &request);
bool ReadTerrainAtlasCache(const std::filesystem::path &path,const std::string &key,std::uint32_t width,std::uint32_t height,
 PreparedTerrain &result,const std::atomic<bool> &cancel);
bool SaveTerrainAtlasCache(const std::filesystem::path &path,const PreparedTerrain &result,const std::atomic<bool> &cancel);
bool PrepareTerrain(TerrainRequest request,PreparedTerrain &result,const std::atomic<bool> &cancel);
// The class map lookups GroundAt/TerrainRegionAt make at each cell's mean UVs.
std::vector<std::array<std::uint32_t,4>> BuildTerrainCellClasses(const std::vector<D3D11TerrainCell> &cells,std::uint32_t cellsX,const std::vector<std::uint8_t> &identity,std::uint32_t width,std::uint32_t height);
// One active CPU job/result. Capacity stays reserved throughout GPU upload.
// The worker receives CPU snapshots, never DX8/11 objects or immediate contexts.
class TerrainPreparation final {
 std::thread worker_;std::mutex mutex_;std::condition_variable wake_;
 std::unique_ptr<TerrainRequest> request_;std::unique_ptr<PreparedTerrain> ready_;
 std::atomic<bool> cancel_{false};bool busy_=false,running_=false,stop_=false;
 void Run();
public:
 ~TerrainPreparation();
 bool Available();bool Submit(TerrainRequest request);
 std::unique_ptr<PreparedTerrain> Take();
 void Finish();void Cancel();void Stop();
};
}
