// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CompatibilityRenderer.h"
namespace GenZH::Modern {
inline void NativeSceneCounts::Add(const DrawPacket &p){
        if(!p.nativeSource.Valid()){++untagged;return;}
        ++tagged;if(p.nativeSource.drawable)++drawable;
        if(p.nativeSource.skinned)++skinned;else ++rigid;
        if(p.nativeSource.sorted)++sorted;
        // Diagnostic surface classes are exclusive; they do not change states.
        if(p.outputOverlay||p.groundOverlay)++overlay;
        else if(p.vertexShader||p.pixelShader)++shader;
        else if(p.States().render[D3DRS_ALPHABLENDENABLE])++transparent;
        else if(p.States().render[D3DRS_ALPHATESTENABLE])++cutout;
        else ++opaque;
}
}
