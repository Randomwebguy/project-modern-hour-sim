// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <ios>
#include <sstream>
#include <string>
#include <mutex>
#include <thread>
namespace GenZHLoad {
inline double Seconds(){LARGE_INTEGER counter{},frequency{};QueryPerformanceCounter(&counter);QueryPerformanceFrequency(&frequency);return static_cast<double>(counter.QuadPart)/static_cast<double>(frequency.QuadPart);}
// perf91: telemetry appends are written by one background thread, in arrival
// order (per file and overall), so the render thread never opens, writes or
// closes a file. Once a second ~30 CSV files were appended inside one frame
// (a ~23 ms frame). Rows keep the timestamps taken when they were formatted.
// GENZH_ASYNC_TELEMETRY=off writes synchronously as before. Pending appends
// are written before the process exits (static destruction joins the writer).
inline std::atomic<bool> asyncTelemetry{true};
inline std::atomic<bool> appendQueueClosed{false};
// header: written first when the file is empty (the loading-v17 convention).
// Telemetry CSV writers put their "pid,..." header line in front of the rows
// when their file did not exist yet; with queued writes that test can run
// before the previous append reached the disk, so a leading "pid," header
// line is dropped when the file already has content (data rows start with
// the process id). The file therefore gets its header exactly once, as before.
inline void WriteAppend(const std::wstring &path,const std::string &text,const std::string &header){
    FILE *file=nullptr;if(_wfopen_s(&file,path.c_str(),L"ab")||!file)return;
    std::fseek(file,0,SEEK_END);const bool empty=std::ftell(file)==0;
    if(!header.empty()&&empty)std::fputs(header.c_str(),file);
    std::size_t skip=0;
    if(!empty&&text.compare(0,4,"pid,")==0){const auto end=text.find('\n');skip=end==std::string::npos?text.size():end+1;}
    if(text.size()>skip)std::fwrite(text.data()+skip,1,text.size()-skip,file);
    std::fclose(file);
}
class AppendQueue {
    struct Item {std::wstring path;std::string text,header;};
    std::mutex mutex_;std::condition_variable wake_,idle_;std::deque<Item> items_;std::thread worker_;
    bool stop_=false,busy_=false;
    static void Write(const Item &item){WriteAppend(item.path,item.text,item.header);}
    void Run(){
        std::unique_lock<std::mutex> lock(mutex_);
        for(;;){
            wake_.wait(lock,[&]{return stop_||!items_.empty();});
            if(items_.empty()){if(stop_)return;continue;}
            std::deque<Item> batch;batch.swap(items_);busy_=true;lock.unlock();
            for(const auto &item:batch)Write(item);
            lock.lock();busy_=false;idle_.notify_all();
        }
    }
public:
    static AppendQueue &Instance(){static AppendQueue queue;return queue;}
    void Append(std::wstring path,std::string text,std::string header=std::string()){
        std::unique_lock<std::mutex> lock(mutex_);
        if(stop_||!asyncTelemetry.load(std::memory_order_relaxed)){
            // Keep the file order: everything queued earlier is written first.
            idle_.wait(lock,[&]{return items_.empty()&&!busy_;});
            Write({std::move(path),std::move(text),std::move(header)});return;
        }
        if(!worker_.joinable())worker_=std::thread([this]{Run();});
        items_.push_back({std::move(path),std::move(text),std::move(header)});wake_.notify_one();
    }
    void Drain(){std::unique_lock<std::mutex> lock(mutex_);idle_.wait(lock,[&]{return items_.empty()&&!busy_;});}
    ~AppendQueue(){
        {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}
        wake_.notify_all();if(worker_.joinable())worker_.join();
        appendQueueClosed.store(true);
    }
};
// Appends after the queue's static destruction are written synchronously.
inline void QueueAppend(std::wstring path,std::string text,std::string header=std::string()){
    if(appendQueueClosed.load()){WriteAppend(path,text,header);return;}
    AppendQueue::Instance().Append(std::move(path),std::move(text),std::move(header));
}
// Drop-in for `std::ofstream log(path,std::ios::app)` telemetry appends: the
// text is formatted exactly as an ofstream would and queued when destroyed.
class AppendLog:public std::ostringstream {
    std::wstring path_;
public:
    explicit AppendLog(const std::wstring &path,std::ios_base::openmode=std::ios_base::app):path_(path){}
    ~AppendLog(){std::string text=str();if(!text.empty())QueueAppend(std::move(path_),std::move(text));}
};
inline void Record(const char *phase,const std::string &detail,double milliseconds,std::uint64_t bytes=0,bool hit=false){
 wchar_t root[32768]{};DWORD n=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",root,32768);if(!n||n>=32768)return;
 std::wstring path(root);path+=L"\\loading-v17.csv";
 std::string safe=detail;for(char &c:safe)if(c==','||c=='\n'||c=='\r'||c=='"')c=' ';
 FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER ticks{};ticks.LowPart=now.dwLowDateTime;ticks.HighPart=now.dwHighDateTime;
 const auto epoch=(ticks.QuadPart-116444736000000000ull)/10000;
 char head[96];const int headBytes=std::snprintf(head,sizeof(head),"%lu,%llu,",GetCurrentProcessId(),static_cast<unsigned long long>(epoch));
 char tail[96];const int tailBytes=std::snprintf(tail,sizeof(tail),",%.3f,%llu,%u\n",milliseconds,static_cast<unsigned long long>(bytes),hit?1u:0u);
 std::string line;line.reserve(std::size_t(headBytes)+std::strlen(phase)+1+safe.size()+std::size_t(tailBytes));
 line.append(head,std::size_t(headBytes));line.append(phase);line.push_back(',');line.append(safe);line.append(tail,std::size_t(tailBytes));
 QueueAppend(std::move(path),std::move(line),"pid,utc_epoch_ms,phase,detail,duration_ms,bytes,cache_hit\n");
}
class Scope {
 const char *phase_;std::string detail_;double start_;
public:
 std::uint64_t bytes=0;bool hit=false;
 Scope(const char *phase,const std::string &detail):phase_(phase),detail_(detail),start_(Seconds()){}
 ~Scope(){Record(phase_,detail_,(Seconds()-start_)*1000,bytes,hit);}
 Scope(const Scope&)=delete;Scope&operator=(const Scope&)=delete;
};
}
