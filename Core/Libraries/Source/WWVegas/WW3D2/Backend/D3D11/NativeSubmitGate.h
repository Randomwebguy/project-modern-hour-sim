// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeSceneIdentity.h"
#include "NativeFixedState.h"
#include "NativeMeshSubmit.h"
#include "OpaqueSortPolicy.h"
namespace GenZH::Modern {
inline thread_local bool sortedSubmitReference=false;
class ScopedSortedSubmitReference {
    bool previous_;
public:
    explicit ScopedSortedSubmitReference(bool reference):previous_(sortedSubmitReference){sortedSubmitReference=reference;}
    ~ScopedSortedSubmitReference(){sortedSubmitReference=previous_;}
};
inline bool NativeSortedSubmitContext(const NativeMeshSource &source,bool enabled,bool offscreen,bool recording){
    // Only snapshot/binding capture changes. The original sorter has already
    // established primitive order; depth, alpha and stencil operations remain.
    return enabled&&!sortedSubmitReference&&!offscreen&&!recording&&source.Valid()&&source.sorted&&!source.skinned&&source.pass!=UINT32_MAX;
}
inline bool NativeSubmitContext(const NativeMeshSource &source,bool enabled,bool offscreen,bool recording){
    return enabled&&!offscreen&&!recording&&source.Valid()&&!source.skinned&&!source.sorted&&source.pass!=UINT32_MAX;
}
inline bool NativeSubmitLayout(const NativeFixedState &state,bool declaration,const NativeIndexedDraw &draw){
    return draw.primitive==D3DPT_TRIANGLELIST&&!declaration&&!state.pixelShader&&
        (state.vertexShader&D3DFVF_POSITION_MASK)==D3DFVF_XYZ&&(state.vertexShader&D3DFVF_NORMAL);
}
inline bool NativeSubmitState(const std::array<DWORD,256> &state){
    return state[D3DRS_ZENABLE]==D3DZB_TRUE&&state[D3DRS_ZWRITEENABLE]&&
        (!state[D3DRS_ALPHABLENDENABLE]||(state[D3DRS_SRCBLEND]==D3DBLEND_ONE&&state[D3DRS_DESTBLEND]==D3DBLEND_ZERO))&&
        OpaqueStencilSafe(state[D3DRS_STENCILENABLE],state[D3DRS_STENCILFUNC],state[D3DRS_STENCILFAIL],
            state[D3DRS_STENCILZFAIL],state[D3DRS_STENCILPASS])&&!state[D3DRS_CLIPPLANEENABLE]&&
        (state[D3DRS_ZFUNC]==D3DCMP_LESS||state[D3DRS_ZFUNC]==D3DCMP_LESSEQUAL);
}
inline bool NativeSubmitBindings(bool vertex,bool index,UINT stride,DWORD vertexUsage,DWORD indexUsage){
    return vertex&&index&&stride&&!(vertexUsage&D3DUSAGE_DYNAMIC)&&!(indexUsage&D3DUSAGE_DYNAMIC);
}
}
