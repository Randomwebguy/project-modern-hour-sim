// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainPreparation.h"
#include <d3d11.h>
#include <wrl/client.h>
namespace GenZH::Modern {
class TerrainUpload final {
 std::unique_ptr<PreparedTerrain> data_;
 std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,5> textures_;
 std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,5> views_;
 Microsoft::WRL::ComPtr<ID3D11Buffer> cellsBuffer_;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cellsView_;
 Microsoft::WRL::ComPtr<ID3D11Buffer> classesBuffer_;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> classesView_;
 unsigned surface_=0,row_=0;std::size_t cellOffset_=0,uploaded_=0,total_=0;
 enum class Stage{CreateSurface,Rows,Mips,CreateCells,CellBytes,Done};Stage stage_=Stage::CreateSurface;
public:
 explicit TerrainUpload(std::unique_ptr<PreparedTerrain> data):data_(std::move(data)){if(data_)total_=data_->Bytes();}
 HRESULT Pump(ID3D11Device *device,ID3D11DeviceContext *context,std::size_t byteBudget=4*1024*1024,double milliseconds=2);
 bool Ready()const{return stage_==Stage::Done;}
 std::size_t Uploaded()const{return uploaded_;}std::size_t RemainingCPU()const{return data_?data_->Bytes():0;}
 float Progress()const{return Ready()?1.0f:total_?static_cast<float>(uploaded_)/static_cast<float>(total_)*.99f:0;}
 const auto &Views()const{return views_;}const auto &Cells()const{return cellsView_;}const auto &Classes()const{return classesView_;}
 const PreparedTerrain &Data()const{return *data_;}
};
}
