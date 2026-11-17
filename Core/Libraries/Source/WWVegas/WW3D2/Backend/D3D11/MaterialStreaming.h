// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MaterialPreparation.h"
#include <deque>
#include <unordered_map>
#include "MaterialMemoryBudget.h"
#include <dxgi1_4.h>
#include <chrono>
namespace GenZH::Modern {
class MaterialUpload final {
 std::unique_ptr<PreparedMaterial> data_;
 Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
 enum class Stage{Create,Rows,Mips,Done};Stage stage_=Stage::Create;
 unsigned mip_=0,row_=0;std::size_t uploaded_=0,resident_=0;
public:
 explicit MaterialUpload(std::unique_ptr<PreparedMaterial> data);
 HRESULT Pump(ID3D11Device *device,ID3D11DeviceContext *context,std::size_t budget=4*1024*1024,double milliseconds=2);
 bool Ready()const{return stage_==Stage::Done;}
 std::size_t Uploaded()const{return uploaded_;}std::size_t ResidentBytes()const{return resident_;}
 std::size_t RemainingCPU()const{return data_?data_->CPUBytes():0;}
 const auto &View()const{return view_;}
};
struct MaterialViews {std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,8> maps;};
// Validated, canonical metadata. Construct once per texture name/root change;
// every lookup still marks the resident entry used and observes async readiness.
class MaterialLookup final {
 friend class MaterialStreaming;
 std::filesystem::path root_,stem_;std::wstring key_;
public:
 MaterialLookup()=default;
 MaterialLookup(const std::filesystem::path &root,const std::filesystem::path &stem);
 bool Valid()const{return !key_.empty();}
 const auto &Stem()const{return stem_;}const auto &Key()const{return key_;}
};
// Per-lookup memo of the entry found last time. Entries are destroyed only by
// eviction or Reset, which advance the epoch; map nodes never move otherwise.
struct MaterialLookupMemo {const void *entry=nullptr;std::uint64_t epoch=0;};
// Render-thread cache/queue. Only the preparation worker performs file I/O.
class MaterialStreaming final {
 struct Entry {
  MaterialRequest request;MaterialViews views;std::size_t bytes=0;std::uint64_t used=0;
  bool ready=false,failed=false,queued=false;
 };
 std::unordered_map<std::wstring,Entry> entries_;std::deque<std::wstring> queue_;
 MaterialPreparation worker_;std::unique_ptr<MaterialUpload> upload_;
 std::wstring active_;unsigned component_=0;std::uint64_t frame_=0,evictions_=0,epoch_=1;
 std::size_t resident_=0,cacheBudget_;std::uint64_t submitted_=0;std::string error_;
 Microsoft::WRL::ComPtr<IDXGIAdapter3> budgetAdapter_;ID3D11Device *budgetDevice_=nullptr;
 std::chrono::steady_clock::time_point budgetTick_{};GeometryMemorySample budgetSample_{};
 void Trim();void FailActive(const std::string &error);
public:
 explicit MaterialStreaming(std::size_t cacheBudget=512*1024*1024):cacheBudget_(cacheBudget){}
 ~MaterialStreaming(){worker_.Stop();}
 const MaterialViews *FindOrRequest(const std::filesystem::path &root,const std::filesystem::path &stem);
 const MaterialViews *FindOrRequest(const MaterialLookup &lookup);
 // Same result and LRU mark as FindOrRequest(lookup), without hashing the key while the memo is valid.
 const MaterialViews *FindOrRequest(const MaterialLookup &lookup,MaterialLookupMemo &memo);
 void BeginFrame(){++frame_;}
 void UpdateBudget(ID3D11Device *device);
 std::size_t Budget()const{return cacheBudget_;}
 HRESULT Pump(ID3D11Device *device,ID3D11DeviceContext *context,bool allowStart=true,std::size_t bytes=4*1024*1024,double milliseconds=2);
 bool CPUBusy(){return !worker_.Available();}
 void Reset();void Stop(){Reset();worker_.Stop();}
 std::size_t ResidentBytes()const{return resident_;}std::size_t Queued()const{return queue_.size();}
 std::size_t Entries()const{return entries_.size();}std::uint64_t Evictions()const{return evictions_;}std::uint64_t Epoch()const{return epoch_;}
 bool Active()const{return !active_.empty();}std::uint64_t SubmittedBytes()const{return submitted_;}
 std::size_t UploadCPUBytes()const{return upload_?upload_->RemainingCPU():0;}
 bool Idle()const{return active_.empty()&&queue_.empty();}const auto &LastError()const{return error_;}
};
}
