// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d8.h>
#include <array>
#include <cstdint>
#include "../../GenZHRenderStates.h"
#include "StateSnapshot.h"
#include <memory>
namespace GenZH::Modern {
class NativeStateMirror {
    bool ready_=false,recording_=false;
    std::array<bool,256> renderValid_{};
    std::array<std::array<bool,32>,8> stageValid_{};
    // perf112: the shared copy handed to packets; rebuilt only when the revision has moved on.
    std::shared_ptr<const StateSnapshot> snapshot_;
    std::uint64_t snapshotRevision_=0;
public:
    std::array<DWORD,256> render{};
    std::array<std::array<DWORD,32>,8> stages{};
    // perf89: identifies the bytes of render and stages. Every change of either
    // array (a setter storing a different value, any full capture) takes a new
    // process-unique value, so equal nonzero revisions mean identical arrays.
    std::uint64_t revision=0;
    static std::uint64_t NextRevision(){static std::uint64_t counter=0;return ++counter;}
    // perf112: the current bytes as a shared immutable snapshot. Equal revisions mean equal bytes, so the copy is
    // rebuilt only after a change; draws captured between two changes share one allocation.
    std::shared_ptr<const StateSnapshot> Snapshot(){
        if(!snapshot_||snapshotRevision_!=revision){
            auto next=std::make_shared<StateSnapshot>();next->render=render;next->stages=stages;
            snapshot_=std::move(next);snapshotRevision_=revision;
        }
        return snapshot_;
    }
    // Only successful native reads/setters establish equality. Invalid enums
    // must still reach the native validator, even if their captured bytes are 0.
    bool SameRender(UINT state,DWORD value)const{return ready_&&!recording_&&state<render.size()&&renderValid_[state]&&render[state]==value;}
    bool SameStage(UINT stage,UINT state,DWORD value)const{return ready_&&!recording_&&stage<stages.size()&&state<32&&stageValid_[stage][state]&&stages[stage][state]==value;}
    void Invalidate(){ready_=false;}
    void Reset(){ready_=recording_=false;}
    void BeginBlock(HRESULT hr){if(SUCCEEDED(hr))recording_=true;}
    void EndBlock(HRESULT hr){if(SUCCEEDED(hr)){recording_=false;Invalidate();}}
    // Custom states (GENZH_D3DRS_DECAL) live only in the mirror: the native device never receives or answers them.
    void SetCustomRender(UINT state,DWORD value){if(state==GENZH_D3DRS_DECAL&&render[state]!=value){render[state]=value;revision=NextRevision();}}
    void RenderChanged(UINT state,DWORD value,HRESULT hr){if(ready_&&!recording_&&SUCCEEDED(hr)&&state<render.size()){if(render[state]!=value){render[state]=value;revision=NextRevision();}renderValid_[state]=true;}}
    void StageChanged(UINT stage,UINT state,DWORD value,HRESULT hr){if(ready_&&!recording_&&SUCCEEDED(hr)&&stage<stages.size()&&state<32){if(stages[stage][state]!=value){stages[stage][state]=value;revision=NextRevision();}stageValid_[stage][state]=true;}}
    template<class ReadRender,class ReadStage>void Capture(ReadRender readRender,ReadStage readStage){
        // During block recording consult native state if a draw is submitted;
        // setters may be recording values rather than applying them live.
        if(ready_&&!recording_)return;
        const DWORD custom=render[GENZH_D3DRS_DECAL];render.fill(0);for(auto &stage:stages)stage.fill(0);render[GENZH_D3DRS_DECAL]=custom;
        for(UINT i=0;i<256;++i)if(i!=GENZH_D3DRS_DECAL)renderValid_[i]=SUCCEEDED(readRender(i,&render[i]));
        for(auto &valid:stageValid_)valid.fill(false);
        for(UINT stage=0;stage<8;++stage)for(UINT state=1;state<32;++state)stageValid_[stage][state]=SUCCEEDED(readStage(stage,state,&stages[stage][state]));
        revision=NextRevision();ready_=true;
    }
};
}
