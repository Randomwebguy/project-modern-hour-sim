// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DrawGrouping.h"
#include "NativeSceneCapture.h"
namespace GenZH::Modern {
struct CameraScene {
    std::vector<const DrawPacket*> packets;
    std::vector<PreparedInstance> prepared;
    std::uint64_t seen=0,boundsTested=0,culled=0,fallback=0;
    NativeSceneCounts native;
};
template<class Scene> inline CameraScene PrepareCameraScene(const Scene &scene,bool cull=true){
    CameraScene result;result.packets.reserve(scene.size());result.prepared.reserve(scene.size());
    for(const auto &p:scene){
        ++result.seen;result.native.Add(p);PreparedInstance prepared{&p,InstanceEligibility(p),ProjectCameraBounds(p)};
        // Only cached, fixed XYZ opaque meshes have conservative immutable
        // bounds. Custom/skinned shaders, terrain displacement, unsafe stencil,
        // near-plane crossings and invalid transforms retain their native path.
        const bool trusted=prepared.bounds.valid;
        if(trusted)++result.boundsTested;else ++result.fallback;
        const auto &b=prepared.bounds;
        if(cull&&trusted&&(b.high[0]<-1||b.low[0]>1||b.high[1]<-1||b.low[1]>1)){++result.culled;continue;}
        result.packets.push_back(&p);result.prepared.push_back(prepared);
    }return result;
}
}
