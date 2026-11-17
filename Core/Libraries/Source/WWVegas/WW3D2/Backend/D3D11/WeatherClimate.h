// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainSurface.h"
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
namespace GenZH::Modern {
enum class MapClimate {Temperate,Arid,Alpine};
struct WeatherClimate {MapClimate kind=MapClimate::Temperate;float snow=0,sand=0,grass=0;};
// photo107: the settled layer a map's climate lays on its world: alpine ground or mission
// snow settles snow (flag 524288), arid ground settles sand dust (flag 1048576).
inline unsigned SettledLayerFlags(const WeatherClimate &climate,bool missionSnow){
 if(missionSnow||climate.kind==MapClimate::Alpine)return 524288u;
 return climate.kind==MapClimate::Arid?1048576u:0u;
}
// Use canonical world-cell base/blend coordinates, not inventory counts or
// texture colours. This runs once on the terrain CPU worker, including disk hits.
inline WeatherClimate AnalyzeTerrainClimate(const D3D11TerrainAtlas &atlas,const std::vector<D3D11TerrainCell> &cells){
 WeatherClimate result;if(!atlas.width||!atlas.height||atlas.count>200||cells.empty())return result;
 std::array<std::vector<unsigned>,32> rows;
 for(unsigned i=0;i<atlas.count;++i){const auto &r=atlas.regions[i];
  const unsigned first=(std::min)(31u,r.y*32/atlas.height),last=(std::min)(31u,(r.y+r.extent)*32/atlas.height);
  for(unsigned y=first;y<=last;++y)rows[y].push_back(i);
 }
 auto ground=[&](float u,float v){
  if(!std::isfinite(u)||!std::isfinite(v)||u<0||v<0||u>=1||v>=1)return D3D11Ground::Unknown;
  const float x=u*atlas.width,y=v*atlas.height;
  for(unsigned i:rows[(std::min)(31u,static_cast<unsigned>(v*32))]){const auto &r=atlas.regions[i];
   if(x>=r.x&&y>=r.y&&x<r.x+r.extent&&y<r.y+r.extent)return r.ground;
  }
  return D3D11Ground::Unknown;
 };
 double snow=0,sand=0,grass=0;
 auto add=[&](D3D11Ground g,double weight){if(g==D3D11Ground::Snow)snow+=weight;else if(g==D3D11Ground::Sand)sand+=weight;else if(g==D3D11Ground::Grass)grass+=weight;};
 for(const auto &cell:cells){float baseU=0,baseV=0,blendU=0,blendV=0,alpha=0;
  for(unsigned corner=0;corner<4;++corner){baseU+=cell.coordinates[corner][0]*.25f;baseV+=cell.coordinates[corner][1]*.25f;blendU+=cell.coordinates[corner][2]*.25f;blendV+=cell.coordinates[corner][3]*.25f;alpha+=cell.alpha[corner]*.25f;}
  if(!std::isfinite(alpha))continue;alpha=std::clamp(alpha,0.0f,1.0f);
  add(ground(baseU,baseV),1-alpha);if(alpha>0)add(ground(blendU,blendV),alpha);
 }
 const double total=static_cast<double>(cells.size());result.snow=static_cast<float>(snow/total);result.sand=static_cast<float>(sand/total);result.grass=static_cast<float>(grass/total);
 // Mixed/unknown maps retain temperate default. Native mission snow overrides
 // classification separately; no climate is inferred from tile filenames here.
 if(result.snow>=.35f)result.kind=MapClimate::Alpine;
 else if(result.sand>=.55f&&result.grass<.2f)result.kind=MapClimate::Arid;
 return result;
}
}
