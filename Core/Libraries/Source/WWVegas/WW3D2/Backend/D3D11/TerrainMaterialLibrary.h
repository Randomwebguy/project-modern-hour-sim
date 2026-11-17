// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NativeImage.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <istream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace GenZH::Modern {
// terrain96: world-space photo-scanned ground materials (ambientCG CC0 sets,
// built by work/terrain96/build_library.py). Each native ground class
// (D3D11Ground) names a primary layer and a macro partner layer; the shader
// tiles them in world space and keeps the map's own colour as a tint, so map
// authors' grass/sand/snow hues and blend painting survive. Unknown ground
// keeps the native atlas.
struct TerrainLibraryLayer {std::string id;float tile=0;std::array<float,3> mean{};};
// terrain97: macro relief from the primary layer's scanned height, resampled at
// macroTile world units (5-25 unit bumps the RTS camera can resolve); amplitude
// is peak-to-peak in world units; tessellation 0..1 raises the vertex density.
struct TerrainLibraryClass {std::uint32_t primary=0,partner=0;float tint=0;bool enabled=false;float relief=0,macroTile=0,tessellation=0;};
struct TerrainLibraryManifest {
    std::uint32_t size=0;
    std::vector<TerrainLibraryLayer> layers;
    std::array<TerrainLibraryClass,16> classes{};
};
constexpr std::uint32_t TerrainLibraryMaxLayers=16;
// manifest.txt: "terrain-library 1", "size N", "layer i id tile r g b", "class g a b tint",
// optional "relief g amplitude macroTile tessellation" (terrain97).
bool ParseTerrainLibraryManifest(std::istream &in,TerrainLibraryManifest &out,std::string &error);
// Shader table (float4[64]): [i]=(tile,mean linear rgb) per layer, [16+g]=(primary,partner,tint,enabled),
// [48+g]=(relief amplitude,macro tile,tessellation weight,0).
constexpr std::uint32_t TerrainLibraryTableEntries=64;
std::array<float,TerrainLibraryTableEntries*4> TerrainLibraryTable(const TerrainLibraryManifest &manifest);

class TerrainMaterialLibrary final {
public:
    static constexpr UINT AlbedoSlot=48,SurfaceSlot=49,TableSlot=50,SamplerSlot=14;
    TerrainMaterialLibrary()=default;
    TerrainMaterialLibrary(const TerrainMaterialLibrary&)=delete;
    TerrainMaterialLibrary &operator=(const TerrainMaterialLibrary&)=delete;
    ~TerrainMaterialLibrary(){Stop();}
    // Starts (or restarts, when the folder changed) the background decode.
    void Start(const std::filesystem::path &folder);
    // Uploads decoded layers within the byte budget (at least one slice pair).
    HRESULT Pump(ID3D11Device *device,ID3D11DeviceContext *context,std::size_t byteBudget=4*1024*1024);
    bool Ready()const{return ready_;}
    bool Pending()const{return started_&&!ready_&&!failed_;}
    bool Failed()const{return failed_;}
    std::uint64_t Revision()const{return revision_;}
    std::size_t Uploaded()const{return uploaded_;}
    const TerrainLibraryManifest &Manifest()const{return manifest_;}
    bool HasRelief()const{for(const auto &c:manifest_.classes)if(c.enabled&&c.relief>0)return true;return false;}
    // Pixel stage for shading; hull/domain stages for macro relief (also in shadow passes).
    void Bind(ID3D11DeviceContext *context,bool pixel=true)const;
    // terrain97: GPU grass roots evaluate the same macro relief in a compute pass.
    void BindCompute(ID3D11DeviceContext *context)const;
    // Device loss or shutdown: cancels the worker and drops GPU resources.
    void Stop();
private:
    struct Decoded {std::uint32_t layer=0;ImageRGBA albedo,surface;};
    void Run(std::filesystem::path folder);
    HRESULT Create(ID3D11Device *device);
    void Fail(const std::string &why);
    std::filesystem::path folder_;
    std::thread worker_;
    std::mutex mutex_;std::condition_variable wake_;
    std::deque<Decoded> decoded_;
    bool manifestReady_=false,workerFailed_=false;std::string workerError_;
    std::atomic<bool> cancel_{false};
    TerrainLibraryManifest manifest_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> albedo_,surface_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> albedoView_,surfaceView_,tableView_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> table_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    std::uint32_t received_=0;std::size_t uploaded_=0;std::uint64_t revision_=0;
    double startedAt_=0;
    bool started_=false,created_=false,ready_=false,failed_=false;
};
}
