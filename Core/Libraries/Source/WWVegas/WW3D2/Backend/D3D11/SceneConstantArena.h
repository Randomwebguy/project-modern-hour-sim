// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
namespace GenZH::Modern {
// perf114: the mapped constant buffer is write-combined memory. Whole 16-byte lines are written with
// streaming stores, so a draw's slice is not pulled through the cache; the caller issues _mm_sfence before Unmap.
inline void CopyToMapped(unsigned char *dest,const void *source,std::size_t bytes){
    if(reinterpret_cast<std::uintptr_t>(dest)&15u){std::memcpy(dest,source,bytes);return;}
    const auto *from=static_cast<const unsigned char*>(source);
    std::size_t at=0;
    for(;at+16<=bytes;at+=16)_mm_stream_si128(reinterpret_cast<__m128i*>(dest+at),_mm_loadu_si128(reinterpret_cast<const __m128i*>(from+at)));
    if(at<bytes)std::memcpy(dest+at,from+at,bytes-at);
}
inline thread_local bool sceneConstantReference=false;
struct ScopedSceneConstantReference {
    bool previous;
    explicit ScopedSceneConstantReference(bool reference):previous(sceneConstantReference){sceneConstantReference=reference;}
    ~ScopedSceneConstantReference(){sceneConstantReference=previous;}
};
struct SceneConstantCounts {std::uint64_t uploads=0,slices=0,binds=0,bypassed=0,failures=0;};
inline thread_local SceneConstantCounts sceneConstantCounts{};
// Exact per-draw bytes, uploaded once per queued scene. Offsets and ranges are
// multiples of 16 float4 constants (256 bytes). No geometry/material batching,
// reordering, shader changes or quality trade-off. DISCARD preserves earlier
// GPU submissions; unsupported drivers and allocation failures use the old CB.
template<class Constants> class SceneConstantArena {
    static constexpr UINT Stride=(sizeof(Constants)+255u)&~255u;
    static_assert(Stride<=65536&&sizeof(Constants)%16==0);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
    UINT capacity_=0,count_=0;
public:
    void Reset(){context_.Reset();buffer_.Reset();capacity_=count_=0;}
    void Initialize(ID3D11Device *device,ID3D11DeviceContext *context,bool enabled){
        Reset();D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        if(!enabled||!device||!context||FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))||!options.ConstantBufferOffsetting)return;
        context->QueryInterface(IID_PPV_ARGS(context_.GetAddressOf()));
    }
    bool Supported()const{return bool(context_);}
    bool Has(UINT index)const{return context_&&buffer_&&index<count_;}
    void End(){count_=0;}
    template<class Groups,class Fill> bool Prepare(ID3D11Device *device,const Groups &groups,Fill fill){
        End();
        constexpr std::size_t budget=32u*1024u*1024u;
        if(sceneConstantReference||!context_||groups.empty()||groups.size()>budget/Stride){++sceneConstantCounts.bypassed;return false;}
        const UINT bytes=static_cast<UINT>(groups.size()*Stride);
        if(bytes>capacity_){
            UINT capacity=capacity_?capacity_:65536;
            while(capacity<bytes)capacity*=2;
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=capacity;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            desc.Usage=D3D11_USAGE_DYNAMIC;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            Microsoft::WRL::ComPtr<ID3D11Buffer> next;
            if(FAILED(device->CreateBuffer(&desc,nullptr,next.GetAddressOf()))){++sceneConstantCounts.failures;return false;}
            buffer_=std::move(next);capacity_=capacity;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(context_->Map(buffer_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))){++sceneConstantCounts.failures;return false;}
        auto *dest=static_cast<unsigned char*>(mapped.pData);
        for(const auto &group:groups){Constants constants=fill(*group.front());CopyToMapped(dest,&constants,sizeof(constants));std::memset(dest+sizeof(constants),0,Stride-sizeof(constants));dest+=Stride;}
        _mm_sfence();context_->Unmap(buffer_.Get(),0);count_=static_cast<UINT>(groups.size());
        ++sceneConstantCounts.uploads;sceneConstantCounts.slices+=count_;return true;
    }
    bool Bind(UINT index){
        if(!Has(index))return false;
        ID3D11Buffer *buffer=buffer_.Get();const UINT first=index*(Stride/16),size=Stride/16;
        context_->VSSetConstantBuffers1(0,1,&buffer,&first,&size);
        context_->PSSetConstantBuffers1(0,1,&buffer,&first,&size);
        context_->GSSetConstantBuffers1(0,1,&buffer,&first,&size);
        context_->HSSetConstantBuffers1(0,1,&buffer,&first,&size);
        context_->DSSetConstantBuffers1(0,1,&buffer,&first,&size);
        ++sceneConstantCounts.binds;return true;
    }
};
}
