// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include "LoadTelemetry.h"
#include <fstream>
#include <filesystem>
#include <windows.h>
namespace GenZH::Modern {
enum class CaptureWork:unsigned {DepthPrepare,DepthSort,SortedSubmit,Snapshot,TextureCapture,Count};
struct CaptureWorkCounter {std::uint64_t calls=0,samples=0;double sampledMs=0,maxSampleMs=0;};
inline std::array<CaptureWorkCounter,static_cast<unsigned>(CaptureWork::Count)> captureWorkCounts{};
inline bool captureWorkEnabled=true;
// All scopes run on the native render thread. Sparse draw scopes avoid adding
// clocks to every triangle. Parent/child times overlap and must not be summed.
class CaptureWorkScope {
    CaptureWorkCounter *counter_=nullptr;std::chrono::steady_clock::time_point start_;
public:
    explicit CaptureWorkScope(CaptureWork kind,unsigned stride=1){
        if(!captureWorkEnabled)return;auto &c=captureWorkCounts[static_cast<unsigned>(kind)];
        if(c.calls++%stride)return;counter_=&c;start_=std::chrono::steady_clock::now();
    }
    ~CaptureWorkScope(){if(counter_){const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start_).count();
        ++counter_->samples;counter_->sampledMs+=ms;counter_->maxSampleMs=(std::max)(counter_->maxSampleMs,ms);}}
};
inline void WriteCaptureWorkTelemetry(const std::filesystem::path &profile,std::uint64_t frames,std::uint64_t calculations,std::uint64_t reuses){
    const auto path=profile/L"renderer-capture-v80.csv";std::error_code ec;
    const bool header=!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)==0;
    GenZHLoad::AppendLog out(path,std::ios::app);if(!out)return;
    if(header)out<<"pid,utc_epoch_ms,frames,scope,calls,samples,sampled_wall_ms,max_sample_wall_ms,derived_calculations,derived_reuses\n";
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const char *names[]={"depth_prepare","depth_sort","sorted_submit","snapshot","texture_capture"};
    for(unsigned i=0;i<captureWorkCounts.size();++i){const auto &c=captureWorkCounts[i];
        out<<GetCurrentProcessId()<<','<<epoch<<','<<frames<<','<<names[i]<<','<<c.calls<<','<<c.samples<<','<<c.sampledMs<<','<<c.maxSampleMs<<','<<calculations<<','<<reuses<<'\n';}
}
}
