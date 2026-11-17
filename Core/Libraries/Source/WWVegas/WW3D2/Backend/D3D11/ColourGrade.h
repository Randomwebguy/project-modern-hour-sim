// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>

namespace GenZH::Modern {

// photo107: the colour grade of the tone map for one linear colour: saturation about luma, highlight
// warmth (from the luma before saturation), contrast about 18% grey, clamped to the display range.
// RenderDevice::BuildColourLut samples this on a 32^3 lattice; Photo107Checks.cpp tests it directly.
inline void GradeColour(float (&colour)[3], float saturation, float warmth, float contrast) {
    const float grey = .2126f * colour[0] + .7152f * colour[1] + .0722f * colour[2];
    for (int k = 0; k < 3; ++k) colour[k] = grey + (colour[k] - grey) * saturation;
    const float t = std::clamp((grey - .25f) / .65f, 0.0f, 1.0f), highlight = warmth * t * t * (3 - 2 * t);
    colour[0] *= 1 + .025f * highlight;
    colour[2] *= 1 - .035f * highlight;
    for (int k = 0; k < 3; ++k) colour[k] = std::clamp(.18f + (colour[k] - .18f) * contrast, 0.0f, 1.0f);
}

}
