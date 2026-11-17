// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <chrono>
namespace GenZH::Modern {
// perf116: Grass is the GPU grass draw (TerrainGrass.cpp), timed separately from the terrain draws around it.
enum class GpuPhase:unsigned {Upload,Capture,Shadow,Terrain,Scene,Post,HUD,Offscreen,Grass,GrassDraw,SceneBlend,SceneWater,Count};
// Sparse GPU submission intervals, not GPU utilization/busy time. No query
// flush, sleep, or blocking read; skip samples under GPU backlog.
class GpuPassTimer {
    static constexpr unsigned Points=40,Slots=8;
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        std::array<Microsoft::WRL::ComPtr<ID3D11Query>,Points> points;
        std::array<GpuPhase,Points> phases{};
        std::array<std::chrono::steady_clock::time_point,Points> cpuPoints{};
        unsigned count=0;bool pending=false,overflow=false;
    };
    std::array<Slot,Slots> slots_;
    int active_=-1;bool available_=false;std::uint64_t frame_=0,skipped_=0,discarded_=0;
public:
    void Reset(){for(auto &s:slots_)s=Slot{};active_=-1;available_=false;frame_=skipped_=discarded_=0;}
    void Initialize(ID3D11Device *device){
        Reset();for(auto &s:slots_){
            D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
            if(FAILED(device->CreateQuery(&d,s.disjoint.GetAddressOf())))return;
            d.Query=D3D11_QUERY_TIMESTAMP;
            for(auto &p:s.points)if(FAILED(device->CreateQuery(&d,p.GetAddressOf())))return;
        }available_=true;
    }
    void Begin(ID3D11DeviceContext *c){
        if(!available_||active_>=0||frame_++%8)return;
        for(unsigned i=0;i<Slots;++i)if(!slots_[i].pending){
            active_=static_cast<int>(i);auto &s=slots_[i];s.count=1;s.overflow=false;
            s.phases[0]=GpuPhase::Upload;c->Begin(s.disjoint.Get());c->End(s.points[0].Get());s.cpuPoints[0]=std::chrono::steady_clock::now();return;
        }++skipped_;
    }
    void Mark(ID3D11DeviceContext *c,GpuPhase phase){
        if(active_<0)return;auto &s=slots_[static_cast<unsigned>(active_)];
        if(s.phases[s.count-1]==phase)return;
        // Reserve one timestamp for the frame end. Never publish truncated sums.
        if(s.count>=Points-1){s.overflow=true;return;}
        c->End(s.points[s.count].Get());s.cpuPoints[s.count]=std::chrono::steady_clock::now();s.phases[s.count++]=phase;
    }
    void End(ID3D11DeviceContext *c){
        if(active_<0)return;auto &s=slots_[static_cast<unsigned>(active_)];
        c->End(s.points[s.count].Get());s.cpuPoints[s.count++]=std::chrono::steady_clock::now();c->End(s.disjoint.Get());s.pending=true;active_=-1;
    }
    template<class Ready>void PollDetailed(ID3D11DeviceContext *c,Ready ready){
        if(!available_)return;
        for(auto &s:slots_)if(s.pending){
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            HRESULT hr=c->GetData(s.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE)continue;
            if(FAILED(hr)||d.Disjoint||!d.Frequency||s.overflow){s.pending=false;++discarded_;continue;}
            std::array<std::uint64_t,Points> ticks{};bool complete=true;
            for(unsigned i=0;i<s.count;++i)if(c->GetData(s.points[i].Get(),&ticks[i],sizeof(ticks[i]),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK){complete=false;break;}
            if(!complete)continue;
            s.pending=false;std::array<double,static_cast<unsigned>(GpuPhase::Count)> result{},cpu{};
            for(unsigned i=0;i+1<s.count;++i){
                if(ticks[i+1]<ticks[i]){complete=false;break;}
                result[static_cast<unsigned>(s.phases[i])]+=static_cast<double>(ticks[i+1]-ticks[i])*1000/static_cast<double>(d.Frequency);
                cpu[static_cast<unsigned>(s.phases[i])]+=std::chrono::duration<double,std::milli>(s.cpuPoints[i+1]-s.cpuPoints[i]).count();
            }
            if(complete)ready(result,cpu);else ++discarded_;
        }
    }
    template<class Ready>void Poll(ID3D11DeviceContext *c,Ready ready){PollDetailed(c,[&](const auto &gpu,const auto &){ready(gpu);});}
    std::uint64_t Skipped()const{return skipped_;}
    std::uint64_t Discarded()const{return discarded_;}
};
}
