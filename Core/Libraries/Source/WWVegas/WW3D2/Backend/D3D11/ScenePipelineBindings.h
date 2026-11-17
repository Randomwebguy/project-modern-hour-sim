// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace GenZH::Modern {
inline thread_local bool sceneBindingsReference=false;
inline thread_local std::uint64_t sceneBindingsSkipped=0;
struct ScopedSceneBindingsReference {
    bool previous;
    explicit ScopedSceneBindingsReference(bool value):previous(sceneBindingsReference){sceneBindingsReference=value;}
    ~ScopedSceneBindingsReference(){sceneBindingsReference=previous;}
};
// This cache is legal only inside the synchronous main-scene Execute loop.
// It never spans a helper pass, frame, shadow, offscreen or HUD draw. Only
// binding slots owned exclusively by Execute in that loop are represented.
class ScenePipelineBindings {
public:
    // perf113 extra slots: the per-draw binds of Execute and BindInstances that
    // no other binder in the loop writes (verified by grep over the D3D11 backend):
    // PS SRV 10 (scene shadow), PS sampler 8 (shadow comparison), DS sampler 9 (terrain),
    // VS/PS CB 4 and VS SRV 15 (instance settings and matrices), SRV 13 and 14 of the
    // terrain cells and exclusion on GS/HS/DS. Surfaces (SRV 11 of PS/GS/HS/DS) are pairs.
    enum Slot {Raster,Blend,Layout,VS,PS,GS,HS,DS,Indices,VertexConstants,Motion,
        PSShadowView,PSCompare,DSSampler,VSInstanceView,VSInstanceCB,PSInstanceCB,
        GSCells,HSCells,DSCells,GSExclusion,DSExclusion,
        // perf114: per-draw binds of the environment, local surface lighting, atmosphere, native light calibration and scene shadow constants (all called from Execute only).
        EnvConstants,EnvSampler,LocalConstants,LocalAtlas,LocalSampler,AtmosphereConstants,CalibrationConstants,
        ShadowVS,ShadowPS,ShadowHS,ShadowDS,ShadowGS,SlotCount};
    enum SurfaceStage {SurfacePixel,SurfaceGeometry,SurfaceHull,SurfaceDomain,SurfaceStageCount};
    enum ArraySlot {EnvReflection,AtmosphereNoise,AtmosphereProbes,AtmosphereSamplers,ArraySlotCount};
private:
    template<class T>struct Memo {T value{};bool valid=false;void Clear(){valid=false;value={};}};
    using DepthValue=std::array<std::uintptr_t,2>;
    using VertexValue=std::array<std::uintptr_t,3>;
    std::array<Memo<void*>,SlotCount> singles_;
    Memo<DepthValue> depth_;Memo<VertexValue> vertex_;
    Memo<std::array<float,6>> viewport_;
    Memo<std::array<void*,8>> samplers_;
    Memo<std::array<void*,10>> textures_;
    Memo<std::array<void*,2>> extra_;
    Memo<std::array<void*,3>> wear_;
    Memo<unsigned> topology_;
    std::array<Memo<std::array<void*,2>>,SurfaceStageCount> surfaces_;
    std::array<Memo<std::array<void*,4>>,ArraySlotCount> arrays_;
    bool active_=false;
    template<class T,class Bind>void Update(Memo<T> &memo,const T &value,Bind bind){
        ++requests;
        if(!active_||!memo.valid||std::memcmp(&memo.value,&value,sizeof(T))){
            bind();++calls;if(active_){memo.value=value;memo.valid=true;}
        }else{++skips;++sceneBindingsSkipped;}
    }
    void Clear(){for(auto &m:singles_)m.Clear();depth_.Clear();vertex_.Clear();viewport_.Clear();samplers_.Clear();textures_.Clear();extra_.Clear();wear_.Clear();topology_.Clear();for(auto &m:surfaces_)m.Clear();for(auto &m:arrays_)m.Clear();}
public:
    std::uint64_t requests=0,calls=0,skips=0,scopes=0;
    // GENZH_SCENE_EXTRA_BINDINGS (runtime switch): off binds the SlotCount extra slots every draw, as before.
    bool extraEnabled=true;
    bool Begin(bool enabled){if(active_)return false;Clear();active_=enabled&&!sceneBindingsReference;if(active_)++scopes;return active_;}
    void End(){active_=false;Clear();}
    bool Active()const{return active_;}
    template<class Bind>void Single(Slot slot,void *pointer,Bind bind){Update(singles_[slot],pointer,bind);}
    // The extra slots bind directly outside the scope or while the switch is off.
    template<class Bind>void ExtraSingle(Slot slot,void *pointer,Bind bind){if(extraEnabled&&active_)Update(singles_[slot],pointer,bind);else bind();}
    template<class P,class Bind>void ExtraSurface(SurfaceStage stage,P *const (&value)[2],Bind bind){
        if(extraEnabled&&active_){std::array<void*,2> v{value[0],value[1]};Update(surfaces_[stage],v,bind);}else bind();
    }
    // Up to four views or samplers of one fixed-length binding (count is part of the slot's identity).
    template<class P,class Bind>void ExtraArray(ArraySlot slot,P *const *values,unsigned count,Bind bind){
        if(extraEnabled&&active_){std::array<void*,4> v{};for(unsigned i=0;i<count&&i<4;++i)v[i]=values[i];Update(arrays_[slot],v,bind);}else bind();
    }
    template<class Bind>void Depth(void *state,unsigned reference,Bind bind){DepthValue v{reinterpret_cast<std::uintptr_t>(state),reference};Update(depth_,v,bind);}
    template<class Bind>void Vertex(void *buffer,unsigned stride,unsigned offset,Bind bind){VertexValue v{reinterpret_cast<std::uintptr_t>(buffer),stride,offset};Update(vertex_,v,bind);}
    template<class Bind>void Viewport(const std::array<float,6> &value,Bind bind){Update(viewport_,value,bind);}
    template<class P,class Bind>void Samplers(const std::array<P*,8> &value,Bind bind){std::array<void*,8> v{};for(unsigned i=0;i<8;++i)v[i]=value[i];Update(samplers_,v,bind);}
    template<class P,class Bind>void Textures(const std::array<P*,10> &value,Bind bind){std::array<void*,10> v{};for(unsigned i=0;i<10;++i)v[i]=value[i];Update(textures_,v,bind);}
    template<class P,class Bind>void Extra(P *const (&value)[2],Bind bind){std::array<void*,2> v{value[0],value[1]};Update(extra_,v,bind);}
    template<class P,class Bind>void Wear(P *const (&value)[3],Bind bind){std::array<void*,3> v{value[0],value[1],value[2]};Update(wear_,v,bind);}
    template<class Bind>void Topology(unsigned value,Bind bind){Update(topology_,value,bind);}
};
struct ScopedScenePipelineBindings {
    ScenePipelineBindings &cache;bool owned;
    ScopedScenePipelineBindings(ScenePipelineBindings &value,bool enabled):cache(value),owned(cache.Begin(enabled)){}
    ~ScopedScenePipelineBindings(){if(owned)cache.End();}
    ScopedScenePipelineBindings(const ScopedScenePipelineBindings&)=delete;
    ScopedScenePipelineBindings &operator=(const ScopedScenePipelineBindings&)=delete;
};
}
