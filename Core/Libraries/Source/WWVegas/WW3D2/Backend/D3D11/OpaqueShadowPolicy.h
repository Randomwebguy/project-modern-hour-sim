// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CompatibilityRenderer.h"
namespace GenZH::Modern {
inline bool OpaqueShadowDepthPacket(const DrawPacket &packet){
 // The existing shadow state disables blending/stencil and uses the same
 // depth bias and topology. Terrain keeps displacement/grass. Programmable
 // vertex and screen-space draws retain their full original shadow path.
 return !packet.terrain&&!packet.outputOverlay&&!packet.vertexShader&&
   packet.constants.geometryFlags[0]<.5f&&packet.constants.alphaStage[0][3]==0&&
   (packet.primitive==D3DPT_TRIANGLELIST||packet.primitive==D3DPT_TRIANGLESTRIP);
}
}
