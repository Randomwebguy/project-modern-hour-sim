// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeDDS.h"
#include "NativeImage.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace GenZH::Modern {
using MaterialSignature=std::array<std::string,8>;
struct MaterialRequest {
 std::filesystem::path root,stem;unsigned next=0;
 MaterialSignature expected{};bool hasExpected=false;
};
struct PreparedMaterial {
 MaterialSignature signature{};unsigned component=0;bool finished=false,fromDDS=false;
 std::filesystem::path source;std::string error;PreparedDDS dds;ImageRGBA rgba;
 std::size_t CPUBytes()const{return dds.data.size()+rgba.pixels.size();}
};
MaterialSignature GetMaterialSignature(const MaterialRequest &request);
bool PrepareMaterial(MaterialRequest request,PreparedMaterial &result,const std::atomic<bool> &cancel);
// One CPU map at a time; capacity stays reserved until its upload finishes.
class MaterialPreparation final {
 std::thread worker_;std::mutex mutex_;std::condition_variable wake_;
 std::unique_ptr<MaterialRequest> request_;std::unique_ptr<PreparedMaterial> ready_;
 std::atomic<bool> cancel_{false};bool busy_=false,running_=false,stop_=false;
 void Run();
public:
 ~MaterialPreparation();bool Available();bool Submit(MaterialRequest request);
 std::unique_ptr<PreparedMaterial> Take();void Finish();void Cancel();void Stop();
};
}
