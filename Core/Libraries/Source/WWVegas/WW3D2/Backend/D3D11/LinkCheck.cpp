// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderDevice.h"
// Build-only tool: not a game and not a renderer demonstration. The conditional
// call retains device initialization and its SDK imports for link validation.
int main(int argc,char **) {
    GenZH::Modern::RenderDevice renderer;
    if(argc>1) return static_cast<int>(renderer.Initialize(nullptr,1,1));
    return renderer.DeviceStatus()==E_UNEXPECTED?0:1;
}
