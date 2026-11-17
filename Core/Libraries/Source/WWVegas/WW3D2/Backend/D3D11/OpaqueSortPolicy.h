// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d8.h>
namespace GenZH::Modern {
// The scene marks visible player pixels with REPLACE, then reads those marks
// in a separate overlay pass. With ALWAYS/KEEP/KEEP, only depth-winning pixels
// change stencil; a category draw preserves reference and write mask exactly.
// Counting, rejected-fragment writes and stencil-tested overlays stay sorted.
inline bool OpaqueStencilSafe(DWORD enabled,DWORD function,DWORD fail,DWORD depthFail,DWORD pass){
 return !enabled||(function==D3DCMP_ALWAYS&&fail==D3DSTENCILOP_KEEP&&depthFail==D3DSTENCILOP_KEEP&&
  (pass==D3DSTENCILOP_KEEP||pass==D3DSTENCILOP_REPLACE));
}
// Headless final-EXE comparison can explicitly exercise the original sorter.
// The production default always permits the checked category optimization.
inline thread_local bool opaqueSortReference=false;
struct ScopedOpaqueSortReference {
 bool previous;
 explicit ScopedOpaqueSortReference(bool reference):previous(opaqueSortReference){opaqueSortReference=reference;}
 ~ScopedOpaqueSortReference(){opaqueSortReference=previous;}
};
inline bool OpaqueSortBypass(DWORD blend,DWORD source,DWORD destination,DWORD depth,
 DWORD write,DWORD compare,DWORD stencil,DWORD pixelShader,
 DWORD stencilFunction=0,DWORD stencilFail=0,DWORD stencilDepthFail=0,DWORD stencilPass=0){
 return (!blend||(source==D3DBLEND_ONE&&destination==D3DBLEND_ZERO))&&depth==D3DZB_TRUE&&write&&
  (compare==D3DCMP_LESS||compare==D3DCMP_LESSEQUAL)&&
  OpaqueStencilSafe(stencil,stencilFunction,stencilFail,stencilDepthFail,stencilPass)&&!pixelShader;
}
}
