// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
namespace GenZH::Modern {
// Sparse asynchronous work counters; no waiting, flushing or query allocation
// in the frame loop. Unavailable/busy counters never delay rendering.
class GpuPipelineStats {
 struct Slot{Microsoft::WRL::ComPtr<ID3D11Query> query;bool pending=false;};
 std::array<Slot,4> slots_;int active_=-1;std::uint64_t frame_=0;bool ready_=false;
public:
 void Reset(){for(auto &slot:slots_)slot=Slot{};active_=-1;frame_=0;ready_=false;}
 void Initialize(ID3D11Device *device){Reset();if(!device)return;D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS,0};for(auto &slot:slots_)if(FAILED(device->CreateQuery(&desc,slot.query.GetAddressOf())))return;ready_=true;}
 template<class Receive>void Poll(ID3D11DeviceContext *context,Receive receive){
  if(!ready_)return;for(auto &slot:slots_)if(slot.pending){D3D11_QUERY_DATA_PIPELINE_STATISTICS data{};const auto hr=context->GetData(slot.query.Get(),&data,sizeof(data),D3D11_ASYNC_GETDATA_DONOTFLUSH);if(hr==S_FALSE)continue;slot.pending=false;if(hr==S_OK)receive(data);}
 }
 void Begin(ID3D11DeviceContext *context){if(!ready_||active_>=0||frame_++%32)return;for(unsigned i=0;i<slots_.size();++i)if(!slots_[i].pending){active_=static_cast<int>(i);context->Begin(slots_[i].query.Get());return;}}
 void End(ID3D11DeviceContext *context){if(active_<0)return;auto &slot=slots_[static_cast<unsigned>(active_)];context->End(slot.query.Get());slot.pending=true;active_=-1;}
};
}
