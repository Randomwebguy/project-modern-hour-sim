// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <d3d8.h>
namespace GenZH::Modern {
inline thread_local bool resolvedStatesReference=false;
inline thread_local std::uint64_t resolvedStateHits=0;
struct ScopedResolvedStatesReference {
    bool previous;
    explicit ScopedResolvedStatesReference(bool value):previous(resolvedStatesReference){resolvedStatesReference=value;}
    ~ScopedResolvedStatesReference(){resolvedStatesReference=previous;}
};
// Cache immutable descriptor resolution, never context binding permission.
// Exact legacy inputs qualify only after the original resolver succeeds. Map
// value addresses survive rehash; Clear MUST precede destruction of those maps.
// A bounded direct-mapped table and full key comparisons make collisions misses.
template<std::size_t N,class Value,std::size_t Slots=64>class ResolvedStateCache {
    static_assert(Slots && !(Slots&(Slots-1)));
public:
    using Key=std::array<DWORD,N>;
    struct Counts {std::uint64_t requests=0,hits=0,misses=0,bypassed=0,failures=0;} counts;
private:
    struct Entry {Key key{};Value *value=nullptr;};
    std::array<Entry,Slots> entries_{};Entry last_{};
    static std::size_t Index(const Key &key){std::uint32_t h=2166136261u;for(auto v:key){h^=v;h*=16777619u;}return h&(Slots-1);}
    static bool Same(const Entry &entry,const Key &key){return entry.value&&!std::memcmp(entry.key.data(),key.data(),sizeof(Key));}
public:
    void Clear(){last_={};for(auto &e:entries_)e={};}
    template<class Resolve>HRESULT Get(const Key &key,bool enabled,Value *&out,Resolve resolve){
        ++counts.requests;out=nullptr;
        if(enabled&&!resolvedStatesReference){
            if(Same(last_,key)){out=last_.value;++counts.hits;++resolvedStateHits;return S_OK;}
            auto &entry=entries_[Index(key)];
            if(Same(entry,key)){last_=entry;out=entry.value;++counts.hits;++resolvedStateHits;return S_OK;}
            ++counts.misses;
            HRESULT hr=resolve(out);
            if(hr==S_OK&&out){entry={key,out};last_=entry;}else ++counts.failures;
            return hr==S_OK&&!out?E_UNEXPECTED:hr;
        }
        ++counts.bypassed;HRESULT hr=resolve(out);if(hr!=S_OK||!out)++counts.failures;return hr==S_OK&&!out?E_UNEXPECTED:hr;
    }
};
// Every original input read by BindStates is represented. Stencil reference and
// viewport are dynamic binding values and remain live in the binding path.
inline constexpr std::array<unsigned,3> RasterInputStates{D3DRS_FILLMODE,D3DRS_CULLMODE,D3DRS_ZBIAS};
inline constexpr std::array<unsigned,6> BlendInputStates{D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_COLORWRITEENABLE,D3DRS_ZWRITEENABLE};
inline constexpr std::array<unsigned,10> DepthInputStates{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_STENCILENABLE,D3DRS_STENCILMASK,D3DRS_STENCILWRITEMASK,D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,D3DRS_STENCILPASS,D3DRS_STENCILFUNC};
inline constexpr std::array<unsigned,10> SamplerInputStates{D3DTSS_ADDRESSU,D3DTSS_ADDRESSV,D3DTSS_ADDRESSW,D3DTSS_MINFILTER,D3DTSS_MAGFILTER,D3DTSS_MIPFILTER,D3DTSS_MAXANISOTROPY,D3DTSS_MIPMAPLODBIAS,D3DTSS_MAXMIPLEVEL,D3DTSS_BORDERCOLOR};
template<std::size_t N,class States>std::array<DWORD,N+1> ResolvedStateKey(const States &states,const std::array<unsigned,N> &inputs,bool extra){
    std::array<DWORD,N+1> key{};for(unsigned i=0;i<N;++i)key[i]=states[inputs[i]];key[N]=extra?1u:0u;return key;
}
}
