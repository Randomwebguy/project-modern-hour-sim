// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d8to9.hpp"
// Internal ABI between translation units with the original D3D8 and D3D9 headers.
// Returned pointers are borrowed; callers retain the owning D3D8 resource.
extern "C" void *GenZH_Resource9Surface(void *resource) { return static_cast<Direct3DSurface8 *>(resource)->GetProxyInterface(); }
extern "C" void *GenZH_Resource9Volume(void *resource) { return static_cast<Direct3DVolume8 *>(resource)->GetProxyInterface(); }
extern "C" void *GenZH_Resource9Texture(void *resource,int type) {
    if(type==D3DRTYPE_TEXTURE) return static_cast<Direct3DTexture8 *>(resource)->GetProxyInterface();
    if(type==D3DRTYPE_CUBETEXTURE) return static_cast<Direct3DCubeTexture8 *>(resource)->GetProxyInterface();
    if(type==D3DRTYPE_VOLUMETEXTURE) return static_cast<Direct3DVolumeTexture8 *>(resource)->GetProxyInterface();
    return nullptr;
}
