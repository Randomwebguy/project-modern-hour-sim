// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
namespace GenZH::Modern {
struct CloudLayer {
 float base=900,top=1800,extinction=.006f;
};
// Smoothly deepen developing storm volumes while retaining nominal vertical
// optical thickness. The native world is Z-up. No texture or geometry scaling.
inline CloudLayer WeatherCloudLayer(float coverage){
 if(!std::isfinite(coverage))coverage=0;
 const float t=std::clamp((coverage-.45f)/.4f,0.0f,1.0f);
 const float thickness=900+1000*t*t*(3-2*t);
 return {900,900+thickness,5.4f/thickness};
}
}
