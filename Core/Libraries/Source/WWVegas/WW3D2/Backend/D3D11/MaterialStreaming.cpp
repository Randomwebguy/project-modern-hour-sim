// SPDX-License-Identifier: GPL-3.0-or-later
#include "MaterialStreaming.h"
#include "LoadTelemetry.h"
#include <algorithm>
#include <cwctype>
#include <sstream>
namespace GenZH::Modern {
void MaterialStreaming::UpdateBudget(ID3D11Device *device){
 if(!device)return;
 if(device!=budgetDevice_){
  budgetDevice_=device;budgetAdapter_.Reset();budgetTick_={};
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())))&&SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf())))adapter.As(&budgetAdapter_);
 }
 const auto now=std::chrono::steady_clock::now();if(budgetTick_!=std::chrono::steady_clock::time_point{}&&now-budgetTick_<std::chrono::seconds(1))return;
 budgetTick_=now;budgetSample_={};MEMORYSTATUSEX cpu{};cpu.dwLength=sizeof(cpu);
 if(GlobalMemoryStatusEx(&cpu)){budgetSample_.cpuKnown=true;budgetSample_.physicalAvailable=cpu.ullAvailPhys;budgetSample_.virtualAvailable=cpu.ullAvailVirtual;}
 DXGI_QUERY_VIDEO_MEMORY_INFO video{};
 if(budgetAdapter_&&SUCCEEDED(budgetAdapter_->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&video))&&video.Budget){budgetSample_.videoKnown=true;budgetSample_.videoBudget=video.Budget;budgetSample_.videoUsage=video.CurrentUsage;}
 cacheBudget_=static_cast<std::size_t>(MaterialMemoryTarget(budgetSample_,resident_,sizeof(void*)*8));Trim();
 wchar_t root[32768]{};const auto length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",root,32768);
 if(length&&length<32768){const auto file=std::filesystem::path(root)/L"material-cache-budget-v57.csv";std::error_code ec;const bool header=!std::filesystem::exists(file,ec)||std::filesystem::file_size(file,ec)==0;GenZHLoad::AppendLog log(file,std::ios::app);
  if(log){if(header)log<<"pid,utc_epoch_ms,cache_budget_bytes,cache_owned_bytes,video_budget_bytes,video_usage_bytes,physical_available_bytes,virtual_available_bytes,metadata_limit\n";
   const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
   log<<GetCurrentProcessId()<<','<<epoch<<','<<cacheBudget_<<','<<resident_<<','<<budgetSample_.videoBudget<<','<<budgetSample_.videoUsage<<','<<budgetSample_.physicalAvailable<<','<<budgetSample_.virtualAvailable<<",4096\n";}
 }
}
MaterialUpload::MaterialUpload(std::unique_ptr<PreparedMaterial> data):data_(std::move(data)){
 if(!data_)return;
 if(data_->fromDDS){for(const auto &mip:data_->dds.levels)resident_+=mip.SysMemSlicePitch;}
 else for(unsigned w=data_->rgba.width,h=data_->rgba.height;w&&h;w=std::max(1u,w/2),h=std::max(1u,h/2)){resident_+=static_cast<std::size_t>(w)*h*4;if(w==1&&h==1)break;}
}
HRESULT MaterialUpload::Pump(ID3D11Device *device,ID3D11DeviceContext *context,std::size_t budget,double milliseconds){
 if(!device||!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!data_||data_->finished||!data_->error.empty()||milliseconds<=0)return E_INVALIDARG;
 if(Ready())return S_OK;
 const bool bc=data_->fromDDS;const unsigned width=bc?data_->dds.width:data_->rgba.width,height=bc?data_->dds.height:data_->rgba.height;
 if(!width||!height||width>8192||height>8192||budget<(bc?data_->dds.levels.empty()?0:data_->dds.levels[0].SysMemPitch:width*4))return E_INVALIDARG;
 const auto start=GenZHLoad::Seconds();std::size_t used=0;
 while(!Ready()&&(GenZHLoad::Seconds()-start)*1000<milliseconds){
  if(stage_==Stage::Create){
   if((bc&&data_->dds.levels.size()!=data_->dds.mips)||(!bc&&data_->rgba.pixels.size()!=static_cast<std::size_t>(width)*height*4))return E_INVALIDARG;
   D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=bc?data_->dds.mips:0;desc.ArraySize=desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
   desc.Format=bc?data_->dds.format:DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(bc?0:D3D11_BIND_RENDER_TARGET);desc.MiscFlags=bc?0:D3D11_RESOURCE_MISC_GENERATE_MIPS;
   HRESULT hr=device->CreateTexture2D(&desc,nullptr,texture_.GetAddressOf());if(FAILED(hr))return hr;
   if(FAILED(hr=device->CreateShaderResourceView(texture_.Get(),nullptr,view_.GetAddressOf())))return hr;stage_=Stage::Rows;return S_OK;
  }
  if(stage_==Stage::Rows){
   const unsigned w=std::max(1u,width>>mip_),h=std::max(1u,height>>mip_);
   const unsigned pitch=bc?data_->dds.levels[mip_].SysMemPitch:w*4,totalRows=bc?std::max(1u,(h+3)/4):h;
   const auto rows=std::min<std::size_t>(totalRows-row_,(budget-used)/pitch);if(!rows)return S_OK;
   // BC boxes use texel coordinates, but source pitch/offset count block rows.
   const unsigned top=bc?row_*4:row_,bottom=bc?(row_+static_cast<unsigned>(rows))*4:row_+static_cast<unsigned>(rows);
   D3D11_BOX box{0,top,0,bc?(w+3)/4*4:w,bottom,1};const auto *source=bc?static_cast<const unsigned char*>(data_->dds.levels[mip_].pSysMem):data_->rgba.pixels.data();
   const bool whole=row_==0&&rows==totalRows;
   context->UpdateSubresource(texture_.Get(),mip_,whole?nullptr:&box,source+static_cast<std::size_t>(row_)*pitch,pitch,0);
   const auto bytes=rows*pitch;used+=bytes;uploaded_+=bytes;row_+=static_cast<unsigned>(rows);
   if(row_==totalRows){row_=0;if(bc){++mip_;if(mip_==data_->dds.mips){data_->dds={};stage_=Stage::Done;}}
    else{std::vector<std::uint8_t>().swap(data_->rgba.pixels);stage_=Stage::Mips;return S_OK;}}
   if(used==budget)return S_OK;continue;
  }
  if(stage_==Stage::Mips){context->GenerateMips(view_.Get());stage_=Stage::Done;return S_OK;}
 }
 return S_OK;
}
MaterialLookup::MaterialLookup(const std::filesystem::path &root,const std::filesystem::path &stem){
 if(root.empty()||stem.empty()||stem.has_parent_path()||stem==L"."||stem==L"..")return;
 root_=root;stem_=stem;key_=(root/stem).lexically_normal().wstring();
 std::transform(key_.begin(),key_.end(),key_.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
}
const MaterialViews *MaterialStreaming::FindOrRequest(const std::filesystem::path &root,const std::filesystem::path &stem){
 return FindOrRequest(MaterialLookup(root,stem));
}
const MaterialViews *MaterialStreaming::FindOrRequest(const MaterialLookup &lookup){
 if(!lookup.Valid())return nullptr;const auto &key=lookup.key_;const auto &root=lookup.root_;const auto &stem=lookup.stem_;
 auto found=entries_.find(key);if(found!=entries_.end()){found->second.used=frame_;return found->second.ready?&found->second.views:nullptr;}
 if(queue_.size()+(active_.empty()?0:1)>=64)return nullptr;
 if(entries_.size()>=4096){auto victim=entries_.end();for(auto it=entries_.begin();it!=entries_.end();++it)if((it->second.ready||it->second.failed)&&it->second.used!=frame_&&(victim==entries_.end()||it->second.used<victim->second.used))victim=it;
  if(victim==entries_.end())return nullptr;resident_-=victim->second.bytes;entries_.erase(victim);++evictions_;++epoch_;}
 Entry entry;entry.request.root=root;entry.request.stem=stem;entry.used=frame_;entry.queued=true;entries_.emplace(key,std::move(entry));queue_.push_back(key);return nullptr;
}
const MaterialViews *MaterialStreaming::FindOrRequest(const MaterialLookup &lookup,MaterialLookupMemo &memo){
 if(memo.entry&&memo.epoch==epoch_){auto *entry=static_cast<Entry*>(const_cast<void*>(memo.entry));entry->used=frame_;return entry->ready?&entry->views:nullptr;}
 const auto *result=FindOrRequest(lookup);
 const auto found=lookup.Valid()?entries_.find(lookup.key_):entries_.end();
 memo.entry=found!=entries_.end()?&found->second:nullptr;memo.epoch=epoch_;
 return result;
}
void MaterialStreaming::Trim(){
 while(resident_>cacheBudget_){auto victim=entries_.end();for(auto it=entries_.begin();it!=entries_.end();++it)
  if(it->second.ready&&it->second.bytes&&it->first!=active_&&it->second.used!=frame_&&(victim==entries_.end()||it->second.used<victim->second.used))victim=it;
  if(victim==entries_.end())break;resident_-=victim->second.bytes;entries_.erase(victim);++evictions_;++epoch_;}
}
void MaterialStreaming::FailActive(const std::string &error){
 error_=error;GenZHLoad::Record("material_fallback",error,0,0,false);
 auto found=entries_.find(active_);if(found!=entries_.end()){resident_-=found->second.bytes;found->second.views={};found->second.bytes=0;found->second.failed=true;found->second.queued=false;}
 upload_.reset();worker_.Finish();active_.clear();
}
HRESULT MaterialStreaming::Pump(ID3D11Device *device,ID3D11DeviceContext *context,bool allowStart,std::size_t bytes,double milliseconds){
 if(!device||!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!bytes||milliseconds<=0)return E_INVALIDARG;
 if(active_.empty()){
  if(!allowStart||queue_.empty()||!worker_.Available())return S_OK;
  const auto key=queue_.front();auto found=entries_.find(key);if(found==entries_.end()){queue_.pop_front();return S_OK;}
  if(worker_.Submit(found->second.request)){active_=key;queue_.pop_front();}return S_OK;
 }
 auto found=entries_.find(active_);if(found==entries_.end()){Reset();return S_OK;}auto &entry=found->second;
 if(!upload_){
  if(worker_.Available()){if(allowStart)worker_.Submit(entry.request);return S_OK;}
  auto result=worker_.Take();if(!result)return S_OK;
  if(!result->error.empty()){FailActive(result->error);return S_OK;}
  if(result->finished){entry.ready=true;entry.queued=false;GenZHLoad::Record("material_ready",entry.request.stem.string(),0,entry.bytes,false);worker_.Finish();active_.clear();Trim();return S_OK;}
  entry.request.expected=std::move(result->signature);entry.request.hasExpected=true;component_=result->component;
  upload_=std::make_unique<MaterialUpload>(std::move(result));
  if(entry.bytes+upload_->ResidentBytes()>cacheBudget_){FailActive("single material exceeds authored GPU cache budget");return S_OK;}
 }
 GenZHLoad::Scope timing("material_upload_slice","shared 4 MiB / 2 ms submission budget");const auto before=upload_->Uploaded();
 const auto hr=upload_->Pump(device,context,bytes,milliseconds);timing.bytes=upload_->Uploaded()-before;submitted_+=timing.bytes;
 if(FAILED(hr)){std::ostringstream detail;detail<<"authored GPU upload failed: stem="<<entry.request.stem.string()<<" component="<<component_<<" HRESULT=0x"<<std::hex<<static_cast<unsigned long>(hr)<<"; retaining native material";FailActive(detail.str());return S_OK;}
 if(upload_->Ready()){
  entry.views.maps[component_]=upload_->View();entry.bytes+=upload_->ResidentBytes();resident_+=upload_->ResidentBytes();entry.request.next=component_+1;
  upload_.reset();worker_.Finish();Trim();
 }
 return S_OK;
}
void MaterialStreaming::Reset(){worker_.Cancel();upload_.reset();queue_.clear();entries_.clear();++epoch_;active_.clear();resident_=0;error_.clear();}
}
