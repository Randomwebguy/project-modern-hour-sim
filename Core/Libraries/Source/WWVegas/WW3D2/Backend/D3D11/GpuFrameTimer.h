// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
namespace GenZH::Modern {
class GpuFrameTimer {
    struct Slot{Microsoft::WRL::ComPtr<ID3D11Query> disjoint,start,end;bool pending=false;};
    std::array<Slot,8> slots_;int active_=-1;bool available_=false;
public:
    void Reset(){for(auto &slot:slots_)slot=Slot{};active_=-1;available_=false;}
    void Initialize(ID3D11Device *device){
        Reset();for(auto &slot:slots_){D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
            if(FAILED(device->CreateQuery(&d,slot.disjoint.GetAddressOf())))return;
            d.Query=D3D11_QUERY_TIMESTAMP;
            if(FAILED(device->CreateQuery(&d,slot.start.GetAddressOf()))||FAILED(device->CreateQuery(&d,slot.end.GetAddressOf())))return;
        }available_=true;
    }
    template<class Ready>void Poll(ID3D11DeviceContext *context,Ready ready){
        if(!available_)return;
        for(auto &slot:slots_)if(slot.pending){
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            HRESULT hr=context->GetData(slot.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE)continue;if(FAILED(hr)||d.Disjoint||!d.Frequency){slot.pending=false;continue;}
            std::uint64_t first=0,last=0;
            if(context->GetData(slot.start.Get(),&first,sizeof(first),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||
               context->GetData(slot.end.Get(),&last,sizeof(last),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
            slot.pending=false;if(last>=first)ready(static_cast<double>(last-first)*1000/static_cast<double>(d.Frequency));
        }
    }
    void Begin(ID3D11DeviceContext *context){
        if(!available_||active_>=0)return;
        for(unsigned i=0;i<slots_.size();++i)if(!slots_[i].pending){active_=static_cast<int>(i);context->Begin(slots_[i].disjoint.Get());context->End(slots_[i].start.Get());return;}
        // Skip a sample when the GPU is behind. Never wait or force a flush.
    }
    void End(ID3D11DeviceContext *context){
        if(active_<0)return;auto &slot=slots_[static_cast<unsigned>(active_)];context->End(slot.end.Get());context->End(slot.disjoint.Get());slot.pending=true;active_=-1;
    }
};
}
