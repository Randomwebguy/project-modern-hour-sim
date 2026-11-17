// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GeometryMemoryBudget.h"
#include "GeometryCache.h"
#include <dxgi1_4.h>
#include <chrono>
#include <filesystem>
#include "LoadTelemetry.h"
#include <fstream>
namespace GenZH::Modern {
class GeometryMemoryMonitor {
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    ID3D11Device *identity_=nullptr;
    std::chrono::steady_clock::time_point sampleTime_{},logTime_{};
    GeometryMemorySample sample_{};
    bool initialized_=false;
public:
    void Reset(){adapter_.Reset();identity_=nullptr;sampleTime_=logTime_={};sample_={};initialized_=false;}
    bool Update(ID3D11Device *device,GeometryCache &cache){
        if(!device)return false;
        if(device!=identity_){
            Reset();identity_=device;
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())))&&SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf())))adapter.As(&adapter_);
        }
        const auto now=std::chrono::steady_clock::now();
        if(initialized_&&now-sampleTime_<std::chrono::seconds(1))return false;
        sampleTime_=now;initialized_=true;sample_={};
        MEMORYSTATUSEX cpu{};cpu.dwLength=sizeof(cpu);
        if(GlobalMemoryStatusEx(&cpu)){
            sample_.cpuKnown=true;sample_.physicalAvailable=cpu.ullAvailPhys;sample_.virtualAvailable=cpu.ullAvailVirtual;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO video{};
        if(adapter_&&SUCCEEDED(adapter_->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&video))&&video.Budget){
            sample_.videoKnown=true;sample_.videoBudget=video.Budget;sample_.videoUsage=video.CurrentUsage;
        }
        const auto target=static_cast<std::size_t>(GeometryMemoryTarget(sample_,cache.Bytes(),sizeof(void*)*8));
        // 16 MiB steps avoid reacting to small budget fluctuations. Reclaim is
        // spread by GeometryCache::Trim; OS queries do not wait on the GPU.
        if(target==cache.Limit())return false;
        cache.SetLimit(target);return true;
    }
    const GeometryMemorySample &Sample()const{return sample_;}
    void Record(const GeometryCache &cache){
        const auto now=std::chrono::steady_clock::now();
        if(logTime_!=std::chrono::steady_clock::time_point{}&&now-logTime_<std::chrono::seconds(1))return;
        logTime_=now;
        wchar_t root[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",root,32768);if(!length||length>=32768)return;
        const auto path=std::filesystem::path(root)/L"renderer-memory-v20.csv";
        std::error_code error;const bool header=!std::filesystem::exists(path,error)||std::filesystem::file_size(path,error)==0;
        GenZHLoad::AppendLog out(path,std::ios::app);if(!out)return;
        if(header)out<<"pid,utc_epoch_ms,pointer_bits,cache_limit_bytes,cache_owned_bytes,cache_entries,budget_evictions_total,expired_prunes_total,obsolete_prunes_total,oversized_skips_total,pressure_skips_total,video_budget_known,video_budget_bytes,video_usage_bytes,cpu_memory_known,physical_available_bytes,virtual_available_bytes\n";
        const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        out<<GetCurrentProcessId()<<','<<epoch<<','<<sizeof(void*)*8<<','<<cache.Limit()<<','<<cache.Bytes()<<','<<cache.Size()<<','
            <<cache.Evictions()<<','<<cache.Expired()<<','<<cache.Obsolete()<<','<<cache.Oversized()<<','<<cache.PressureSkipped()<<','
            <<sample_.videoKnown<<','<<sample_.videoBudget<<','<<sample_.videoUsage<<','<<sample_.cpuKnown<<','<<sample_.physicalAvailable<<','<<sample_.virtualAvailable<<'\n';
    }
};
}
