// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <guiddef.h>
#include "TerrainSurface.h"
#include "ShroudMapping.h"
#include "NativeMeshSubmit.h"
#include <atomic>
#include <vector>
// Buffer locks hand out the CPU mirror and stream it to the native buffer at
// Unlock (perf83). GENZH_MIRRORED_LOCKS=off restores the native-pointer path;
// the renderer refreshes this once per frame.
inline std::atomic<bool> mirroredBufferLocks{true};
// perf87 CopyRects repeat detection: 0 = off (every copy rebuilds the texture),
// 1 = on (content-identical repeats keep the texture revision), 2 = verify
// (repeats still rebuild and the rebuilt pixels are compared with the previous
// upload). GENZH_COPY_RECTS_REPEAT=off / GENZH_COPY_RECTS_VERIFY=on, per frame.
inline std::atomic<int> copyRectsRepeatMode{1};
// perf88: buffered and native mesh draws build their packet in the scene
// queue's next slot (GENZH_STAGED_PACKETS=off builds a local packet and moves it).
inline std::atomic<bool> stagedScenePackets{true};
extern const GUID D3D11TextureNameGUID;
extern const GUID D3D11TerrainAtlasGUID;
extern const GUID D3D11TerrainCellsGUID;
extern const GUID D3D11TerrainElevationGUID;
extern const GUID D3D11ShroudMappingGUID;
struct IDirect3DDevice8;
struct IDirect3DBaseTexture8;
void SetD3D11TextureName(IDirect3DBaseTexture8 *texture,const char *name);
// perf92: tags the shroud projection texture (ShroudMapping.h). Called by
// W3DShroud::render every frame; only a changed mapping writes private data.
void SetD3D11ShroudMapping(IDirect3DBaseTexture8 *texture,const D3D11ShroudMapping &mapping);
std::uint64_t GetD3D11ShroudRevision();
// perf93: per-cell vision of the local player (0 visible, 128 explored but not
// visible, 255 never explored) in shroud cell order, plus the map border state.
// Zero Hour writes explored cells with ClearAlpha (FogAlpha = ClearAlpha), so the
// shroud texture alone cannot place fog over ground that is out of sight.
struct D3D11ShroudVisibility {std::vector<std::uint8_t> cells;std::uint32_t cellsX=0,cellsY=0;std::uint8_t border=255;std::uint64_t revision=0;};
void SetD3D11ShroudVisibility(const std::uint8_t *cells,std::uint32_t cellsX,std::uint32_t cellsY,std::uint8_t border);
const D3D11ShroudVisibility &GetD3D11ShroudVisibility();
void SetD3D11TerrainAtlas(IDirect3DBaseTexture8 *texture,const D3D11TerrainAtlas &atlas);
void SetD3D11TerrainCells(IDirect3DBaseTexture8 *texture,const D3D11TerrainCell *cells,std::uint32_t count);
// terrain97: vertex heights ((cellsX+1)*(cellsY+1)) and per-cell triangle split flags.
void SetD3D11TerrainElevation(IDirect3DBaseTexture8 *texture,std::uint32_t cellsX,std::uint32_t cellsY,const float *heights,const std::uint8_t *flips);
void SetD3D11GroundBlocks(const std::vector<D3D11GroundBlock> &blocks);
const std::vector<D3D11GroundBlock> &GetD3D11GroundBlocks();
std::uint64_t GetD3D11GroundBlocksRevision();
// Client-thread render metadata only. Never changes native WeatherSetting/RNG.
struct D3D11MapWeather {std::uint64_t epoch=1;bool snowEnabled=false;};
void ResetD3D11MapWeather();
void SetD3D11NativeSnow(bool enabled);
const D3D11MapWeather &GetD3D11MapWeather();
// water107: the game's water pixel programs (W3DWater) and its soft-edge settings
// (TheWaterTransparency, GlobalData::m_showSoftWaterEdge). Client thread.
struct D3D11WaterSurface {std::uint32_t programs[3]{};float transparentDepth=0,minOpacity=1;bool additive=false,softEdge=false;};
void SetD3D11WaterPrograms(std::uint32_t trapezoid,std::uint32_t river,std::uint32_t sea);
void SetD3D11WaterTransparency(float transparentDepth,float minOpacity,bool additive,bool softEdge);
const D3D11WaterSurface &GetD3D11WaterSurface();
bool IsD3D11WaterProgram(std::uint32_t program);
void RegisterD3D11GroundOverlayTexture(const char *name);
bool IsD3D11GroundOverlayTexture(const char *name);
std::uint64_t GetD3D11GroundOverlayRevision();
void D3D11Diagnostic(const char *message);
// Returns false if the renderer was not requested. A requested connection
// either replaces the device slot successfully or throws a diagnostic; it
// never silently labels a native DX8 frame as a D3D11 frame.
bool IsD3D11RendererRequested();
void ConnectD3D11Device(IDirect3DDevice8 **device,void *window);
