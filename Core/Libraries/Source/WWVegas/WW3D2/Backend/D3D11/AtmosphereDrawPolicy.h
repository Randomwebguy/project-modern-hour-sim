// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d8.h>
namespace GenZH::Modern {
// Protect native visibility/shroud overlays and transparent particle order.
// Arbitrary vertex programs and secondary projections retain native rendering.
template<class Packet>bool AtmosphereWorldPacket(const Packet &p){
 return !p.outputOverlay&&!p.vertexShader&&(!p.pixelShader||p.terrain)&&p.constants.geometryFlags[0]<.5f&&
  p.States().render[D3DRS_ZENABLE]&&p.States().render[D3DRS_ZWRITEENABLE]&&!p.States().render[D3DRS_ALPHABLENDENABLE];
}
// perf94: screen-space draws (HUD, radar, text): pretransformed vertices or an
// orthographic projection. World passes (depth, GTAO, fog of war) run below them.
template<class Packet>bool ScreenSpacePacket(const Packet &p){
 return !p.terrain&&!p.outputOverlay&&(p.constants.geometryFlags[0]>.5f||(p.constants.projection[11]==0&&p.constants.projection[15]==1));
}
}
