// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace GenZH::Modern {
// Keep the exact production world/instance/light transform. A position-only
// return lets the compiler remove all color, normal and texture coordinate
// work. Append separately so unchanged shaders retain their existing keys.
inline constexpr char OpaqueShadowShaderSuffix[]=R"GENZH(
float4 LegacyOpaqueShadowVS(LegacyInput input):SV_POSITION {
 return LegacyShadowVS(input).position;
}
)GENZH";
}
