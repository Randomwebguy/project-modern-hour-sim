// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <array>
#include <chrono>
#include <filesystem>
#include "LoadTelemetry.h"
#include <fstream>
#include <iomanip>
#include <cstdint>
namespace GenZH::Modern {
// End-of-successful-Present intervals include native simulation, throttle and
// driver waits. They are CPU wall time, not GPU busy time or display scanout.
class PresentedFrameTelemetry {
public:
    struct Sample {std::int64_t epoch=0;std::uint64_t frame=0;double sceneMs=0,presentMs=0,intervalMs=-1;};
private:
    std::array<Sample,120> samples_{};
    std::size_t used_=0;std::uint64_t frame_=0;
    std::chrono::steady_clock::time_point previous_{};
    std::chrono::steady_clock::time_point lastFlush_{},lastAdded_{};
public:
    ~PresentedFrameTelemetry(){Flush();}
    void Reset(){Flush();previous_={};lastFlush_={};}
    void Add(double sceneMs,double presentMs,std::chrono::steady_clock::time_point now){
        auto &s=samples_[used_++];s.frame=++frame_;s.sceneMs=sceneMs;s.presentMs=presentMs;
        s.epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        s.intervalMs=previous_==std::chrono::steady_clock::time_point{}?-1:std::chrono::duration<double,std::milli>(now-previous_).count();
        previous_=now;lastAdded_=now;if(lastFlush_==std::chrono::steady_clock::time_point{})lastFlush_=now;
        if(used_==samples_.size()||now-lastFlush_>=std::chrono::seconds(1))Flush();
    }
    std::size_t Pending()const{return used_;}
    const Sample &PendingSample(std::size_t i)const{return samples_[i];}
    void Flush(){
        if(!used_)return;
        wchar_t root[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",root,32768);
        if(!length||length>=32768){used_=0;return;}
        const auto path=std::filesystem::path(root)/L"renderer-frame-v21.csv";
        std::error_code error;const bool header=!std::filesystem::exists(path,error)||std::filesystem::file_size(path,error)==0;
        GenZHLoad::AppendLog out(path,std::ios::app);
        if(out){
            if(header)out<<"pid,utc_epoch_ms,frame_id,scene_cpu_ms,present_cpu_ms,presented_interval_ms\n";
            out<<std::fixed<<std::setprecision(6);
            for(std::size_t i=0;i<used_;++i){const auto &s=samples_[i];out<<GetCurrentProcessId()<<','<<s.epoch<<','<<s.frame<<','<<s.sceneMs<<','<<s.presentMs<<','<<s.intervalMs<<'\n';}
        }
        // Bounded even if the profile is unavailable. No growing history queue.
        used_=0;lastFlush_=lastAdded_;
    }
};
}
