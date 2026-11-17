// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
namespace GenZH::Modern {
// Exact separable maximum of a clamped 3x3 R8 neighborhood. Build only when
// authoritative roads/footprints change; never per vertex or per frame.
inline std::vector<std::uint8_t> FilterTerrainExclusion(const std::vector<std::uint8_t> &source,unsigned width,unsigned height){
 if(!width||!height||source.size()/width!=height||source.size()%width)throw std::invalid_argument("terrain exclusion dimensions");
 std::vector<std::uint8_t> horizontal(source.size()),filtered(source.size());
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
  const auto row=static_cast<std::size_t>(y)*width;
  horizontal[row+x]=std::max({source[row+(x?x-1:0)],source[row+x],source[row+std::min(x+1,width-1)]});
 }
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
  filtered[static_cast<std::size_t>(y)*width+x]=std::max({horizontal[static_cast<std::size_t>(y?y-1:0)*width+x],horizontal[static_cast<std::size_t>(y)*width+x],horizontal[static_cast<std::size_t>(std::min(y+1,height-1))*width+x]});
 return filtered;
}
}
