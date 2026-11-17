#include "TerrainExclusionFilter.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CompatibilityRenderer.h"
#include "DeviceBridge.h"
#include "TerrainTileCache.h"
#include "NativeSurface.h"
#include <d3dx8.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cctype>
#include <DirectXMath.h>

namespace GenZH::Modern {
namespace {
using Pixels=std::vector<std::uint8_t>;
HRESULT ReadRGBA(IDirect3DTexture8 *texture,UINT width,UINT height,IDirect3DDevice8 *native,Pixels &pixels){
    D3DSURFACE_DESC sourceDescription{};D3DLOCKED_RECT direct{};
    if(SUCCEEDED(texture->GetLevelDesc(0,&sourceDescription))){
        SurfaceLayout layout=SurfaceLayout::BGRA8;bool supported=true;
        switch(sourceDescription.Format){
        case D3DFMT_A8R8G8B8:break;
        case D3DFMT_X8R8G8B8:layout=SurfaceLayout::BGRX8;break;
        case D3DFMT_A1R5G5B5:layout=SurfaceLayout::BGRA1555;break;
        case D3DFMT_R5G6B5:layout=SurfaceLayout::BGR565;break;
        case D3DFMT_A4R4G4B4:layout=SurfaceLayout::BGRA4444;break;
        default:supported=false;break;
        }
        if(supported&&SUCCEEDED(texture->LockRect(0,&direct,nullptr,D3DLOCK_READONLY))){
            ImageRGBA image;
            const bool copied=direct.Pitch>0&&CopySurfaceRGBA(static_cast<const std::uint8_t*>(direct.pBits),
                static_cast<std::size_t>(sourceDescription.Height)*direct.Pitch,sourceDescription.Width,sourceDescription.Height,static_cast<UINT>(direct.Pitch),layout,image);
            texture->UnlockRect(0);ImageRGBA resized;
            if(!copied||!ResizeRGBA(image,width,height,resized))return E_INVALIDARG;
            pixels=std::move(resized.pixels);return S_OK;
        }
    }
    // Unusual native formats keep their compatibility conversion fallback.
    ComPtr<IDirect3DTexture8> staging;
    HRESULT hr=D3DXCreateTexture(native,width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,staging.GetAddressOf());
    if(FAILED(hr))return hr;
    ComPtr<IDirect3DSurface8> source,dest;
    if(FAILED(hr=texture->GetSurfaceLevel(0,source.GetAddressOf()))||FAILED(hr=staging->GetSurfaceLevel(0,dest.GetAddressOf())))return hr;
    if(FAILED(hr=D3DXLoadSurfaceFromSurface(dest.Get(),nullptr,nullptr,source.Get(),nullptr,nullptr,D3DX_FILTER_LINEAR,0)))return hr;
    D3DLOCKED_RECT lock{};if(FAILED(hr=dest->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return hr;
    pixels.resize(static_cast<std::size_t>(width)*height*4);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
        const auto *in=static_cast<const std::uint8_t*>(lock.pBits)+static_cast<std::size_t>(y)*lock.Pitch+x*4;
        auto *out=pixels.data()+(static_cast<std::size_t>(y)*width+x)*4;
        out[0]=in[2];out[1]=in[1];out[2]=in[0];out[3]=in[3];
    }
    dest->UnlockRect();return S_OK;
}

}

HRESULT CompatibilityRenderer::BuildTerrainExclusion(){
    const DrawPacket *terrain=nullptr;
    for(const auto &packet:scenePackets_)if(packet.terrain){terrain=&packet;break;}
    // A HUD, decal or offscreen flush is not a map unload. Only terrain draws
    // bind this view; preserve its immutable occupancy between those barriers.
    // Device reset owns destruction; the atlas lifetime/revision guards reuse.
    if(!terrain)return S_OK;
    const auto *dimensions=terrain->terrainDimensions;
    const UINT width=std::min(2048u,static_cast<UINT>(dimensions[0])*4),height=std::min(2048u,static_cast<UINT>(dimensions[1])*4);
    if(!width||!height)return E_INVALIDARG;
    std::array<float,4> signature{};std::copy_n(dimensions,4,signature.begin());
    const auto blocksRevision=GetD3D11GroundBlocksRevision();
    const auto mapOwner=terrain->textures[0];
    const auto mapRevision=mapOwner?mapOwner->revision:0;
    const bool sameMap=terrainExclusionInputs_.MapMatches(mapOwner,mapRevision);
    std::vector<const DrawPacket*> roads;std::vector<GroundRoadInput> inputs;
    for(const auto &packet:scenePackets_){
        if(packet.terrain||packet.outputOverlay||packet.vertexShader||packet.constants.geometryFlags[0]>.5f||!packet.States().render[D3DRS_ZENABLE]||!packet.groundExclusionOverlay)continue;
        if(packet.primitive!=D3DPT_TRIANGLELIST&&packet.primitive!=D3DPT_TRIANGLESTRIP)continue;
        if(packet.States().render[D3DRS_ALPHABLENDENABLE]&&(packet.States().render[D3DRS_SRCBLEND]==D3DBLEND_ONE||packet.States().render[D3DRS_DESTBLEND]==D3DBLEND_ONE))continue;
        GroundRoadInput input;input.identity=packet.sourceIdentity;input.vertex=packet.sourceVertex;input.index=packet.sourceIndex;
        input.indexed=!packet.Indices().empty();input.primitive=static_cast<std::uint32_t>(packet.primitive);input.sourcePrimitive=static_cast<std::uint32_t>(packet.sourcePrimitive);std::copy_n(packet.constants.world,16,input.world.begin());
        inputs.push_back(input);roads.push_back(&packet);
    }
    if(sameMap&&terrainExclusionTexture_&&terrainExclusionView_&&width==terrainExclusionWidth_&&height==terrainExclusionHeight_&&
       terrainExclusionInputs_.Matches(blocksRevision,signature,inputs)){++sceneStats_.exclusionReuse;return S_OK;}
    ++sceneStats_.exclusionBuilds;
    const float sx=width/(dimensions[0]*10),sy=height/(dimensions[1]*10),border=dimensions[2];
    std::vector<std::array<float,6>> triangles;
    // Conservative world-space rasterization. Texture color never determines
    // occupancy. Map roads/concrete bibs are actual submitted flat triangles.
    auto raster=[&](DirectX::XMFLOAT2 a,DirectX::XMFLOAT2 b,DirectX::XMFLOAT2 c){
        triangles.push_back({a.x,a.y,b.x,b.y,c.x,c.y});
    };
    for(const auto &block:GetD3D11GroundBlocks()){
        DirectX::XMFLOAT2 corners[4];const float co=std::cos(block.angle),si=std::sin(block.angle);
        for(UINT i=0;i<4;++i){const float x=(i==0||i==3?-1:1)*block.major,y=(i<2?-1:1)*block.minor;
            corners[i]={block.x+x*co-y*si,block.y+x*si+y*co};}
        raster(corners[0],corners[1],corners[2]);raster(corners[0],corners[2],corners[3]);
    }
    for(const auto *road:roads){const auto &packet=*road;
        // RoadType registers actual loaded road filenames, including custom
        // map/mod textures. Cloud/fog passes are not ground obstructions.
        // Structures are covered by GeometryInfo footprints above. Scanning
        // every tank/building face here was costly and erased grass under roofs.
        DirectX::XMFLOAT4X4 world;std::memcpy(&world,packet.constants.world,64);const auto matrix=DirectX::XMLoadFloat4x4(&world);
        if(packet.geometry&&packet.geometry->sharedIndices){
            const auto &indices=packet.Indices();std::vector<DirectX::XMFLOAT3> positions(indices.size());
            sceneStats_.roadVerticesTransformed+=positions.size();
            for(std::size_t i=0;i<indices.size();++i){const auto *v=packet.Position(indices[i]);DirectX::XMStoreFloat3(&positions[i],DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(v[0],v[1],v[2],1),matrix));}
            for(std::size_t i=0;i+2<positions.size();i+=3){const auto a=positions[i],b=positions[i+1],c=positions[i+2];raster({a.x,a.y},{b.x,b.y},{c.x,c.y});}
            continue;
        }
        const auto first=packet.geometry&&packet.geometry->sharedVertices?packet.geometry->nativeFirst:0;
        const auto range=packet.geometry&&packet.geometry->sharedVertices?packet.geometry->nativeRange:packet.VertexCount();
        std::vector<DirectX::XMFLOAT3> positions(range);
        sceneStats_.roadVerticesTransformed+=positions.size();
        for(std::size_t i=0;i<positions.size();++i){const auto *v=packet.Position(first+i);DirectX::XMStoreFloat3(&positions[i],DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(v[0],v[1],v[2],1),matrix));}
        const auto count=packet.Indices().empty()?positions.size():packet.Indices().size();
        auto index=[&](std::size_t i){return packet.Indices().empty()?static_cast<UINT>(i):packet.Indices()[i]-first;};
        const std::size_t step=packet.primitive==D3DPT_TRIANGLELIST?3:1;
        for(std::size_t i=0;i+2<count;i+=step){auto a=positions[index(i)],b=positions[index(i+1)],c=positions[index(i+2)];
            // Only horizontal ground overlays here; elevated structures use
            // authoritative rotated object footprints above. Billboards and
            // particles must not erase patches of grass.
            raster({a.x,a.y},{b.x,b.y},{c.x,c.y});
        }
    }
    // Exact world-space input comparison; construction/destruction, rotations,
    // roads and a changed terrain mapping invalidate without a time delay.
    if(sameMap&&terrainExclusionTexture_&&width==terrainExclusionWidth_&&height==terrainExclusionHeight_&&
       signature==terrainExclusionDimensions_&&triangles==terrainExclusionTriangles_){terrainExclusionInputs_.Commit(blocksRevision,signature,std::move(inputs));return S_OK;}
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(width)*height,0);
    auto paint=[&](DirectX::XMFLOAT2 a,DirectX::XMFLOAT2 b,DirectX::XMFLOAT2 c){
        for(auto *p:{&a,&b,&c}){p->x=(p->x+border)*sx;p->y=(p->y+border)*sy;}
        const float area=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);if(std::abs(area)<.001f)return;
        const int minx=std::max(0,static_cast<int>(std::floor(std::min({a.x,b.x,c.x})-1)));
        const int miny=std::max(0,static_cast<int>(std::floor(std::min({a.y,b.y,c.y})-1)));
        const int maxx=std::min(static_cast<int>(width)-1,static_cast<int>(std::ceil(std::max({a.x,b.x,c.x})+1)));
        const int maxy=std::min(static_cast<int>(height)-1,static_cast<int>(std::ceil(std::max({a.y,b.y,c.y})+1)));
        for(int y=miny;y<=maxy;++y)for(int x=minx;x<=maxx;++x){
            const float px=x+.5f-a.x,py=y+.5f-a.y;
            const float u=(px*(c.y-a.y)-py*(c.x-a.x))/area,v=((b.x-a.x)*py-(b.y-a.y)*px)/area;
            if(u>=-.01f&&v>=-.01f&&u+v<=1.01f)mask[static_cast<std::size_t>(y)*width+x]=255;
        }
    };
    for(const auto &t:triangles)paint({t[0],t[1]},{t[2],t[3]},{t[4],t[5]});
    HRESULT hr=S_OK;
    if(width!=terrainExclusionWidth_||height!=terrainExclusionHeight_){
        terrainExclusionTexture_.Reset();terrainExclusionView_.Reset();
        D3D11_TEXTURE2D_DESC description{};description.Width=width;description.Height=height;description.MipLevels=description.ArraySize=description.SampleDesc.Count=1;
        description.Format=DXGI_FORMAT_R8_UNORM;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(hr=renderer_.Device()->CreateTexture2D(&description,nullptr,terrainExclusionTexture_.GetAddressOf()))||FAILED(hr=renderer_.Device()->CreateShaderResourceView(terrainExclusionTexture_.Get(),nullptr,terrainExclusionView_.GetAddressOf())))return hr;
        terrainExclusionWidth_=width;terrainExclusionHeight_=height;
    }
    const auto filteredMask=FilterTerrainExclusion(mask,width,height);
    renderer_.Context()->UpdateSubresource(terrainExclusionTexture_.Get(),0,nullptr,filteredMask.data(),width,0);
    ++sceneStats_.exclusionUploads;++terrainExclusionRevision_;
    terrainExclusionInputs_.Commit(blocksRevision,signature,std::move(inputs));
    terrainExclusionInputs_.CommitMap(mapOwner,mapRevision);
    terrainExclusionTriangles_=std::move(triangles);terrainExclusionDimensions_=signature;return S_OK;
}

HRESULT CompatibilityRenderer::PumpTerrainLoading(){
    auto owner=terrainPendingOwner_.lock();
    if(!owner||owner->revision!=terrainPendingRevision_){
        if(terrainPendingRevision_){terrainUpload_.reset();terrainPreparation_.Cancel();terrainPendingOwner_.reset();terrainPendingRevision_=0;}
        return S_OK;
    }
    auto &cached=terrainMaps_[owner->native.Get()];
    if(!terrainUpload_){auto result=terrainPreparation_.Take();
        if(!result)return S_OK;
        if(!result->error.empty()||result->pixels[0].empty()){
            cached.failedRevision=owner->revision;D3D11Diagnostic(("Terrain worker fallback: "+result->error).c_str());terrainPreparation_.Finish();terrainPendingOwner_.reset();terrainPendingRevision_=0;return S_OK;
        }
        terrainUpload_=std::make_unique<TerrainUpload>(std::move(result));
    }
    GenZHLoad::Scope upload("terrain_upload_slice","4 MiB / 2 ms submission budget");const auto before=terrainUpload_->Uploaded();
    HRESULT hr=terrainUpload_->Pump(renderer_.Device(),renderer_.Context());upload.bytes=terrainUpload_->Uploaded()-before;
    if(FAILED(hr)){
        cached.failedRevision=owner->revision;D3D11Diagnostic("Terrain bounded upload failed; retaining native terrain");terrainUpload_.reset();terrainPreparation_.Finish();terrainPendingOwner_.reset();terrainPendingRevision_=0;return S_OK;
    }
    if(terrainUpload_->Ready()){
        cached.climate=terrainUpload_->Data().climate;
        cached.views=terrainUpload_->Views();cached.cells=terrainUpload_->Cells();cached.classes=terrainUpload_->Classes();cached.revision=terrainPendingRevision_;cached.shadowOwner=std::make_shared<std::uint8_t>(std::uint8_t{0});
        GenZHLoad::Record("terrain_ready",terrainUpload_->Data().cacheHit?"atlas disk hit":"fresh atlas",0,terrainUpload_->Uploaded(),terrainUpload_->Data().cacheHit);
        terrainUpload_.reset();terrainPreparation_.Finish();terrainPendingOwner_.reset();terrainPendingRevision_=0;
    }
    return S_OK;
}

HRESULT CompatibilityRenderer::CaptureTerrain(DrawPacket &packet){
    // Only actual atlas metadata can enable the terrain pipeline. Water, roads,
    // buildings and decals must never be classified from their colors or FVF.
    if(offscreen_||packet.vertexShader||packet.outputOverlay||!packet.textures[0]||
       packet.constants.geometryFlags[0]>.5f||!packet.States().render[D3DRS_ZENABLE])return S_OK;
    const bool triangles=packet.primitive==D3DPT_TRIANGLELIST||packet.primitive==D3DPT_TRIANGLESTRIP||packet.primitive==D3DPT_TRIANGLEFAN;
    if(!triangles)return S_OK;
    if(!terrainEnabled_)return S_OK;
    auto owner=packet.textures[0];
    // Ordinary model/particle textures have no atlas. Avoid clearing a 28 KiB
    // atlas on every such draw; only allocate/read it after the size probe.
    DWORD bytes=0;owner->native->GetPrivateData(D3D11TerrainAtlasGUID,nullptr,&bytes);
    if(!bytes)return S_OK;
    if(bytes!=sizeof(D3D11TerrainAtlas))return Fail(E_INVALIDARG,"invalid map terrain atlas size");
    D3D11TerrainAtlas atlas;
    if(FAILED(owner->native->GetPrivateData(D3D11TerrainAtlasGUID,&atlas,&bytes)))return S_OK;
    if(bytes!=sizeof(atlas)||atlas.version!=2||atlas.count>200||!atlas.width||!atlas.height||atlas.width>4096||atlas.height>4096)return Fail(E_INVALIDARG,"invalid map terrain atlas");
    if(!atlas.cellsX||!atlas.cellsY||atlas.cellsX>1024||atlas.cellsY>1024)return S_OK;
    auto &cached=terrainMaps_[owner->native.Get()];
    if(cached.owner.expired()){cached=TerrainMaps{};cached.owner=owner;}
    HRESULT hr=S_OK;
    if(cached.revision!=owner->revision){
        if(cached.failedRevision==owner->revision)return S_OK;
        auto pending=terrainPendingOwner_.lock();
        // A second atlas drawn in this frame waits its turn. It must not cancel
        // another atlas every frame and starve the single preparation worker.
        if(pending==owner&&terrainPendingRevision_!=owner->revision){
            terrainUpload_.reset();terrainPreparation_.Cancel();terrainPendingOwner_.reset();terrainPendingRevision_=0;
        }
        if(!terrainPendingOwner_.lock()&&terrainPreparation_.Available()&&!materialStreaming_.CPUBusy()){
            GenZHLoad::Scope timing("terrain_snapshot_native","canonical CPU snapshot");
            TerrainRequest request;request.atlas=atlas;request.cells.resize(static_cast<std::size_t>(atlas.cellsX)*atlas.cellsY);
            DWORD cellBytes=static_cast<DWORD>(request.cells.size()*sizeof(D3D11TerrainCell));
            if(FAILED(hr=owner->native->GetPrivateData(D3D11TerrainCellsGUID,request.cells.data(),&cellBytes))||cellBytes!=request.cells.size()*sizeof(D3D11TerrainCell))return Fail(E_INVALIDARG,"missing canonical terrain cells");
            ComPtr<IDirect3DTexture8> base;
            if(FAILED(hr=owner->native->QueryInterface(IID_IDirect3DTexture8,reinterpret_cast<void**>(base.GetAddressOf())))||FAILED(hr=ReadRGBA(base.Get(),atlas.width,atlas.height,native_.Get(),request.base.pixels)))return Fail(hr,"read native map terrain snapshot");
            request.base.width=atlas.width;request.base.height=atlas.height;
            wchar_t folder[32768]{};DWORD length=GetEnvironmentVariableW(L"GENZH_MATERIAL_DIR",folder,32768);
            if(length&&length<32768)request.materials=folder;
            length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",folder,32768);if(length&&length<32768)request.profile=folder;
            std::string error;if(!ValidateTerrainRequest(request,error)){cached.failedRevision=owner->revision;D3D11Diagnostic(error.c_str());return S_OK;}
            timing.bytes=request.base.pixels.size()+request.cells.size()*sizeof(D3D11TerrainCell);
            // terrain97: native elevation (vertex heights + triangle split) for the GPU grass roots.
            cached.elevation.Reset();cached.flips.Reset();
            DWORD elevationBytes=0;owner->native->GetPrivateData(D3D11TerrainElevationGUID,nullptr,&elevationBytes);
            if(elevationBytes==D3D11TerrainElevationBytes(atlas.cellsX,atlas.cellsY)){
                std::vector<std::uint8_t> blob(elevationBytes);D3D11TerrainElevationHeader header;
                if(SUCCEEDED(owner->native->GetPrivateData(D3D11TerrainElevationGUID,blob.data(),&elevationBytes))&&(std::memcpy(&header,blob.data(),sizeof(header)),header.version==1&&header.cellsX==atlas.cellsX&&header.cellsY==atlas.cellsY)){
                    const auto *heights=reinterpret_cast<const float*>(blob.data()+sizeof(header));const UINT vertsX=atlas.cellsX+1,vertsY=atlas.cellsY+1;
                    const auto range=std::minmax_element(heights,heights+std::size_t(vertsX)*vertsY);bool finite=std::all_of(heights,heights+std::size_t(vertsX)*vertsY,[](float h){return std::isfinite(h);});
                    auto texture=[&](UINT width,UINT height,DXGI_FORMAT format,const void *data,UINT pitch,ComPtr<ID3D11ShaderResourceView> &view){
                        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                        D3D11_SUBRESOURCE_DATA init{data,pitch,0};ComPtr<ID3D11Texture2D> resource;
                        return SUCCEEDED(renderer_.Device()->CreateTexture2D(&desc,&init,resource.GetAddressOf()))&&SUCCEEDED(renderer_.Device()->CreateShaderResourceView(resource.Get(),nullptr,view.GetAddressOf()));};
                    if(finite&&texture(vertsX,vertsY,DXGI_FORMAT_R32_FLOAT,heights,vertsX*4,cached.elevation)&&
                       texture(atlas.cellsX,atlas.cellsY,DXGI_FORMAT_R8_UINT,blob.data()+sizeof(header)+std::size_t(vertsX)*vertsY*4,atlas.cellsX,cached.flips)){
                        cached.minHeight=*range.first;cached.maxHeight=*range.second;
                    }else{cached.elevation.Reset();cached.flips.Reset();}
                }
            }
            if(terrainPreparation_.Submit(std::move(request))){terrainPendingOwner_=owner;terrainPendingRevision_=owner->revision;}
        }
        // Keep the complete native terrain until all five maps and cells are
        // uploaded. Partial PBR/height/grass resources are never bound.
        return S_OK;
    }
    packet.terrain=true;packet.capturedTextures[0]=cached.views[0];
    packet.terrainClimate=cached.climate;
    // The map's native alpha blend uses the same atlas at stage 1.
    if(packet.textures[1]&&packet.textures[1]->native.Get()==owner->native.Get())packet.capturedTextures[1]=cached.views[0];
    packet.materialTextures[0]=cached.views[1];packet.materialTextures[1]=cached.views[2];
    packet.terrainHeight=cached.views[3];packet.terrainClass=cached.views[4];
    packet.terrainCells=cached.cells;packet.terrainClasses=cached.classes;packet.terrainShadowOwner=cached.shadowOwner;
    packet.terrainDimensions[0]=static_cast<float>(atlas.cellsX);packet.terrainDimensions[1]=static_cast<float>(atlas.cellsY);
    packet.terrainDimensions[2]=atlas.borderWorld;packet.terrainDimensions[3]=10;
    UINT flags=1|2|8;
    // terrain96: world-space photo-scanned layers per ground class (LegacyScene MaterialLight).
    if(TerrainLibraryActive())flags|=4096;
    // terrain97: macro relief in the terrain hull/domain shaders (TerrainGeometry.hlsl).
    if(TerrainReliefActive())flags|=8192;
    // terrain101: continuous ground-class field instead of the per-cell staircase.
    if(TerrainLibraryActive()&&terrainFieldEnabled_&&packet.terrainClasses)flags|=65536;
    // terrain102: hex stochastic tiling of the library layers (no visible tile repeat).
    if(TerrainLibraryActive()&&terrainHexEnabled_)flags|=131072;
    // terrain104: wet ground (porous darkening, standing water in the layers' low spots, rain rings).
    if(TerrainLibraryActive()&&terrainWetEnabled_)flags|=262144;
    // photo107: scattered rocks on the library ground (4194304), and the map's settled layer
    // (snow or sand dust, from the same climate that drives the atmosphere) on its flat areas.
    if(TerrainLibraryActive()&&terrainRocksEnabled_)flags|=4194304;
    if(TerrainLibraryActive()&&accumulationEnabled_)flags|=SettledLayerFlags(packet.terrainClimate,GetD3D11MapWeather().snowEnabled);
    // terrain97: GPU grass (16384) replaces the legacy geometry-shader blades (32).
    if(foliageEnabled_&&!packet.States().render[D3DRS_ALPHABLENDENABLE])flags|=grassEnabled_&&!grass_.Failed()&&cached.elevation&&cached.flips?16384:32;
    // terrain98: analytic grass shadows on the ground (32768), with the GPU grass only.
    if((flags&16384)&&grassShadowsEnabled_)flags|=32768;
    if(packet.textures[1]&&packet.textures[1]->native.Get()==owner->native.Get()&&AssemblyText(packet.pixelAssembly).find("lrp")!=std::string::npos)flags|=16;
    packet.constants.modernMaterial[3]=static_cast<float>(static_cast<UINT>(packet.constants.modernMaterial[3])|flags);
    // Full opaque terrain gets linear GGX. Multiplicative fog/cloud passes
    // retain the native pixel program and still use identical displacement.
    
    if(!packet.States().render[D3DRS_ALPHABLENDENABLE]&&packet.pixelShader&&!materialCompatibility_){
        packet.constants.modernMaterial[0]=2;
        auto &c=packet.constants;
        std::fill_n(c.globalAmbient,4,0.0f);
        std::fill_n(c.materialEmissive,4,0.0f);
        for(UINT light=0;light<4;++light){
            std::fill_n(c.lightPosition[light],4,0.0f);
            if(light>=3)continue;
            c.lightPosition[light][3]=static_cast<float>(D3DLIGHT_DIRECTIONAL);
            std::copy_n(atlas.diffuse[light],4,c.lightDiffuse[light]);
            std::fill_n(c.lightAmbient[light],4,0.0f);
            for(UINT channel=0;channel<3;++channel)c.globalAmbient[channel]+=atlas.ambient[light][channel];
            // WorldHeightMap uses lightPos as the incoming light direction.
            std::copy_n(atlas.direction[light],4,c.lightDirection[light]);
        }
    }
    return S_OK;
}
}
