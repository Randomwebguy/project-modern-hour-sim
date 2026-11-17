// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "StateSnapshot.h"
#include "RenderDevice.h"
#include "AtmosphereGPU.h"
#include "ShadowCascades.h"
#include "ShadowCascadeGPU.h"
#include "LocalShadowGPU.h"
#include "LocalShadowBindings.h"
#include "LocalSurfaceLightingGPU.h"
#include "NativeLightCalibrationGPU.h"
#include "ShadowReuseCache.h"
#include "LegacyDraw.h"
#include "GeometryCache.h"
#include "GpuFrameTimer.h"
#include "GpuPassTimer.h"
#include "GpuPipelineStats.h"
#include "ConstantUploadCache.h"
#include "TerrainUpload.h"
#include "TerrainMaterialLibrary.h"
#include "TerrainGrass.h"
#include "MaterialStreaming.h"
#include "SceneGroup.h"
#include "PresentedFrameTelemetry.h"
#include "TerrainExclusionInputs.h"
#include "NativeSceneIdentity.h"
#include "NativeFixedState.h"
#include "DerivedFixedState.h"
#include "PagedSceneQueue.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <functional>
#include "BinaryStateKey.h"
#include "SceneConstantArena.h"
#include "ScenePipelineBindings.h"
#include "ResolvedPipelineStates.h"
#include "RuntimeSwitches.h"
#include "ByteEquality.h"
#include "VertexShaderConstants.h"
#include "ShroudMapping.h"

namespace GenZH::Modern {
struct SortedColorPalette;
struct BufferData {
    std::vector<std::uint8_t> bytes;
    DWORD fvf=0,usage=0;D3DFORMAT format=D3DFMT_INDEX16;
    std::uint64_t revision=1;
    ComPtr<IUnknown> native;
};
struct TextureData {
    ComPtr<IDirect3DBaseTexture8> native;
    std::uint64_t revision=1;
    // perf87 CopyRects verify mode: an upload at expectedRevision must reproduce
    // the pixels uploaded at baseRevision (a skipped repeat would reuse them).
    bool copyVerified=false;std::uint64_t expectedRevision=0,baseRevision=0;
    // perf92: shroud mapping private data, re-read when GetD3D11ShroudRevision changes.
    std::uint64_t shroudRevision=0;bool shroud=false;D3D11ShroudMapping shroudMapping{};
};
struct CopyRectsStats {std::uint64_t copies=0,tracked=0,repeats=0,verified=0,mismatches=0;};
struct LegacyConstants {
    float world[16],view[16],projection[16],normalWorld[16];
    float textureMatrices[8][16];
    float materialDiffuse[4],materialAmbient[4],materialSpecular[4],materialEmissive[4];
    float globalAmbient[4],textureFactor[4],fogColor[4],fogSettings[4];
    float viewport[4],geometryFlags[4],materialFlags[4],pointSettings[4];
    float lightDiffuse[4][4],lightAmbient[4][4],lightSpecular[4][4];
    float lightPosition[4][4],lightDirection[4][4],lightAttenuation[4][4],lightSpot[4][4];
    UINT colorStage[8][4],alphaStage[8][4],coordinateStage[8][4];
    float bump[8][4],bumpLuma[8][4],pixelConstants[8][4];
    float bridgeFlags[4],pointScale[4],modernMaterial[4],cameraWorld[4];
    float vehicleAge[4]{},vehicleProfile[4]{};
    // photo107: per-object motion (see LegacyData): previous world relative to the current one (inverse(current) * previous), camera previous and current unjittered view-projection.
    float motionPreviousWorld[16]{},motionCurrentVP[16]{},motionPreviousVP[16]{},motionSettings[4]{};
};
static_assert(sizeof(LegacyConstants)==2480);
// perf111: shader disassembly is shared and immutable, so copying a packet does not copy the text.
inline const std::string &AssemblyText(const std::shared_ptr<const std::string> &text){static const std::string empty;return text?*text:empty;}
struct DrawPacket {
    NativeMeshSource nativeSource{};
    LegacyConstants constants{};
    std::vector<LegacyVertex> vertices;
    std::vector<FixedVertex> fixedVertices;
    std::vector<std::uint32_t> indices;
    std::shared_ptr<DecodedGeometry> geometry;
    GeometryKey sourceIdentity{};
    std::shared_ptr<void> sourceVertex,sourceIndex;
    bool CompactVertices()const{return geometry?geometry->CompactVertices():!fixedVertices.empty();}
    std::size_t VertexCount()const{return geometry?geometry->VertexCount():(fixedVertices.empty()?vertices.size():fixedVertices.size());}
    const float *Position(std::size_t i)const{return geometry?geometry->Position(i):(fixedVertices.empty()?vertices[i].position:fixedVertices[i].position);}
    void CompactDynamic(){
        if(geometry||vertexShader||vertices.empty())return;
        fixedVertices.resize(vertices.size());
        for(std::size_t i=0;i<vertices.size();++i)std::memcpy(&fixedVertices[i],&vertices[i],sizeof(FixedVertex));
        std::vector<LegacyVertex>().swap(vertices);
    }
    const std::vector<std::uint32_t> &Indices()const{return geometry?geometry->indices:indices;}
    std::array<std::shared_ptr<TextureData>,8> textures;
    // Deferred UI must retain the resource revision used when it was submitted.
    // Font atlases can be rewritten again before EndScene.
    std::array<ComPtr<ID3D11ShaderResourceView>,8> capturedTextures;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> materialTextures;
    ComPtr<ID3D11ShaderResourceView> terrainHeight,terrainClass,terrainCells,terrainClasses;
    std::shared_ptr<void> terrainShadowOwner; // CPU lifetime token for immutable uploaded terrain maps.
    float terrainDimensions[4]{};
    WeatherClimate terrainClimate;
    bool terrain=false,groundOverlay=false,groundExclusionOverlay=false;
    ComPtr<ID3D11ShaderResourceView> heightTexture,emissiveTexture;
    std::array<ComPtr<ID3D11ShaderResourceView>,3> wearTextures;
    // perf112: shared immutable state (StateSnapshot.h). States() reads it; MutableStates() clones it before a write.
    std::shared_ptr<const StateSnapshot> stateSnapshot;
    const StateSnapshot &States()const{static const StateSnapshot zeros{};return stateSnapshot?*stateSnapshot:zeros;}
    StateSnapshot &MutableStates(){if(!stateSnapshot||stateSnapshot.use_count()>1)stateSnapshot=std::make_shared<StateSnapshot>(States());return const_cast<StateSnapshot&>(*stateSnapshot);}
    // perf89: nonzero when both arrays are the device state mirror's bytes at
    // this mirror revision (NativeStateMirror). Code that edits either array
    // after capture must reset it to 0.
    std::uint64_t stateRevision=0;
    float waterShore[4]{}; // water107: transparent depth, min opacity, foam (flag 2097152)
    D3DPRIMITIVETYPE primitive=D3DPT_TRIANGLELIST;
    D3DPRIMITIVETYPE sourcePrimitive=D3DPT_TRIANGLELIST;
    DWORD fvf=0,pixelShader=0;
    std::shared_ptr<const std::string> pixelAssembly,vertexAssembly;
    DWORD vertexShader=0;
    VertexShaderConstants vertexConstants; // 96 float4 registers, read when vertexShader is set
    bool outputOverlay=false;
};
// perf89: GENZH_STATE_REVISION=off always compares the bytes.
inline std::atomic<bool> stateRevisionCompare{true};
// perf88: equality of the captured render and texture-stage state arrays as one
// byte comparison each (DWORD values, no padding: identical to std::array ==).
// perf89: packets captured at the same mirror revision hold the same bytes.
inline bool SameStateArrays(const DrawPacket &a,const DrawPacket &b){
    if(a.stateRevision&&a.stateRevision==b.stateRevision&&stateRevisionCompare.load(std::memory_order_relaxed))return true;
    // perf112: one shared snapshot is one set of bytes.
    if(a.stateSnapshot&&a.stateSnapshot==b.stateSnapshot)return true;
    return BytesEqual(a.States().render.data(),b.States().render.data(),sizeof(StateSnapshot::render))&&
           BytesEqual(a.States().stages.data(),b.States().stages.data(),sizeof(StateSnapshot::stages));
}
static_assert(sizeof(StateSnapshot::render)==256*sizeof(DWORD)&&sizeof(StateSnapshot::stages)==8*32*sizeof(DWORD));

// Device interception owns the old asset-loading interfaces; all submitted
// geometry, state and presentation in this class use the modern device.
class CompatibilityRenderer final {
    friend struct TerrainExclusionCheckAccess;
public:
    ~CompatibilityRenderer(){try{if(stats_.frames)WritePerformanceStats();}catch(...){}terrainPreparation_.Stop();terrainLibrary_.Stop();materialStreaming_.Stop();}
    HRESULT Initialize(IDirect3DDevice8 *native,HWND window,UINT width,UINT height);
    void PrepareReset();
    HRESULT Reset(HWND window,UINT width,UINT height);
    HRESULT BeginScene();
    HRESULT EndScene();
    HRESULT Present();
    HRESULT Clear(DWORD flags,D3DCOLOR color,float depth,DWORD stencil);
    // texturesCaptured: the packet already carries the CaptureTextures result of an
    // identical capture key (sorted-run template, perf84).
    HRESULT Draw(DrawPacket &&packet,bool texturesCaptured=false);
    // perf88: a packet built in the scene queue's next slot is committed there by
    // Draw instead of being moved in. Callers discard it on every other path.
    DrawPacket &StagePacket(){return scenePackets_.stage();}
    void DiscardStagedPacket(){scenePackets_.discard();}
    HRESULT Snapshot(IDirect3DDevice8 *native,DrawPacket &packet,bool statesCaptured=false,const NativeFixedState *fixedState=nullptr);
    // Renderer inputs read by Snapshot/CaptureTextures/CaptureTerrain besides the
    // packet's states, fixed state, textures, source, FVF and primitive.
    std::array<std::uint64_t,8> CaptureContext()const;
    HRESULT CaptureTexturesForCheck(DrawPacket &packet){return CaptureTextures(packet);}
    std::size_t ScenePacketCount()const{return scenePackets_.size();}
    const DrawPacket &LastScenePacket()const{return scenePackets_.back();}
    void SetDerivedStateReuse(bool enabled){derivedFixed_.enabled=enabled;}
    bool Offscreen()const{return offscreen_;}
    HRESULT SetTarget(IDirect3DSurface8 *color,IDirect3DSurface8 *depth);
    HRESULT SynchronizeSurface(IDirect3DSurface8 *surface);
    HRESULT SurfaceChanged(IDirect3DSurface8 *surface);
    HRESULT ReadFrontBuffer(IDirect3DSurface8 *surface);
    void ForgetShaders();
    void TextureChanged(const std::shared_ptr<TextureData> &texture);
    // A destination the renderer reads back or renders into is never a CopyRects repeat candidate.
    bool TracksSurface(IDirect3DSurface8 *surface)const{return surface==nativeDefault_.Get()||surfaces_.count(surface)!=0;}
    CopyRectsStats &CopyStats(){return copyRectsStats_;}
    const std::string &LastError() const { return error_; }
    void GeometryCacheResult(bool hit,UINT vertices,UINT indices){
        if(hit)++stats_.cacheHits;else{++stats_.cacheMisses;stats_.decodedVertices+=vertices;stats_.decodedIndices+=indices;}
    }
    void GeometryCacheState(std::size_t bytes,std::size_t entries,std::uint64_t evictions){stats_.cacheBytes=bytes;stats_.cacheEntries=entries;stats_.cacheEvictions=evictions;}
    RenderDevice &Renderer() { return renderer_; }
private:
    struct Program {
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11VertexShader> shadowVertex,opaqueShadowVertex;
        ComPtr<ID3D11PixelShader> shadowPixel;
        ComPtr<ID3D11GeometryShader> point;
        ComPtr<ID3D11HullShader> terrainHull;
        ComPtr<ID3D11GeometryShader> terrainGrass,terrainGrassCompact;
        bool terrainCompactColor=false,terrainCompactShadow=false;
        ComPtr<ID3D11PixelShader> terrainPixel,terrainShadowPixel;
        ComPtr<ID3D11DomainShader> terrainDomain;
        ComPtr<ID3D11InputLayout> layout;
    };
    struct GPUTexture {
        std::weak_ptr<TextureData> owner;
        std::uint64_t revision=0;
        ComPtr<ID3D11Resource> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11RenderTargetView> target;
        std::string materialName;MaterialLookup materialLookup;MaterialLookupMemo materialMemo;bool materialAudited=false;
        std::uint64_t materialRootRevision=0,overlayRevision=0;
        bool groundOverlay=false,groundExclusionOverlay=false,wearTexture=false;
        std::vector<std::uint8_t> verifyRGBA;std::uint64_t verifyRevision=0; // perf87 CopyRects verify mode only
    };
    struct SurfaceMapping {
        ComPtr<IDirect3DSurface8> native;
        std::shared_ptr<TextureData> owner;
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11DepthStencilView> depth;
        UINT subresource=0;
    };
    HRESULT MapSurface(IDirect3DSurface8 *native,SurfaceMapping *&mapping);
    HRESULT ReadSurface(ID3D11Resource *resource,UINT subresource,IDirect3DSurface8 *dest,bool linear);
    HRESULT Execute(const DrawPacket &packet,bool shadow=false,const DrawGroup *instances=nullptr,UINT shadowSize=2048,const SortedColorPalette *palette=nullptr,UINT sortedIndexCount=0);
    HRESULT FlushScene();
    HRESULT GetProgram(const DrawPacket &packet,Program *&program,bool requireShadow=false,bool sortedPalette=false);
    HRESULT GetTexture(const std::shared_ptr<TextureData> &texture,GPUTexture *&result);
    HRESULT CaptureTextures(DrawPacket &packet);
    HRESULT CaptureTerrain(DrawPacket &packet);
    HRESULT BuildTerrainExclusion();
    HRESULT BindStates(const DrawPacket &packet);
    HRESULT BindStatesOriginal(const DrawPacket &packet);
    HRESULT UploadGeometry(const DrawPacket &packet,bool shadow=false);
    LegacyConstants DrawConstants(const DrawPacket &packet,bool shadow);
    HRESULT Fail(HRESULT code,const std::string &message);
    DerivedFixedState derivedFixed_;
    RenderDevice renderer_;
    AtmosphereGPU atmosphere_;
    NativeLightCalibrationGPU calibratedLights_;
    std::chrono::steady_clock::time_point atmosphereTick_;
    double atmosphereDelta_=0;
    bool worldFrameDrawn_=false,frameHasWorldDepth_=false;
    bool screenTargetsBound_=false,primaryDepthTarget_=true;
    ComPtr<IDirect3DDevice8> native_;
    ComPtr<IDirect3DSurface8> nativeDefault_;
    ComPtr<ID3D11Buffer> vertexBuffer_,indexBuffer_,constantBuffer_,vertexConstants_,shadowConstants_,terrainConstants_;
    SceneConstantArena<LegacyConstants> sceneConstantArena_;
    UINT sceneConstantIndex_=UINT_MAX;
    ComPtr<ID3D11RasterizerState> shadowRaster_;
    ComPtr<ID3D11DepthStencilState> shadowState_;
    UINT vertexCapacity_=0,indexCapacity_=0;
    ComPtr<ID3D11Buffer> instanceBuffer_,instanceSettings_;
    ComPtr<ID3D11ShaderResourceView> instanceView_;
    UINT instanceCapacity_=0;
    HRESULT BindInstances(const DrawGroup *instances,UINT sortedBase=0);
    using ProgramKey=std::array<DWORD,12>;
    std::unordered_map<BinaryStateKey<ProgramKey>,Program,BinaryStateHash<ProgramKey>> programs_;
    std::unordered_map<DWORD,std::shared_ptr<const std::string>> pixelAssembly_,vertexAssembly_;
    ScenePipelineBindings sceneBindings_;bool sceneBindingsEnabled_=true;
    bool resolvedStatesEnabled_=true;
    bool sortedColorPaletteEnabled_=true;
    RuntimeSwitches runtimeSwitches_;
    ResolvedStateCache<4,ComPtr<ID3D11RasterizerState>> resolvedRaster_;
    ResolvedStateCache<7,ComPtr<ID3D11BlendState>> resolvedBlend_;
    ResolvedStateCache<11,ComPtr<ID3D11DepthStencilState>> resolvedDepth_;
    ResolvedStateCache<11,ComPtr<ID3D11SamplerState>> resolvedSampler_;
    BinaryStateTable<D3D11_RASTERIZER_DESC,ComPtr<ID3D11RasterizerState>> rasterStates_;
    BinaryStateTable<D3D11_BLEND_DESC,ComPtr<ID3D11BlendState>> blendStates_;
    BinaryStateTable<D3D11_DEPTH_STENCIL_DESC,ComPtr<ID3D11DepthStencilState>> depthStates_;
    BinaryStateTable<D3D11_SAMPLER_DESC,ComPtr<ID3D11SamplerState>> samplerStates_;
    std::unordered_map<IDirect3DBaseTexture8*,GPUTexture> textures_;
    MaterialStreaming materialStreaming_;
    std::filesystem::path materialRoot_;
    std::uint64_t materialRootRevision_=1;
    std::size_t queueHighWater_=0;
    bool materialCompatibility_=false,terrainEnabled_=true,foliageEnabled_=true,terrainLibraryEnabled_=true,terrainReliefEnabled_=true,grassEnabled_=true,grassDrawn_=false,grassShadowsEnabled_=true,grassInteractionEnabled_=true,terrainFieldEnabled_=true,terrainHexEnabled_=true,terrainWetEnabled_=true,accumulationEnabled_=true,terrainRocksEnabled_=true,decalEnabled_=true;UINT cascadeSize_=2048;float cascadeSplit_=.35f;
    // accum105: snow (524288) / dust (1048576) material bits for opaque world objects, from the
    // map climate (alpine ground or mission snow / arid ground); set at execute, read at capture.
    std::atomic<std::uint32_t> accumulationFlags_{0};bool waterShoreEnabled_=true,waterReflectionEnabled_=true;Microsoft::WRL::ComPtr<ID3D11Buffer> waterConstants_;
    std::uint64_t waterShoreDraws_=0,waterShoreFallbacks_=0;
    // photo107: per-object motion vectors. Each object (render root, mesh, pass) keeps last frame's
    // world matrix; a moving object binds its previous world and the camera's previous and current
    // unjittered view-projections (b15). Its offset goes to the motion target (aux 5) for TAA.
    struct MotionKey {std::uint64_t root,mesh;std::uint32_t pass;bool operator==(const MotionKey &o)const{return root==o.root&&mesh==o.mesh&&pass==o.pass;}};
    struct MotionKeyHash {std::size_t operator()(const MotionKey &k)const{return std::hash<std::uint64_t>()(k.root*1000003ull^k.mesh*2654435761ull^k.pass);}};
    std::unordered_map<MotionKey,std::array<float,16>,MotionKeyHash> motionPrevious_,motionCurrent_;
    float motionCurrentVP_[16]{},motionPreviousVP_[16]{};bool motionCurrentValid_=false,motionPreviousValid_=false,motionEnabled_=true;
    std::uint64_t motionDraws_=0;
    void FillMotion(LegacyConstants &constants,const DrawPacket &packet,bool shadow);std::uint32_t accumulationReported_=~0u;
    std::vector<std::array<float,4>> grassVehicles_;
    // terrain97: GPU-driven grass (TerrainGrass.h), GENZH_GRASS=off keeps the legacy blades.
    TerrainGrassGPU grass_;
    std::chrono::steady_clock::time_point grassEpoch_=std::chrono::steady_clock::now();
    HRESULT DrawGrass(const DrawPacket &packet,ID3D11PixelShader *pixel);
    // taa100: temporal AA (GENZH_TAA). Halton jitter of the primary camera's world draws.
    bool taaEnabled_=true,taaActive_=false;std::uint64_t taaFrame_=0;std::array<float,2> taaJitter_{};
    void ApplyTaaJitter(LegacyConstants &constants);
    // taa101: clip-space jitter for game vertex programs (b8): this frame's and zero.
    ComPtr<ID3D11Buffer> taaVertexJitter_,taaVertexZero_;std::uint64_t taaVertexDraws_=0;
    // terrain96: world-space photo-scanned ground layers (GENZH_TERRAIN_LIBRARY).
    TerrainMaterialLibrary terrainLibrary_;
    bool TerrainLibraryActive()const{return terrainLibraryEnabled_&&terrainLibrary_.Ready();}
    // terrain97: macro relief from the library heights (GENZH_TERRAIN_RELIEF).
    bool TerrainReliefActive()const{return TerrainLibraryActive()&&terrainReliefEnabled_&&terrainLibrary_.HasRelief();}
    void RefreshCaptureConfiguration();
    struct TerrainMaps {
        WeatherClimate climate;
        std::weak_ptr<TextureData> owner;
        std::uint64_t revision=0;
        std::uint64_t failedRevision=0;
        std::array<ComPtr<ID3D11ShaderResourceView>,5> views;
        ComPtr<ID3D11ShaderResourceView> cells,classes;
        std::shared_ptr<void> shadowOwner;
        // terrain97: native elevation for the GPU grass roots (game private data).
        ComPtr<ID3D11ShaderResourceView> elevation,flips;
        float minHeight=0,maxHeight=0;
    };
    std::unordered_map<IDirect3DBaseTexture8*,TerrainMaps> terrainMaps_;
    TerrainPreparation terrainPreparation_;
    std::unique_ptr<TerrainUpload> terrainUpload_;
    std::weak_ptr<TextureData> terrainPendingOwner_;
    std::uint64_t terrainPendingRevision_=0;
    HRESULT PumpTerrainLoading();
    ComPtr<ID3D11Texture2D> terrainExclusionTexture_;
    ComPtr<ID3D11ShaderResourceView> terrainExclusionView_;
    UINT terrainExclusionWidth_=0,terrainExclusionHeight_=0;
    std::uint64_t terrainExclusionRevision_=1;
    std::vector<std::array<float,6>> terrainExclusionTriangles_;
    std::array<float,4> terrainExclusionDimensions_{};
    TerrainExclusionInputs terrainExclusionInputs_;
    ComPtr<ID3D11SamplerState> terrainSampler_;
    PagedSceneQueue<DrawPacket> overlays_;
    PagedSceneQueue<DrawPacket> scenePackets_;
    std::uint64_t sortedPacketMerges_=0;
    std::chrono::steady_clock::time_point lastStatsWrite_=std::chrono::steady_clock::now();
    ShadowConstants shadowData_{};
    ShadowCascadeGPU shadowCascades_;
    LocalShadowGPU localShadowAtlas_;
    LocalShadowBindings localShadowBindings_;
    LocalSurfaceLightingGPU localSurfaceLighting_;
    struct LocalSurfaceStats {std::uint64_t prepared=0,taggedDraws=0,taggedLights=0,binds=0;} localSurfaceStats_;
    std::array<LocalShadowFaceCache,LocalShadowFaces> localShadowCache_;
    std::uint64_t localShadowFrameTriangles_=0;
    struct LocalShadowStats {std::uint64_t selected=0,shadowed=0,faces=0,hits=0,draws=0,triangles=0,budgetBypass=0,unshadowed=0;double cpuMs=0;} localShadowStats_;
    ShadowCascadeGPU staticShadowCascades_;
    std::array<ShadowReuseCache,2> shadowReuse_;
    std::array<std::vector<ShadowDrawStamp>,2> shadowStampScratch_;std::vector<std::size_t> shadowStamped_;
    // perf86 diagnostics: GPU texture (re)uploads per reporting window.
    struct TextureUploadStat {std::uint64_t count=0,fresh=0;std::string detail;};
    std::unordered_map<const void*,TextureUploadStat> textureUploads_;
    void NoteTextureUpload(IDirect3DBaseTexture8 *native,bool fresh);
    CopyRectsStats copyRectsStats_;
    // perf92 volumetric shroud: the frame's shroud texture (from a replaced
    // native pass) and the stage-neutralizing white texture for water passes.
    bool ShroudTexture(TextureData &texture)const;
    bool ReplaceShroudPasses()const;
    HRESULT DrawShroudFog();
    HRESULT FinishWorldPasses(); // perf94: depth, GTAO and fog once per frame, before the HUD
    bool worldPassesDone_=false;
    std::shared_ptr<TextureData> shroudFrameTexture_;
    ComPtr<ID3D11ShaderResourceView> shroudWhite_;
    bool atmosphereReadyLast_=false; // the previous world frame prepared the atmosphere
    struct ShroudFogStats {std::uint64_t replaced=0,neutralized=0,fogPasses=0,flatPasses=0,missed=0,invalid=0,vision=0,beforeHud=0;} shroudFogStats_;
    bool shadowCache_=false;
    struct ShadowCacheStats {std::uint64_t passes=0,hits=0,rebuilds=0,staticDraws=0,liveDraws=0,savedDraws=0,copies=0,bypassed=0;
        double preparationMs=0;} shadowCacheStats_;
    struct ShadowStats {std::uint64_t frames=0,cascadeFrames=0,fallbackFrames=0,inputs=0;
        std::array<std::uint64_t,2> retained{},culled{},groups{};
        std::array<double,2> submitMs{},texelSum{};} shadowStats_;
    bool terrainShadows_=true;
    struct TerrainShadowStats {std::uint64_t inputs=0;std::array<std::uint64_t,2> retained{},stationary{},live{};} terrainShadowStats_;
    bool modernShadows_=false;
    bool cameraCulling_=true;
    struct SceneStats {
        std::uint64_t seen=0,boundsTested=0,culled=0,fallback=0,shadowInputs=0;
        std::uint64_t exclusionReuse=0,exclusionBuilds=0,exclusionUploads=0,roadVerticesTransformed=0;
        double preparationMs=0;
    } sceneStats_;
    NativeSceneCounts nativeStats_;
    struct FrameStats {
        std::uint64_t frames=0,cacheHits=0,cacheMisses=0,decodedVertices=0,decodedIndices=0,uploadBytes=0,draws=0;
        std::size_t cacheBytes=0,cacheEntries=0;std::uint64_t cacheEvictions=0,nativeDraws=0,groupedInstances=0;
        std::uint64_t gpuSamples=0,shaderCacheHits=0,shaderCompiles=0;
        double cpuMs=0,exclusionMs=0,shadowMs=0,sceneMs=0,gpuMs=0;
    } stats_;
    GpuFrameTimer gpuTimer_;
    GpuPassTimer passTimer_;
    GpuFrameTimer shadowDrawTimer_,lightPreparationTimer_;
    bool opaqueShadowDepth_=true;
    GpuPipelineStats pipelineStats_;
    ConstantUploadCache<sizeof(LegacyConstants)> drawConstantsCache_;
    ConstantUploadCache<1536> vertexConstantsCache_;
    ConstantUploadCache<sizeof(ShadowConstants)> shadowConstantsCache_;
    ConstantUploadCache<16> terrainConstantsCache_,instanceSettingsCache_;
    struct WorkStats {
        std::uint64_t constantUploads=0,constantSkips=0,dynamicVertexBytes=0,dynamicIndexBytes=0,compactedDraws=0;
        std::uint64_t sceneSeen=0,sceneEligible=0,sceneGroups=0,reorderedInstances=0,passSamples=0;
        std::array<std::uint64_t,9> eligibility{};
        std::uint64_t terrainCompactDraws=0,terrainFullDraws=0,pipelineSamples=0;
        std::array<std::uint64_t,7> pipelineCounts{};
        std::uint64_t opaqueShadowDraws=0,fallbackShadowDraws=0,shadowDrawSamples=0,lightPreparationSamples=0;
        double shadowDrawMs=0,lightPreparationMs=0;
        std::uint64_t terrainTracked=0,terrainFirst=0,terrainWorldChanged=0,terrainCameraChanged=0,
            terrainSourceChanged=0,terrainMapChanged=0,terrainShapeChanged=0,terrainOwnerChanged=0,terrainUntracked=0;
        std::array<double,static_cast<unsigned>(GpuPhase::Count)> passMs{};
        std::array<double,static_cast<unsigned>(GpuPhase::Count)> passCpuMs{};
        double groupingCpuMs=0;
    } workStats_;
    std::chrono::steady_clock::time_point frameStart_;
    double completedSceneCpuMs_=0;
    PresentedFrameTelemetry presentedFrames_;
    void WritePerformanceStats();
    std::unordered_map<IDirect3DSurface8*,SurfaceMapping> surfaces_;
    float clear_[4]={0,0,0,0};
    float clearDepth_=1;DWORD clearStencil_=0;
    bool inScene_=false,ready_=false,offscreen_=false;
    std::string error_;
};
// perf88: destroys a staged packet on every path where Draw did not commit it
// (early returns, rejected or failed draws), matching a local packet's lifetime.
struct StagedPacketGuard {
    CompatibilityRenderer &renderer;bool staged;
    ~StagedPacketGuard(){if(staged)renderer.DiscardStagedPacket();}
};
}
