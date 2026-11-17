// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeFixedState.h"
#include <cstdint>
namespace GenZH::Modern {
inline thread_local bool deferredFixedReference=false;
inline thread_local std::uint64_t deferredFixedWritesObserved=0;
struct ScopedDeferredFixedReference {
    bool previous;
    explicit ScopedDeferredFixedReference(bool value):previous(deferredFixedReference){deferredFixedReference=value;}
    ~ScopedDeferredFixedReference(){deferredFixedReference=previous;}
};
// Only exact values previously accepted by this native device can be deferred.
// A hash collision is a miss, never permission to skip validation.
template<class T,unsigned Count>class AcceptedFixedValues {
    struct Entry {T value{};unsigned tag=0;bool valid=false;};
    std::array<Entry,Count> entries_{};
    static unsigned Slot(unsigned tag,const T &value){
        // perf115: 64-bit words instead of one multiply per byte (about 1% of CPU in
        // the fortress scene). Any deterministic slot is correct: Has() still
        // compares the tag and every byte.
        std::uint64_t h=0xcbf29ce484222325ull^tag;
        const auto *bytes=reinterpret_cast<const unsigned char*>(&value);
        std::size_t i=0;
        for(;i+sizeof(std::uint64_t)<=sizeof(T);i+=sizeof(std::uint64_t)){std::uint64_t word;std::memcpy(&word,bytes+i,sizeof(word));h=(h^word)*0x100000001b3ull;}
        for(;i<sizeof(T);++i)h=(h^bytes[i])*0x100000001b3ull;
        return static_cast<unsigned>((h^(h>>32))%Count);
    }
public:
    bool Has(unsigned tag,const T &value)const{const auto &e=entries_[Slot(tag,value)];return e.valid&&e.tag==tag&&!std::memcmp(&e.value,&value,sizeof(T));}
    void Remember(unsigned tag,const T &value){auto &e=entries_[Slot(tag,value)];e.value=value;e.tag=tag;e.valid=true;}
    void Clear(){for(auto &e:entries_)e.valid=false;}
};
class DeferredFixedWrites {
    AcceptedFixedValues<D3DMATRIX,1024> transforms_;
    AcceptedFixedValues<D3DMATERIAL8,256> materials_;
    std::array<D3DMATRIX,2> pending_{};D3DMATERIAL8 material_{};
    unsigned dirty_=0;bool active_=false;HRESULT error_=S_OK;
public:
    struct Counts {std::uint64_t scopes=0,deferred=0,nativeWrites=0,synchronizations=0,failures=0;} counts;
    bool Begin(bool enabled,const NativeFixedStateMirror &mirror){
        if(active_||!enabled||deferredFixedReference||!mirror.Ready()||FAILED(error_))return false;
        active_=true;++counts.scopes;return true;
    }
    HRESULT Status()const{return error_;}
    bool Active()const{return active_;}
    unsigned Dirty()const{return dirty_;}
    template<class Native>HRESULT Sync(Native &native){
        if(FAILED(error_))return error_;
        if(dirty_)++counts.synchronizations;
        for(unsigned i=0;i<3;++i)if(dirty_&(1u<<i)){
            ++counts.nativeWrites;
            const HRESULT hr=i==2?native.SetMaterial(&material_):native.SetTransform(i?D3DTS_VIEW:D3DTS_WORLD,&pending_[i]);
            if(FAILED(hr)){error_=hr;active_=false;++counts.failures;return hr;}
            dirty_&=~(1u<<i);
        }
        return S_OK;
    }
    template<class Native>HRESULT End(Native &native){const auto hr=Sync(native);active_=false;return hr;}
    // Reset intentionally discards pending state: native Reset establishes a new
    // device epoch. Validation learned in the old epoch cannot authorize writes.
    void Reset(){active_=false;dirty_=0;error_=S_OK;transforms_.Clear();materials_.Clear();}
    template<class Native>HRESULT Transform(Native &native,NativeFixedStateMirror &mirror,D3DTRANSFORMSTATETYPE state,const D3DMATRIX *matrix){
        if(FAILED(error_))return error_;
        if(mirror.SameTransform(state,matrix))return S_OK;
        const int i=state==D3DTS_WORLD?0:state==D3DTS_VIEW?1:-1;
        if(active_&&mirror.Ready()&&i>=0&&matrix&&transforms_.Has(static_cast<unsigned>(state),*matrix)){
            pending_[i]=*matrix;dirty_|=1u<<i;mirror.TransformChanged(state,matrix,S_OK);++counts.deferred;++deferredFixedWritesObserved;return S_OK;
        }
        auto hr=Sync(native);if(FAILED(hr))return hr;++counts.nativeWrites;
        hr=native.SetTransform(state,matrix);
        if(hr==S_OK&&matrix&&i>=0&&!mirror.Recording())transforms_.Remember(static_cast<unsigned>(state),*matrix);
        mirror.TransformChanged(state,matrix,hr);return hr;
    }
    template<class Native>HRESULT Material(Native &native,NativeFixedStateMirror &mirror,const D3DMATERIAL8 *material){
        if(FAILED(error_))return error_;
        if(mirror.SameMaterial(material))return S_OK;
        if(active_&&mirror.Ready()&&material&&materials_.Has(0,*material)){
            material_=*material;dirty_|=4;mirror.MaterialChanged(material,S_OK);++counts.deferred;++deferredFixedWritesObserved;return S_OK;
        }
        auto hr=Sync(native);if(FAILED(hr))return hr;++counts.nativeWrites;
        hr=native.SetMaterial(material);
        if(hr==S_OK&&material&&!mirror.Recording())materials_.Remember(0,*material);
        mirror.MaterialChanged(material,hr);return hr;
    }
};
}
