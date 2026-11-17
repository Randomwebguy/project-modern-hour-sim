// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AtmosphereDrawPolicy.h"
namespace GenZH::Modern {
template<class Packet>bool ScreenMaterialPacket(const Packet &packet,bool shadow,bool offscreen,bool primaryDepth,bool cameraMatches){
 constexpr DWORD rgb=D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE;
 return !shadow&&!offscreen&&primaryDepth&&cameraMatches&&AtmosphereWorldPacket(packet)&&packet.constants.modernMaterial[0]>.5f&&(packet.States().render[D3DRS_COLORWRITEENABLE]&rgb)==rgb;
}
}
