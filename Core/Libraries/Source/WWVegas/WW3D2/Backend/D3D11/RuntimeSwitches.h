// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LoadTelemetry.h"
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace GenZH::Modern {
// Same-process A/B measurement for the offline test profile. While
// GENZH_USER_DATA_DIR\runtime-switches.txt exists and was written after this
// process started, its KEY=on|off lines replace the launch values of the
// listed reference switches. Every listed switch is re-read each frame and
// selects between pixel-equivalent paths; nothing here changes content or
// quality, except the look A/B keys GENZH_SHROUD_FOG(_FLAT) and
// GENZH_TERRAIN_LIBRARY (native atlas against the world-space library) and
// GENZH_TERRAIN_RELIEF (library macro relief) and GENZH_GRASS (GPU grass against
// the legacy geometry-shader blades), GENZH_GRASS_SHADOWS, GENZH_GRASS_INTERACTION and
// GENZH_TAA (temporal AA against the spatial edge AA), GENZH_TERRAIN_FIELD (continuous
// ground-class field against the per-cell class staircase), GENZH_TERRAIN_HEX (hex stochastic
// tiling against the plain repeating library tiles), GENZH_TERRAIN_WET (wet ground: porous
// darkening, standing water, rain rings, against the uniform wet film), GENZH_CAS (sharpening
// of the TAA output against the plain copy), GENZH_ACCUMULATION (settled snow / dust on
// the upward faces of world objects, from the map climate), GENZH_BLOOM (off / legacy: the
// former 3x3 tone-map bloom / default: the mip-chain bloom of the world image), GENZH_WATER_SHORE
// (per-pixel soft shore of the game's water against the native destination-alpha shore), GENZH_SCENE_EXTRA_BINDINGS (per-draw binds of the main scene cached in the scene binding cache against the direct binds). Removing the file restores the launch values. A stale file from an
// earlier process is ignored. Polled at most twice per second.
class RuntimeSwitches {
public:
    static constexpr std::array<const char*,40> Keys{
        "GENZH_SORTED_MATERIAL_PALETTE","GENZH_SORTED_COLOR_PALETTE","GENZH_DEPTH_RADIX",
        "GENZH_DERIVED_FIXED","GENZH_CAPTURE_PROFILE","GENZH_MESH_RAY_ACCEL","GENZH_MIRRORED_LOCKS",
        "GENZH_SORTED_RUN_TEMPLATES","GENZH_SORTED_RUN_TEMPLATE_VERIFY",
        "GENZH_COPY_RECTS_REPEAT","GENZH_COPY_RECTS_VERIFY","GENZH_STATIC_TEXT_REUSE","GENZH_SNAPSHOT_RECYCLE",
        "GENZH_STATIC_TEXT_VERIFY","GENZH_STAGED_PACKETS","GENZH_STATE_REVISION","GENZH_SIMD_EQUAL","GENZH_ASYNC_TELEMETRY",
        "GENZH_SHROUD_FOG","GENZH_SHROUD_FOG_FLAT","GENZH_TERRAIN_LIBRARY","GENZH_TERRAIN_RELIEF","GENZH_GRASS","GENZH_GRASS_SHADOWS","GENZH_GRASS_INTERACTION","GENZH_TAA","GENZH_TERRAIN_FIELD","GENZH_TERRAIN_HEX","GENZH_TERRAIN_WET","GENZH_CAS","GENZH_ACCUMULATION","GENZH_BLOOM","GENZH_WATER_SHORE","GENZH_ROCKS","GENZH_GRADE","GENZH_MOTION","GENZH_GI","GENZH_WATER_REFLECTION","GENZH_DECAL","GENZH_SCENE_EXTRA_BINDINGS"};
    void Poll(){
        const double now=GenZHLoad::Seconds();if(now<nextPoll_)return;nextPoll_=now+.5;
        if(!captured_)CaptureLaunchValues();
        wchar_t root[32768]{};const DWORD n=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",root,32768);if(!n||n>=32768)return;
        const std::wstring path=std::wstring(root)+L"\\runtime-switches.txt";
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const bool present=GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data)&&
            CompareFileTime(&data.ftLastWriteTime,&processStart_)>0&&data.nFileSizeHigh==0&&data.nFileSizeLow<=4096;
        if(!present){if(active_){Restore();active_=false;}return;}
        if(active_&&!CompareFileTime(&data.ftLastWriteTime,&stamp_)&&data.nFileSizeLow==size_)return;
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;
        char text[4097]{};DWORD read=0;const BOOL ok=ReadFile(file,text,4096,&read,nullptr);CloseHandle(file);if(!ok)return;
        stamp_=data.ftLastWriteTime;size_=data.nFileSizeLow;active_=true;
        // The file is the complete requested state: unlisted keys use launch values.
        std::array<std::string,Keys.size()> wanted=launch_;std::array<bool,Keys.size()> set{};
        for(char *line=text;*line;){
            char *end=line;while(*end&&*end!='\n'&&*end!='\r')++end;const char next=*end;*end=0;
            if(char *eq=std::strchr(line,'=')){*eq=0;const char *value=eq+1;
                for(unsigned i=0;i<Keys.size();++i)if(!std::strcmp(line,Keys[i])&&(!std::strcmp(value,"on")||!std::strcmp(value,"off"))){wanted[i]=value;set[i]=true;}}
            line=next?end+1:end;
        }
        std::string applied;
        for(unsigned i=0;i<Keys.size();++i){
            char current[32]{};GetEnvironmentVariableA(Keys[i],current,sizeof(current));
            if(wanted[i]==current)continue;
            SetEnvironmentVariableA(Keys[i],wanted[i].empty()?nullptr:wanted[i].c_str());
            applied+=std::string(applied.empty()?"":" ")+Keys[i]+"="+(wanted[i].empty()?"launch":wanted[i])+(set[i]?"":"(launch)");
        }
        if(!applied.empty())GenZHLoad::Record("runtime_switch_v82",applied,0);
    }
private:
    void CaptureLaunchValues(){
        captured_=true;FILETIME created{},exited{},kernel{},user{};
        if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))processStart_=created;
        for(unsigned i=0;i<Keys.size();++i){char value[32]{};const DWORD n=GetEnvironmentVariableA(Keys[i],value,sizeof(value));launch_[i]=n&&n<sizeof(value)?value:"";}
    }
    void Restore(){
        std::string applied;
        for(unsigned i=0;i<Keys.size();++i){char current[32]{};GetEnvironmentVariableA(Keys[i],current,sizeof(current));
            if(launch_[i]==current)continue;SetEnvironmentVariableA(Keys[i],launch_[i].empty()?nullptr:launch_[i].c_str());
            applied+=std::string(applied.empty()?"":" ")+Keys[i]+"=launch";}
        if(!applied.empty())GenZHLoad::Record("runtime_switch_v82",applied+" (file removed)",0);
    }
    std::array<std::string,Keys.size()> launch_{};FILETIME processStart_{},stamp_{};DWORD size_=0;
    double nextPoll_=0;bool captured_=false,active_=false;
};
}
