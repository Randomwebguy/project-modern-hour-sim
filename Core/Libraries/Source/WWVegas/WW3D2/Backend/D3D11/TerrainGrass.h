// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainMaterialLibrary.h"
#include "GpuPassTimer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GenZH::Modern {
struct LegacyConstants;
// terrain97: GPU-driven grass (TerrainGrass.hlsl). Once per frame, after the
// first opaque modern terrain draw: GrassCS places and culls blades over the
// visible ground into an append buffer, then one DrawInstancedIndirect expands
// them with GrassVS through the terrain program's own LegacyPS (full lighting,
// shadows, IBL, fog). Technique adapted from EricHu33/UnityGrassIndirectRenderingExample (MIT).
struct TerrainGrassInputs {
    ID3D11ShaderResourceView *height=nullptr,*identity=nullptr,*cells=nullptr,*exclusion=nullptr;
    ID3D11ShaderResourceView *classes=nullptr; // terrain101: per-cell class records (ground field)
    ID3D11ShaderResourceView *elevation=nullptr,*flips=nullptr,*atlas=nullptr;
    ID3D11SamplerState *terrainSampler=nullptr;
    const TerrainMaterialLibrary *library=nullptr;
    std::array<float,4> dimensions{}; // cellsX, cellsY, border world, cell size
    float minHeight=0,maxHeight=0;    // native elevation range (world units)
};
struct TerrainGrassSettings {
    float cellSize=.45f,maxDistance=520,fullDensityDistance=140;
    float bladeHeight=2.0f,bladeWidth=.16f,pixelWidth=.9f;
    float windX=.8f,windY=.6f,sway=.18f,gust=.06f;
    std::uint32_t capacity=400000;
    // terrain98: vehicle interaction map (toroidal, around the visible ground).
    float interactionCell=1.0f,recoverySeconds=6.0f;std::uint32_t interactionSize=512;
};
struct TerrainGrassStats {std::uint64_t frames=0,sites=0,failures=0;std::uint32_t lastGridX=0,lastGridY=0;std::string lastError;};
class TerrainGrassGPU final {
public:
    static constexpr UINT BladeSlot=53,ElevationSlot=51,FlipSlot=52,AtlasSlot=54,ConstantSlot=9,InteractionSlot=55,InteractionConstantSlot=10,ClassesSlot=56;
    static constexpr std::uint32_t MaxVehicles=64;
    // terrain98: ground vehicles and low aircraft this frame (xyz position, w radius).
    void SetVehicles(const std::vector<std::array<float,4>> &vehicles){vehicles_=vehicles;if(vehicles_.size()>MaxVehicles)vehicles_.resize(MaxVehicles);}
    // terrain98: the grass placement/interaction constants and interaction map for
    // the terrain pixel shader (analytic grass shadows on the ground).
    void BindPixel(ID3D11DeviceContext *context)const;
    bool HasBlades()const{return haveBlades_;}
    // Offline checks: the interaction texel at a world position (push xy, strength, key).
    HRESULT ReadInteraction(ID3D11Device *device,ID3D11DeviceContext *context,float x,float y,std::array<float,4> &value);
    // pixel: the terrain program's full LegacyPS (LegacyOutput input signature).
    HRESULT Draw(ID3D11Device *device,ID3D11DeviceContext *context,const LegacyConstants &constants,const TerrainGrassInputs &inputs,
                 ID3D11PixelShader *pixel,float seconds);
    void Reset();
    // Offline checks: the blades appended by the last Draw (root xyz, colour, facing, height, width, normal).
    HRESULT ReadBlades(ID3D11Device *device,ID3D11DeviceContext *context,std::vector<std::array<float,8>> &blades);
    TerrainGrassSettings settings;
    // perf116: optional GPU phase timer; the blade draw is marked as GrassDraw (the placement stays in Grass).
    GenZH::Modern::GpuPassTimer *passTimer=nullptr;
    const TerrainGrassStats &Stats()const{return stats_;}
    bool Failed()const{return failed_;}
    // World-space grid covering the visible ground (exposed for the offline check).
    static bool VisibleGrid(const LegacyConstants &constants,const TerrainGrassInputs &inputs,const TerrainGrassSettings &settings,
                            std::array<int,2> &first,std::array<std::uint32_t,2> &count);
private:
    HRESULT Create(ID3D11Device *device);
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> compute_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> blades_,arguments_,legacy_,grass_,map_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> bladesUAV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bladesSRV_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> interactCompute_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> interaction_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> interactionUAV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> interactionSRV_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> interactionConstants_;
    std::vector<std::array<float,4>> vehicles_;
    float lastSeconds_=-1,windowCenter_[2]{};
    std::uint32_t capacity_=0;bool failed_=false,haveBlades_=false;
    TerrainGrassStats stats_;
};
}
