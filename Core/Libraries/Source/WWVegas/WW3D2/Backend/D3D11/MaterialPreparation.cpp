// SPDX-License-Identifier: GPL-3.0-or-later
#include "MaterialPreparation.h"
#include <sstream>
namespace GenZH::Modern {
namespace {
const wchar_t *suffixes[]={L"_albedo.tga",L"_normal.tga",L"_orm.tga",L"_height.tga",L"_emissive.tga",L"_wear_albedo.tga",L"_wear_normal.tga",L"_wear_orm.tga"};
std::filesystem::path Source(const MaterialRequest &r,unsigned i){return r.root/(r.stem.wstring()+suffixes[i]);}
std::filesystem::path DDSPath(const std::filesystem::path &source){return source.parent_path()/L"runtime-dds"/std::filesystem::path(source.filename()).replace_extension(L".dds");}
std::string Stamp(const std::filesystem::path &path){
 WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a)||(a.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return "missing";
 ULARGE_INTEGER size{},time{};size.LowPart=a.nFileSizeLow;size.HighPart=a.nFileSizeHigh;time.LowPart=a.ftLastWriteTime.dwLowDateTime;time.HighPart=a.ftLastWriteTime.dwHighDateTime;
 return std::to_string(size.QuadPart)+":"+std::to_string(time.QuadPart);
}
}
MaterialSignature GetMaterialSignature(const MaterialRequest &r){MaterialSignature result;
 for(unsigned i=0;i<8;++i){const auto source=Source(r,i),dds=DDSPath(source);result[i]=Stamp(source)+"|"+Stamp(dds)+"|"+Stamp(std::filesystem::path(dds).replace_extension(L".source"));}return result;
}
bool PrepareMaterial(MaterialRequest r,PreparedMaterial &out,const std::atomic<bool> &cancel){
 out={};GenZHLoad::Scope timing("material_prepare_worker",r.stem.string());
 auto fail=[&](const char *why){out.error=why;return false;};
 if(r.root.empty()||r.stem.empty()||r.stem.has_parent_path()||r.stem==L"."||r.stem==L".."||r.next>8)return fail("invalid authored material request");
 if(cancel)return false;out.signature=GetMaterialSignature(r);
 if(r.hasExpected&&out.signature!=r.expected)return fail("authored material edited during streaming");
 unsigned index=r.next;for(;index<8&&out.signature[index].rfind("missing|",0)==0;++index){}
 if(index==8){out.finished=true;return !cancel;}
 out.component=index;out.source=Source(r,index);const auto dds=DDSPath(out.source);
 if(IsPreparedCurrent(out.source,dds)){
  GenZHLoad::Scope ddsTiming("material_read_dds",out.source.filename().string());
  std::ifstream file(dds,std::ios::binary|std::ios::ate);const auto size=file?file.tellg():std::streampos(-1);
  if(size>=148&&size<=96*1024*1024){std::vector<unsigned char> bytes(static_cast<std::size_t>(size));file.seekg(0);bool read=true;
   for(std::size_t at=0;at<bytes.size();){if(cancel)return false;const auto count=std::min(bytes.size()-at,std::size_t(1024*1024));if(!file.read(reinterpret_cast<char*>(bytes.data()+at),static_cast<std::streamsize>(count))){read=false;break;}at+=count;}
   std::string error;if(read&&DecodePreparedDDS(std::move(bytes),out.dds,error)){out.fromDDS=true;ddsTiming.hit=true;ddsTiming.bytes=out.CPUBytes();}
   else if(read)GenZHLoad::Record("material_dds_rejected",out.source.filename().string()+": "+error,0);
  }
 }
 if(!out.fromDDS){
  if(cancel)return false;GenZHLoad::Scope fallback("material_read_tga_fallback",out.source.filename().string());
  if(!LoadTGA(out.source,out.rgba,out.error))return false;fallback.bytes=out.CPUBytes();
 }
 if(cancel)return false;
 if(out.signature!=GetMaterialSignature(r)){out.dds={};out.rgba={};return fail("authored source changed while reading");}
 timing.hit=out.fromDDS;timing.bytes=out.CPUBytes();return true;
}
void MaterialPreparation::Run(){for(;;){std::unique_ptr<MaterialRequest> request;
 {std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return stop_||request_!=nullptr;});if(stop_)return;request=std::move(request_);running_=true;}
 auto result=std::make_unique<PreparedMaterial>();try{PrepareMaterial(std::move(*request),*result,cancel_);}catch(const std::exception &e){result->error=e.what();}catch(...){result->error="material preparation exception";}request.reset();
 {std::lock_guard<std::mutex> lock(mutex_);running_=false;if(cancel_||stop_){result.reset();busy_=false;}else ready_=std::move(result);}
}}
MaterialPreparation::~MaterialPreparation(){Stop();}
bool MaterialPreparation::Available(){std::lock_guard<std::mutex> lock(mutex_);return !busy_&&!stop_;}
bool MaterialPreparation::Submit(MaterialRequest request){std::lock_guard<std::mutex> lock(mutex_);if(busy_||stop_)return false;
 try{request_=std::make_unique<MaterialRequest>(std::move(request));if(!worker_.joinable())worker_=std::thread([this]{Run();});}catch(...){request_.reset();return false;}
 busy_=true;cancel_=false;wake_.notify_one();return true;
}
std::unique_ptr<PreparedMaterial> MaterialPreparation::Take(){std::lock_guard<std::mutex> lock(mutex_);return std::move(ready_);}
void MaterialPreparation::Finish(){std::lock_guard<std::mutex> lock(mutex_);if(!running_){ready_.reset();busy_=false;}}
void MaterialPreparation::Cancel(){std::lock_guard<std::mutex> lock(mutex_);cancel_=true;if(!running_){ready_.reset();request_.reset();busy_=false;}wake_.notify_one();}
void MaterialPreparation::Stop(){{std::lock_guard<std::mutex> lock(mutex_);stop_=true;cancel_=true;request_.reset();ready_.reset();wake_.notify_one();}if(worker_.joinable())worker_.join();}
}
