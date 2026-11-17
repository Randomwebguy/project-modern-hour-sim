// SPDX-License-Identifier: GPL-3.0-or-later
#include "WW3D2/Backend/D3D11/SortedColorPalette.h"
// Explicit headless command: no game, window, audio or input initialization.
#include "Common/GameMemory.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshraycast.h"
#include "WW3D2/assetmgr.h"
#include "WW3D2/rendobj.h"
#include "WW3D2/htree.h"
#include "WW3D2/hlod.h"
#include "WW3D2/w3d_file.h"
#include "WW3D2/ShadowVertexParents.h"
#include "WW3D2/IndexBufferCounts.h"
#include "WW3D2/Backend/D3D11/NativeMeshGeometry.h"
#include "WW3D2/Backend/D3D11/NativeLegacyPacking.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8polygonrenderer.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/vertmaterial.h"
#include "WW3D2/mapper.h"
#include "WW3D2/Backend/D3D11/OrderedSortBatch.h"
#include "WW3D2/Backend/D3D11/StableDepthSort.h"
#include "WW3D2/Backend/D3D11/PresentedFrameTelemetry.h"
#include "WW3D2/Backend/D3D11/OpaqueSortPolicy.h"
#include "WW3D2/Backend/D3D11/NativeSubmitGate.h"
#include "WW3D2/Backend/D3D11/DeferredFixedWrites.h"
#include "WW3D2/Backend/D3D11/CompatibilityRenderer.h"
#include "WW3D2/Backend/D3D11/DeviceBridge.h"
#include "WWLib/rawfile.h"
#include "WWLib/chunkio.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <string>
#include <d3dcompiler.h>

namespace GenZH::Modern {
struct SceneConstantArenaCheckAccess {
 static unsigned Run(){
  unsigned checks=0;auto check=[&](bool ok,const char *label){++checks;if(!ok)throw std::runtime_error(label);};
  HWND window=CreateWindowExA(0,"STATIC","scene constant checks",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  check(window!=nullptr,"hidden constant arena target");
  struct WindowLease{HWND window;~WindowLease(){DestroyWindow(window);}} lease{window};
  RenderDevice renderer;check(SUCCEEDED(renderer.Initialize(window,64,64)),"constant arena GPU resource device");
  auto *device=renderer.Device();auto *context=renderer.Context();
  SceneConstantArena<LegacyConstants> arena;arena.Initialize(device,context,true);check(arena.Supported(),"fixture device supports constant buffer offsets");
  DrawPacket packet;std::vector<DrawGroup> groups(96,DrawGroup{&packet});unsigned index=0;
  auto fill=[&](const DrawPacket &){LegacyConstants c{};const unsigned a=index+17,b=index^0x7152ab00u;std::memcpy(c.world,&a,4);std::memcpy(c.vehicleProfile+3,&b,4);++index;return c;};
  check(arena.Prepare(device,groups,fill)&&index==96,"96 exact constant slices uploaded once, larger than 64 KiB");
  const char *source="cbuffer Draw:register(b0){uint4 values[142];} RWStructuredBuffer<uint2> result:register(u0); [numthreads(1,1,1)] void main(){result[values[0].x-17]=uint2(values[0].x,values[141].w);}";
  ComPtr<ID3DBlob> code,error;check(SUCCEEDED(D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,code.GetAddressOf(),error.GetAddressOf())),"constant slice GPU probe compiles");
  ComPtr<ID3D11ComputeShader> shader;check(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader.GetAddressOf())),"constant slice GPU shader");
  D3D11_BUFFER_DESC desc{};desc.ByteWidth=96*8;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=8;
  ComPtr<ID3D11Buffer> output,readback;check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,output.GetAddressOf())),"constant probe output");
  ComPtr<ID3D11UnorderedAccessView> uav;check(SUCCEEDED(device->CreateUnorderedAccessView(output.Get(),nullptr,uav.GetAddressOf())),"constant probe UAV");
  desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,readback.GetAddressOf())),"constant probe readback");
  ComPtr<ID3D11DeviceContext1> context1;check(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(context1.GetAddressOf()))),"constant range context");
  ID3D11UnorderedAccessView *view=uav.Get();context->CSSetUnorderedAccessViews(0,1,&view,nullptr);context->CSSetShader(shader.Get(),nullptr,0);
  for(unsigned i=0;i<96;++i){
   check(arena.Bind(i),"every prepared slice binds");
   ComPtr<ID3D11Buffer> bound;UINT first=0,size=0;context1->VSGetConstantBuffers1(0,1,bound.GetAddressOf(),&first,&size);
   constexpr unsigned sliceUnits=((sizeof(LegacyConstants)+255u)&~255u)/16u;check(first==i*sliceUnits&&size==sliceUnits,"range uses aligned 256-byte units, including offsets beyond 64 KiB");
   auto *buffer=bound.Get();context1->CSSetConstantBuffers1(0,1,&buffer,&first,&size);context->Dispatch(1,1,1);
  }
  view=nullptr;context->CSSetUnorderedAccessViews(0,1,&view,nullptr);context->CopyResource(readback.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
  check(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"constant GPU result maps");
  auto *values=static_cast<const unsigned*>(mapped.pData);bool exact=true;for(unsigned i=0;i<96;++i)exact=exact&&values[i*2]==i+17&&values[i*2+1]==(i^0x7152ab00u);
  context->Unmap(readback.Get(),0);check(exact,"all 96 GPU slices read exact first and last payload words");
  arena.End();check(!arena.Has(0)&&!arena.Bind(0),"frame end releases all slice eligibility");
  {ScopedSceneConstantReference reference(true);check(!arena.Prepare(device,groups,fill)&&!arena.Has(0),"explicit reference uses legacy constant buffer");}
  groups.resize(15000,DrawGroup{&packet});check(!arena.Prepare(device,groups,fill),"oversized scene retains complete legacy fallback");
  arena.Initialize(device,context,false);check(!arena.Supported()&&!arena.Prepare(device,groups,fill),"disabled/unsupported path never binds an offset buffer");
  arena.Reset();check(!arena.Has(0),"device reset discards resource and range state");
  context->ClearState();return checks;
 }
};
struct TerrainExclusionCheckAccess {
 static unsigned Run(){
  unsigned checks=0;auto check=[&](bool ok,const char *label){++checks;if(!ok)throw std::runtime_error(label);};
  HWND window=CreateWindowExA(0,"STATIC","terrain mask file checks",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  check(window!=nullptr,"hidden terrain mask target");
  struct WindowLease{HWND window;~WindowLease(){DestroyWindow(window);}} lease{window};
  CompatibilityRenderer renderer;check(SUCCEEDED(renderer.Renderer().Initialize(window,64,64)),"terrain GPU resource device");
  const auto oldBlocks=GetD3D11GroundBlocks();struct BlockLease{std::vector<D3D11GroundBlock> blocks;~BlockLease(){SetD3D11GroundBlocks(blocks);}} blockLease{oldBlocks};
  std::vector<D3D11GroundBlock> blocks{{20,20,6,4,0}};SetD3D11GroundBlocks(blocks);
  DrawPacket terrain;terrain.terrain=true;terrain.textures[0]=std::make_shared<TextureData>();
  terrain.terrainDimensions[0]=terrain.terrainDimensions[1]=16;terrain.terrainDimensions[3]=10;
  auto build=[&](bool world){renderer.scenePackets_.clear();if(world)renderer.scenePackets_.push_back(DrawPacket(terrain));check(SUCCEEDED(renderer.BuildTerrainExclusion()),"actual terrain mask builder");};
  auto pixels=[&](){
   D3D11_TEXTURE2D_DESC desc{};renderer.terrainExclusionTexture_->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(renderer.Renderer().Device()->CreateTexture2D(&desc,nullptr,staging.GetAddressOf())),"mask readback allocation");
   auto *context=renderer.Renderer().Context();context->CopyResource(staging.Get(),renderer.terrainExclusionTexture_.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
   check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"actual occupancy GPU readback");
   std::vector<unsigned char> result(desc.Width*desc.Height);for(unsigned y=0;y<desc.Height;++y)std::memcpy(result.data()+y*desc.Width,static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch,desc.Width);
   context->Unmap(staging.Get(),0);return result;
  };
  build(true);const auto first=pixels();auto *texture=renderer.terrainExclusionTexture_.Get();const auto revision=renderer.terrainExclusionRevision_;
  check(first[8*64+8]==255&&first[0]==0,"building covered and empty ground unmasked");
  for(unsigned i=0;i<20;++i){build(false);build(true);}
  check(renderer.terrainExclusionTexture_.Get()==texture&&renderer.terrainExclusionRevision_==revision&&renderer.sceneStats_.exclusionUploads==1,"twenty HUD/world barriers reuse the original GPU mask");
  check(pixels()==first,"resource reuse preserves every occupancy pixel");
  ++terrain.textures[0]->revision;build(true);check(renderer.sceneStats_.exclusionUploads==2&&pixels()==first,"atlas revision rebuilds exact occupancy");
  terrain.textures[0]=std::make_shared<TextureData>();build(true);check(renderer.sceneStats_.exclusionUploads==3&&pixels()==first,"different same-size map invalidates safely");
  blocks[0].x=100;blocks[0].y=100;SetD3D11GroundBlocks(blocks);build(true);const auto moved=pixels();
  check(renderer.sceneStats_.exclusionUploads==4&&moved[8*64+8]==0&&moved[40*64+40]==255,"moved/destroyed building footprint updates immediately");
  renderer.scenePackets_.clear();return checks;
 }
};
}

namespace {
void Require(bool ok,const char *reason){if(!ok)throw std::runtime_error(reason);}
void Chunk(ChunkSaveClass &save,unsigned id,const void *data,unsigned bytes){
    Require(save.Begin_Chunk(id),"begin fixture chunk");
    Require(save.Write(data,bytes)==bytes,"write fixture chunk");
    Require(save.End_Chunk(),"end fixture chunk");
}
template<class T> std::vector<T> ReadArray(ChunkLoadClass &load){
    Require(load.Cur_Chunk_Length()%sizeof(T)==0,"invalid source array length");
    std::vector<T> data(load.Cur_Chunk_Length()/sizeof(T));
    Require(load.Read(data.data(),load.Cur_Chunk_Length())==load.Cur_Chunk_Length(),"read source array");
    return data;
}
struct ModelTotals {unsigned files=0,meshes=0,roots=0,bones=0,bindings=0,nativeSources=0,nativeGeometry=0;uint64_t vertices=0,triangles=0,sortedOpaqueTriangles=0,sortedTransparentTriangles=0;};
void ReadUV(ChunkLoadClass &chunks,std::vector<W3dTexCoordStruct> &uv){
    while(chunks.Open_Chunk()){
        const auto id=chunks.Cur_Chunk_ID();
        if(id==W3D_CHUNK_STAGE_TEXCOORDS&&uv.empty())uv=ReadArray<W3dTexCoordStruct>(chunks);
        else if(id==W3D_CHUNK_TEXTURE_STAGE)ReadUV(chunks,uv);
        chunks.Close_Chunk();
    }
}
void CheckExistingModel(const std::filesystem::path &path,WW3DAssetManager &assets,ModelTotals &totals){
    RawFileClass source(path.string().c_str());Require(source.Open(FileClass::READ)!=0,"open current model");
    ChunkLoadClass chunks(&source);
    std::vector<W3dHLodHeaderStruct> roots;
    std::vector<std::vector<W3dHLodSubObjectStruct>> rootBindings;
    std::vector<std::pair<W3dHierarchyStruct,std::vector<W3dPivotStruct>>> trees;
    while(chunks.Open_Chunk()){
        const auto id=chunks.Cur_Chunk_ID();
        if(id==W3D_CHUNK_MESH){
            const int meshOffset=source.Seek(0)-8;
            std::vector<Vector3> positions,normals;
            std::vector<W3dTriStruct> triangles;
            std::vector<W3dVertInfStruct> influences;
            std::vector<W3dTexCoordStruct> uv;
            while(chunks.Open_Chunk()){
                switch(chunks.Cur_Chunk_ID()){
                    case W3D_CHUNK_VERTICES:positions=ReadArray<Vector3>(chunks);break;
                    case W3D_CHUNK_VERTEX_NORMALS:normals=ReadArray<Vector3>(chunks);break;
                    case W3D_CHUNK_TRIANGLES:triangles=ReadArray<W3dTriStruct>(chunks);break;
                    case W3D_CHUNK_VERTEX_INFLUENCES:influences=ReadArray<W3dVertInfStruct>(chunks);break;
                    case W3D_CHUNK_MATERIAL_PASS:ReadUV(chunks,uv);break;
                }
                chunks.Close_Chunk();
            }
            RawFileClass nativeFile(path.string().c_str());Require(nativeFile.Open(FileClass::READ)!=0,"open native mesh source");
            Require(nativeFile.Seek(meshOffset,SEEK_SET)==meshOffset,"seek native mesh source");
            ChunkLoadClass nativeChunks(&nativeFile);Require(nativeChunks.Open_Chunk(),"open native mesh chunk");
            std::function<void()> retainedCheck;
            {
            MeshModelClass mesh;Require(mesh.Load_W3D(nativeChunks)==WW3D_ERROR_OK,"current native mesh load");
            if(mesh.Get_Flag(MeshGeometryClass::SORT))for(unsigned pass=0;pass<mesh.Get_Pass_Count();++pass)
                for(unsigned i=0;i<triangles.size();++i){const auto shader=mesh.Get_Shader(i,pass);
                    if(shader.Get_Dst_Blend_Func()==ShaderClass::DSTBLEND_ZERO&&shader.Get_Src_Blend_Func()==ShaderClass::SRCBLEND_ONE&&shader.Get_Depth_Mask()==ShaderClass::DEPTH_WRITE_ENABLE)++totals.sortedOpaqueTriangles;
                    else ++totals.sortedTransparentTriangles;
                }
            Require(mesh.Get_Vertex_Count()==positions.size()&&mesh.Get_Polygon_Count()==triangles.size(),"current counts changed");
            Require(normals.size()==positions.size(),"current normal count");
            Require(std::memcmp(mesh.Get_Vertex_Array(),positions.data(),positions.size()*sizeof(Vector3))==0,"current vertices changed");
            Require(std::memcmp(mesh.Get_Vertex_Normal_Array(),normals.data(),normals.size()*sizeof(Vector3))==0,"current normals changed");
            for(size_t i=0;i<triangles.size();++i)for(unsigned c=0;c<3;++c)
                Require(mesh.Get_Polygon_Array()[i][c]==triangles[i].Vindex[c],"current indices changed");
            if(!uv.empty()){
                Require(uv.size()==positions.size()&&mesh.Get_UV_Array()!=nullptr,"current UV count");
                for(size_t i=0;i<uv.size();++i)Require(mesh.Get_UV_Array()[i].X==uv[i].U&&mesh.Get_UV_Array()[i].Y==1.0f-uv[i].V,"current UV changed");
            }
            if(!influences.empty())for(size_t i=0;i<influences.size();++i)
                Require(mesh.Get_Vertex_Bone_Links()[i]==influences[i].BoneIdx,"current skin links changed");
            {
                using namespace GenZH::Modern;
                auto recipe=std::make_shared<NativeGeometryRecipe>();recipe->polygons.resize(triangles.size());
                for(unsigned i=0;i<triangles.size();++i)recipe->polygons[i]=i;
                const unsigned channels=std::min(static_cast<unsigned>(mesh.Get_UV_Array_Count()),8u);
                unsigned fvf=D3DFVF_XYZ|D3DFVF_NORMAL|(channels<<D3DFVF_TEXCOUNT_SHIFT);
                if(mesh.Get_Color_Array(0,false))fvf|=D3DFVF_DIFFUSE;if(mesh.Get_Color_Array(1,false))fvf|=D3DFVF_SPECULAR;
                auto view=CaptureNativeGeometry(mesh,fvf,std::move(recipe),0,0,0,mesh.Get_Vertex_Count());
                DecodedGeometry geometry;Require(SUCCEEDED(BuildNativeGeometry(view,geometry)),"actual native compact geometry construction");
                Require(geometry.vertices.empty()&&geometry.fixedVertices.size()==positions.size(),"actual native output has no expanded vertex copy");
                for(size_t i=0;i<positions.size();++i){const auto &v=geometry.fixedVertices[i];
                    Require(std::memcmp(v.position,&positions[i],12)==0&&v.position[3]==1,"actual native position differs");
                    Require(std::memcmp(v.normal,&normals[i],12)==0,"actual native normal differs");
                    for(unsigned channel=0;channel<channels;++channel)Require(std::memcmp(v.uv[channel],&mesh.Get_UV_Array_By_Index(channel)[i],8)==0,"actual native UV channel differs");
                }
                for(size_t i=0;i<triangles.size();++i)for(unsigned corner=0;corner<3;++corner)Require(geometry.indices[i*3+corner]==triangles[i].Vindex[corner],"actual native uint32 topology differs");
                auto retained=view;retained.modelContext=nullptr;retained.current=nullptr;
                auto owners=mesh.Retain_Render_Arrays();
                retainedCheck=[retained,owners=std::move(owners),expected=std::move(geometry)](){
                    const auto stride=NativeLegacyStride(retained);
                    std::vector<std::uint8_t> packed(std::size_t(stride)*retained.vertices);
                    Require(stride&&PackNativeLegacyVertices(retained,packed.data(),packed.size()),"actual arrays survive MeshModel destruction and pack fallback");
                    std::vector<LegacyVertex> decoded;std::string error;
                    Require(SUCCEEDED(DecodeVertices(packed.data(),packed.size(),stride,retained.fvf,0,retained.vertices,decoded,error)),"actual retained fallback FVF decode");
                    for(size_t i=0;i<decoded.size();++i)Require(std::memcmp(&decoded[i],&expected.fixedVertices[i],192)==0,"actual fallback native PBR input bytes differ");
                    std::vector<std::uint32_t> indices(expected.indices.size());
                    Require(PackNativeLegacyIndices(retained,0,reinterpret_cast<std::uint8_t*>(indices.data()),indices.size()*4)&&indices==expected.indices,"actual retained R32 category topology differs");
                };
                // Actual production buffer wrappers reserve metadata/recipes
                // without touching a device or allocating a COM buffer.
                const auto allocated=deferredLegacyStats.materializedBuffers;
                auto *lazyVB=NEW_REF(DX8VertexBufferClass,(fvf,mesh.Get_Vertex_Count(),DX8VertexBufferClass::USAGE_DEFAULT,true));
                auto *lazyIB=NEW_REF(DX8IndexBufferClass,(triangles.size()*3,DX8IndexBufferClass::USAGE_DEFAULT,true));
                lazyVB->Add_Deferred_Write(0,mesh.Get_Vertex_Count()*NativeLegacyStride(retained),
                    [retained](std::uint8_t *out,std::size_t bytes){return PackNativeLegacyVertices(retained,out,bytes);});
                lazyIB->Add_Deferred_Write(0,triangles.size()*12,
                    [retained](std::uint8_t *out,std::size_t bytes){return PackNativeLegacyIndices(retained,0,out,bytes);});
                Require(lazyVB->Deferred_Legacy()&&lazyIB->Deferred_Legacy()&&lazyVB->deferred_writes.Pending()==1&&lazyIB->deferred_writes.Pending()==1&&allocated==deferredLegacyStats.materializedBuffers,"actual wrappers eagerly allocated legacy buffers");
                lazyVB->Release_Ref();lazyIB->Release_Ref();
                ++totals.nativeGeometry;
            }
            } // Destroy model before exercising retained array providers.
            retainedCheck();
            ++totals.meshes;totals.vertices+=positions.size();totals.triangles+=triangles.size();
        }else if(id==W3D_CHUNK_HLOD){
            std::vector<W3dHLodSubObjectStruct> bindings;
            while(chunks.Open_Chunk()){
                if(chunks.Cur_Chunk_ID()==W3D_CHUNK_HLOD_HEADER){auto a=ReadArray<W3dHLodHeaderStruct>(chunks);Require(a.size()==1,"HLOD header");roots.push_back(a[0]);}
                if(chunks.Cur_Chunk_ID()==W3D_CHUNK_HLOD_LOD_ARRAY){
                    while(chunks.Open_Chunk()){
                        if(chunks.Cur_Chunk_ID()==W3D_CHUNK_HLOD_SUB_OBJECT){auto a=ReadArray<W3dHLodSubObjectStruct>(chunks);Require(a.size()==1,"HLOD binding");bindings.push_back(a[0]);}
                        chunks.Close_Chunk();
                    }
                }
                chunks.Close_Chunk();
            }
            rootBindings.push_back(std::move(bindings));
        }else if(id==W3D_CHUNK_HIERARCHY){
            W3dHierarchyStruct h{};std::vector<W3dPivotStruct> pivots;
            while(chunks.Open_Chunk()){
                if(chunks.Cur_Chunk_ID()==W3D_CHUNK_HIERARCHY_HEADER){auto a=ReadArray<W3dHierarchyStruct>(chunks);Require(a.size()==1,"hierarchy header");h=a[0];}
                if(chunks.Cur_Chunk_ID()==W3D_CHUNK_PIVOTS)pivots=ReadArray<W3dPivotStruct>(chunks);
                chunks.Close_Chunk();
            }
            trees.push_back({h,std::move(pivots)});
        }
        chunks.Close_Chunk();
    }
    source.Close();
    Require(assets.Load_3D_Assets(source),"load current asset prototypes");
    for(const auto &tree:trees){
        const auto *native=assets.Get_HTree(tree.first.Name);Require(native!=nullptr,"current hierarchy missing");
        Require(native->Num_Pivots()==tree.second.size(),"current hierarchy count changed");
        for(size_t i=0;i<tree.second.size();++i){
            Require(std::strcmp(native->Get_Bone_Name(static_cast<int>(i)),tree.second[i].Name)==0,"current bone name changed");
            const auto expectedParent=tree.second[i].ParentIdx==0xffffffffu?0u:tree.second[i].ParentIdx;
            Require(static_cast<unsigned>(native->Get_Parent_Index(static_cast<int>(i)))==expectedParent,"current bone parent changed");
        }
        totals.bones+=static_cast<unsigned>(tree.second.size());
    }
    for(size_t r=0;r<roots.size();++r){
        const auto &header=roots[r];
        Require(assets.Render_Obj_Exists(header.Name),"current HLOD prototype missing");
        auto *root=assets.Create_Render_Obj(header.Name);Require(root!=nullptr,"instantiate current HLOD");
        Require(root->Get_Num_Sub_Objects()>0,"current HLOD children missing");
        auto *lod=dynamic_cast<HLodClass*>(root);Require(lod!=nullptr,"current HLOD type");
        root->Set_Native_Drawable_ID(231);
        auto *clone=root->Clone();auto *cloneLod=dynamic_cast<HLodClass*>(clone);
        Require(cloneLod&&clone->Get_Native_Render_ID()!=root->Get_Native_Render_ID(),"native clone root identity");
        Require(clone->Get_Native_Drawable_ID()==0,"prototype clone must not steal drawable owner");
        for(int l=0;l<lod->Get_Lod_Count();++l)for(int c=0;c<lod->Get_Lod_Model_Count(l);++c){
            auto *mesh=dynamic_cast<MeshClass*>(lod->Peek_Lod_Model(l,c));
            auto *cloneMesh=dynamic_cast<MeshClass*>(cloneLod->Peek_Lod_Model(l,c));
            if(!mesh)continue;
            Require(cloneMesh!=nullptr,"native clone mesh type");
            auto native=GenZH::Modern::CaptureNativeMeshSource<RenderObjClass>(*mesh,0,!!mesh->Peek_Model()->Get_Flag(MeshGeometryClass::SKIN));
            Require(native.Valid()&&native.root==root->Get_Native_Render_ID()&&native.drawable==231,"native mesh root/drawable capture");
            Require(native.mesh!=cloneMesh->Get_Native_Render_ID(),"native cloned mesh instance identity");
            Require(native.resource==cloneMesh->Peek_Model()->Get_Native_Geometry_ID(),"native cloned meshes share resource identity");
            Require(native.bone==lod->Get_Lod_Model_Bone(l,c)&&native.lod==root->Get_LOD_Level(),"native attachment/LOD capture");
            ++totals.nativeSources;
        }
        clone->Release_Ref();
        for(const auto &binding:rootBindings[r]){
            bool found=false;
            for(int l=0;l<lod->Get_Lod_Count();++l)for(int c=0;c<lod->Get_Lod_Model_Count(l);++c){
                const auto *model=lod->Peek_Lod_Model(l,c);
                if(model&&_stricmp(model->Get_Name(),binding.Name)==0&&lod->Get_Lod_Model_Bone(l,c)==binding.BoneIndex)found=true;
            }
            Require(found,"current mesh to pivot binding changed");++totals.bindings;
        }
        root->Release_Ref();++totals.roots;
    }
    ++totals.files;
}
void CheckMesh(const std::filesystem::path &folder,bool skin){
    constexpr unsigned count=70005,polygons=count/3;
    std::vector<Vector3> positions(count),normals(count);
    std::vector<W3dTexCoordStruct> uv(count);
    std::vector<W3dVertInfStruct> bones(count);
    std::vector<W3dTriStruct> triangles(polygons);
    for(unsigned i=0;i<count;++i){
        const unsigned t=i/3,c=i%3;
        positions[i].Set(float((t%257)*2)+(c==1?1.0f:0.0f),float((t/257)*2)+(c==2?1.0f:0.0f),0);
        normals[i].Set(0,0,1);uv[i].U=float(i%128)/128.0f;uv[i].V=float(i%256)/256.0f;
        bones[i]={};bones[i].BoneIdx=static_cast<uint16>(i%11);
    }
    for(unsigned i=0;i<polygons;++i){triangles[i]={};for(unsigned c=0;c<3;++c)triangles[i].Vindex[c]=i*3+c;triangles[i].Normal.Z=1;}
    W3dMeshHeader3Struct header{};header.Version=W3D_MAKE_VERSION(4,2);
    header.Attributes=skin?W3D_MESH_FLAG_GEOMETRY_TYPE_SKIN:W3D_MESH_FLAG_COLLISION_TYPE_PHYSICAL;
    std::strcpy(header.MeshName,skin?"SKIN70005":"RIGID70005");std::strcpy(header.ContainerName,"PORT19");
    header.NumTris=polygons;header.NumVertices=count;header.Max.X=514;header.Max.Y=184;header.SphRadius=600;
    const auto path=folder/(skin?"skin70005.w3d":"rigid70005.w3d");
    {RawFileClass file(path.string().c_str());Require(file.Open(FileClass::WRITE)!=0,"open fixture write");ChunkSaveClass save(&file);
    Require(save.Begin_Chunk(W3D_CHUNK_MESH),"begin mesh");
    Chunk(save,W3D_CHUNK_MESH_HEADER3,&header,sizeof(header));
    Chunk(save,W3D_CHUNK_VERTICES,positions.data(),count*sizeof(Vector3));
    Chunk(save,W3D_CHUNK_VERTEX_NORMALS,normals.data(),count*sizeof(Vector3));
    Chunk(save,W3D_CHUNK_TRIANGLES,triangles.data(),polygons*sizeof(W3dTriStruct));
    if(skin)Chunk(save,W3D_CHUNK_VERTEX_INFLUENCES,bones.data(),count*sizeof(W3dVertInfStruct));
    W3dMaterialInfoStruct material{};material.PassCount=1;
    Chunk(save,W3D_CHUNK_MATERIAL_INFO,&material,sizeof(material));
    Require(save.Begin_Chunk(W3D_CHUNK_MATERIAL_PASS),"begin material pass");
    Require(save.Begin_Chunk(W3D_CHUNK_TEXTURE_STAGE),"begin texture stage");
    Chunk(save,W3D_CHUNK_STAGE_TEXCOORDS,uv.data(),count*sizeof(W3dTexCoordStruct));
    Require(save.End_Chunk()&&save.End_Chunk()&&save.End_Chunk(),"finish fixture");}
    MeshModelClass mesh;
    RawFileClass file(path.string().c_str());Require(file.Open(FileClass::READ)!=0,"open fixture read");ChunkLoadClass load(&file);
    Require(load.Open_Chunk()&&load.Cur_Chunk_ID()==W3D_CHUNK_MESH,"read mesh chunk");
    Require(mesh.Load_W3D(load)==WW3D_ERROR_OK,"native MeshModelClass load failed");
    Require(mesh.Get_Vertex_Count()==count&&mesh.Get_Polygon_Count()==polygons,"native counts truncated");
    Require(std::memcmp(mesh.Get_Vertex_Array(),positions.data(),count*sizeof(Vector3))==0,"native vertex bytes changed");
    Require(std::memcmp(mesh.Get_Vertex_Normal_Array(),normals.data(),count*sizeof(Vector3))==0,"native normals changed");
    const auto *nativeUV=mesh.Get_UV_Array();Require(nativeUV!=nullptr,"native UV missing");
    const auto *nativeTriangles=mesh.Get_Polygon_Array();
    for(unsigned i=0;i<polygons;++i)for(unsigned c=0;c<3;++c)Require(nativeTriangles[i][c]==triangles[i].Vindex[c],"native triangle index truncated");
    for(unsigned i=0;i<count;++i){
        Require(nativeUV[i].X==uv[i].U&&nativeUV[i].Y==1.0f-uv[i].V,"UV reference changed");
        if(skin)Require(mesh.Get_Vertex_Bone_Links()[i]==bones[i].BoneIdx,"native bone link changed");
    }
    if(!skin)Require(mesh.Has_Cull_Tree(),"wide native collision tree missing");
    std::vector<unsigned> parents;Require(W3DGeometry::BuildShadowVertexParents(positions.data(),count,parents)==count,"shadow parent count");
    for(unsigned i=0;i<count;++i)Require(parents[i]==i,"shadow parent truncated");
    positions.push_back(positions[65536]);positions.push_back(Vector3(-0.0f,0,0));
    Require(W3DGeometry::BuildShadowVertexParents(positions.data(),static_cast<unsigned>(positions.size()),parents)==count,"shadow duplicate lookup");
    Require(parents[count]==65536&&parents[count+1]==0,"shadow high index or signed zero changed");
}
}
int RunGeometryPortChecks(const char *path){
    std::filesystem::path folder(path);std::filesystem::create_directories(folder);
    std::filesystem::remove(folder/"native-geometry-result.json");
    std::filesystem::remove(folder/"native-geometry-error.txt");
    const auto start=std::chrono::steady_clock::now();
    try{
        WW3DAssetManager assets;
        Require(sizeof(TriIndex)==12&&sizeof(W3DIndex)==4,"native index width");
        Require(W3DIndexByteSize(210015)==840060,"native index byte count");
        CheckMesh(folder,false);CheckMesh(folder,true);
        ModelTotals totals;
        if(std::filesystem::exists(folder/"models"))for(const auto &entry:std::filesystem::directory_iterator(folder/"models"))
            if(entry.is_regular_file())CheckExistingModel(entry.path(),assets,totals);
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        std::ofstream(folder/"native-geometry-result.json")<<"{\"passed\":true,\"pointer_bits\":"<<sizeof(void*)*8
            <<",\"native_index_bits\":32,\"vertices_per_mesh\":70005,\"meshes\":2,\"max_index\":70004,"
            "\"uv_normals_bone_links_preserved\":true,\"collision_tree\":true,\"dynamic_shadow_lookup\":true,\"current_model_files\":"<<totals.files
            <<",\"current_meshes\":"<<totals.meshes<<",\"current_vertices\":"<<totals.vertices<<",\"current_triangles\":"<<totals.triangles
            <<",\"sorted_opaque_triangles\":"<<totals.sortedOpaqueTriangles<<",\"sorted_other_triangles\":"<<totals.sortedTransparentTriangles<<",\"current_HLOD_roots\":"<<totals.roots<<",\"current_bones\":"<<totals.bones<<",\"current_mesh_pivot_bindings\":"<<totals.bindings<<",\"native_source_meshes\":"<<totals.nativeSources<<",\"native_geometry_meshes\":"<<totals.nativeGeometry<<",\"game_started\":false,\"milliseconds\":"<<ms<<"}\n";
        return 0;
    }catch(const std::exception &error){std::ofstream(folder/"native-geometry-error.txt")<<error.what();return 2;}
}

// Exercise the actual WW3D sorter inside the final executable, with a hidden
// native device. Header policy checks cannot prove this source was linked.
static unsigned CheckSortedPaletteGPU(IDirect3DDevice8 *device,bool materialColors=false){
 using namespace GenZH::Modern;
 constexpr UINT count=70018,base=13,stride=44;
 const DWORD fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2;
 ComPtr<IDirect3DVertexBuffer8> vb;ComPtr<IDirect3DIndexBuffer8> ib;
 Require(SUCCEEDED(device->CreateVertexBuffer(count*stride,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,vb.GetAddressOf())),"shared sorted GPU VB allocation");
 Require(SUCCEEDED(device->CreateIndexBuffer(17*4,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,D3DFMT_INDEX32,D3DPOOL_DEFAULT,ib.GetAddressOf())),"shared sorted GPU R32 IB allocation");
 NativeMeshSource source;source.root=201;source.mesh=202;source.resource=203;source.drawable=3;source.sorted=true;
 D3DMATRIX identity{};identity._11=identity._22=identity._33=identity._44=1;
 D3DMATERIAL8 material{};material.Diffuse=material.Ambient={1,1,1,1};
 ComPtr<IDirect3DTexture8> checker;
 Require(SUCCEEDED(device->CreateTexture(16,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,checker.GetAddressOf())),"two-UV checker texture");
 D3DLOCKED_RECT checkerLock{};Require(SUCCEEDED(checker->LockRect(0,&checkerLock,nullptr,0)),"two-UV checker pixels");
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){const DWORD color=0xff000000u|((32+x*13)<<16)|((40+y*11)<<8)|(90+(x+y)%8*20);
  std::memcpy(static_cast<BYTE*>(checkerLock.pBits)+y*checkerLock.Pitch+x*4,&color,4);}
 Require(SUCCEEDED(checker->UnlockRect(0)),"two-UV checker commit");
 unsigned comparisons=0;
 for(unsigned alpha=0;alpha<2;++alpha)for(unsigned uvCase=0;uvCase<12;++uvCase){std::vector<unsigned> reference;
  for(unsigned mode=0;mode<2;++mode){
   ScopedSortedColorPaletteReference legacy(!mode);
   ScopedSortedMaterialPaletteReference materialReference(!mode);
   const auto beforePalette=sortedColorPaletteCounts;
   BYTE *bytes=nullptr;
   Require(SUCCEEDED(vb->Lock(0,0,&bytes,D3DLOCK_DISCARD)),"capture fresh VB revision");
   std::memset(bytes,0,count*stride);
   const UINT vertices[]={70017,65549,16,70016,65548,17,70015,65547,18,70014,65546,19,70013,65545,20};const float positions[][3]={{-.6f,-.5f,.4f},{.65f,-.45f,.4f},{-.05f,.65f,.4f}};
   for(unsigned n=0;n<15;++n){const unsigned i=n%3;auto *v=bytes+vertices[n]*stride;std::memcpy(v,positions[i],12);const float normal[]={0,0,1};std::memcpy(v+12,normal,12);
    const DWORD color=(alpha?0x80:0xff)*0x01000000u|(i==0?0xe04030:(i==1?0x30d060:0x4070e0));std::memcpy(v+24,&color,4);
    const float uv[]={float(i)*.3f,.2f,float(i)*.2f,.7f};std::memcpy(v+28,uv,16);}
   Require(SUCCEEDED(vb->Unlock()),"commit VB revision");
   Require(SUCCEEDED(ib->Lock(0,0,&bytes,D3DLOCK_DISCARD)),"capture fresh IB revision");
   UINT raw[17]={UINT_MAX,UINT_MAX};for(unsigned n=0;n<15;++n)raw[2+n]=vertices[n]-base;std::memcpy(bytes,raw,sizeof(raw));
   Require(SUCCEEDED(ib->Unlock()),"commit IB revision");
   Require(SUCCEEDED(device->SetStreamSource(0,vb.Get(),stride))&&SUCCEEDED(device->SetIndices(ib.Get(),base)),"shared GPU offset bindings");
   Require(SUCCEEDED(device->SetVertexShader(fvf)),"shared fixed FVF");
   device->SetPixelShader(0);device->SetMaterial(&material);device->SetTransform(D3DTS_VIEW,&identity);device->SetTransform(D3DTS_PROJECTION,&identity);
   D3DVIEWPORT8 viewport{0,0,64,64,0,1};device->SetViewport(&viewport);
   device->SetRenderState(D3DRS_COLORVERTEX,TRUE);
   device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE,materialColors?D3DMCS_MATERIAL:D3DMCS_COLOR1);
   device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE,D3DMCS_MATERIAL);device->SetRenderState(D3DRS_SPECULARMATERIALSOURCE,D3DMCS_MATERIAL);
   device->SetRenderState(D3DRS_SPECULARENABLE,materialColors?TRUE:FALSE);
   device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_FOGENABLE,FALSE);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
   device->SetRenderState(D3DRS_ZENABLE,TRUE);device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);device->SetRenderState(D3DRS_ZWRITEENABLE,alpha?FALSE:TRUE);
   device->SetRenderState(D3DRS_ALPHABLENDENABLE,alpha?TRUE:FALSE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
   device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);device->SetRenderState(D3DRS_STENCILENABLE,FALSE);device->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
   for(unsigned stage=0;stage<8;++stage){device->SetTexture(stage,nullptr);device->SetTextureStageState(stage,D3DTSS_COLOROP,stage?D3DTOP_DISABLE:D3DTOP_SELECTARG1);device->SetTextureStageState(stage,D3DTSS_COLORARG1,D3DTA_DIFFUSE);device->SetTextureStageState(stage,D3DTSS_ALPHAOP,stage?D3DTOP_DISABLE:D3DTOP_SELECTARG1);device->SetTextureStageState(stage,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);}
   if(uvCase){
    device->SetTexture(0,checker.Get());device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE);
    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
   }
   device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,uvCase>=1&&uvCase<=8?uvCase-1:0);
   auto textureMatrix=identity;textureMatrix._11=.7f;textureMatrix._22=.8f;textureMatrix._41=.15f;textureMatrix._42=.2f;textureMatrix._44=.65f;
   device->SetTransform(D3DTS_TEXTURE0,&textureMatrix);
   device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,uvCase==9?D3DTTFF_COUNT3|D3DTTFF_PROJECTED:D3DTTFF_DISABLE);
   if(uvCase==10){device->SetTexture(1,checker.Get());device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_ADD);
    device->SetTextureStageState(1,D3DTSS_COLORARG1,D3DTA_TEXTURE);device->SetTextureStageState(1,D3DTSS_COLORARG2,D3DTA_CURRENT);
    device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(1,D3DTSS_ALPHAARG1,D3DTA_CURRENT);
    device->SetTextureStageState(1,D3DTSS_TEXCOORDINDEX,1);device->SetTextureStageState(1,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);}
   if(uvCase==11){D3DLIGHT8 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Direction.z=-1;light.Diffuse={.7f,.6f,.5f,1};
    device->SetLight(0,&light);device->LightEnable(0,TRUE);device->SetRenderState(D3DRS_LIGHTING,TRUE);device->SetRenderState(D3DRS_AMBIENT,0xff202020);}
   if(materialColors){D3DLIGHT8 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Direction.z=-1;
    light.Diffuse={.7f,.6f,.5f,1};light.Specular={.25f,.35f,.45f,1};
    device->SetLight(0,&light);device->LightEnable(0,TRUE);device->SetRenderState(D3DRS_LIGHTING,TRUE);device->SetRenderState(D3DRS_AMBIENT,0xff202020);
   }
   Require(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xff000000,1,0)),"shared captured fresh GPU frame");
   Require(SUCCEEDED(device->BeginScene()),"shared captured begin");
   for(unsigned draw=0;draw<5;++draw){
    auto world=identity;if(uvCase==11){world._11=draw==4?-.9f:.9f+.01f*draw;world._22=1.1f;world._12=.06f*draw;world._13=.1f;}world._41=.05f*draw;world._43=-.015f*draw;device->SetTransform(D3DTS_WORLD,&world);
    if(materialColors){auto varied=material;
     varied.Diffuse={.2f+.13f*draw,.8f-.11f*draw,.3f+.09f*draw,alpha?.2f+.13f*draw:1.0f};
     varied.Ambient={.15f+.1f*draw,.8f-.08f*draw,.45f,1};varied.Specular={.7f-.1f*draw,.15f+.1f*draw,.3f,1};
     device->SetMaterial(&varied);
    }
    ScopedNativeMeshSource scoped(source);long result=E_FAIL;
    Require(nativeMeshDispatch.Try(device,NativeIndexedDraw{D3DPT_TRIANGLELIST,0,count-base,2+draw*3,1},result),"actual palette high base/R32 dispatch");
    Require(SUCCEEDED(result),"shared captured high-index draw");
   }
   Require(SUCCEEDED(device->EndScene()),"shared captured end");
   if(mode&&materialColors)Require(sortedColorPaletteCounts.materialSavedDraws>beforePalette.materialSavedDraws,"different material colors actually merged on GPU");
   if(mode){Require(sortedColorPaletteCounts.savedDraws>beforePalette.savedDraws&&sortedColorPaletteCounts.builds>beforePalette.builds,"actual specialized palette VS drew merged ranges");Require(sortedColorPaletteCounts.gpuFailures==beforePalette.gpuFailures,"palette GPU upload succeeded");}
   ComPtr<IDirect3DSurface8> image;Require(SUCCEEDED(device->CreateImageSurface(64,64,D3DFMT_A8R8G8B8,image.GetAddressOf()))&&SUCCEEDED(device->GetFrontBuffer(image.Get())),"shared captured GPU readback");
   D3DLOCKED_RECT locked{};Require(SUCCEEDED(image->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"shared GPU pixels");std::vector<unsigned> pixels(4096);
   for(unsigned y=0;y<64;++y)std::memcpy(pixels.data()+y*64,static_cast<const BYTE*>(locked.pBits)+y*locked.Pitch,64*4);
   image->UnlockRect();unsigned colored=0;for(auto pixel:pixels)colored+=(pixel&0xffffff)!=0;
   Require(colored>500,"shared captured GPU reference nonempty");
   if(!mode)reference=std::move(pixels);else{Require(reference==pixels,"shared VB/IB GPU differs from sparse compatibility reference with base/revisions");++comparisons;}
   Require(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)),"shared captured hidden present");
  }
 }
 device->SetTexture(0,nullptr);device->SetTexture(1,nullptr);device->LightEnable(0,FALSE);device->SetRenderState(D3DRS_LIGHTING,FALSE);
 device->SetStreamSource(0,nullptr,0);device->SetIndices(nullptr,0);
 return comparisons;
}

static unsigned CheckCapturedSortedBuffers(IDirect3DDevice8 *device){
 using namespace GenZH::Modern;
 constexpr UINT count=70018,base=13,stride=44;
 const DWORD fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2;
 ComPtr<IDirect3DVertexBuffer8> vb;ComPtr<IDirect3DIndexBuffer8> ib;
 Require(SUCCEEDED(device->CreateVertexBuffer(count*stride,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,vb.GetAddressOf())),"shared sorted GPU VB allocation");
 Require(SUCCEEDED(device->CreateIndexBuffer(8*4,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,D3DFMT_INDEX32,D3DPOOL_DEFAULT,ib.GetAddressOf())),"shared sorted GPU R32 IB allocation");
 NativeMeshSource source;source.root=201;source.mesh=202;source.resource=203;source.drawable=3;source.sorted=true;
 D3DMATRIX identity{};identity._11=identity._22=identity._33=identity._44=1;
 D3DMATERIAL8 material{};material.Diffuse=material.Ambient={1,1,1,1};
 ComPtr<IDirect3DTexture8> checker;
 Require(SUCCEEDED(device->CreateTexture(16,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,checker.GetAddressOf())),"two-UV checker texture");
 D3DLOCKED_RECT checkerLock{};Require(SUCCEEDED(checker->LockRect(0,&checkerLock,nullptr,0)),"two-UV checker pixels");
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){const DWORD color=0xff000000u|((32+x*13)<<16)|((40+y*11)<<8)|(90+(x+y)%8*20);
  std::memcpy(static_cast<BYTE*>(checkerLock.pBits)+y*checkerLock.Pitch+x*4,&color,4);}
 Require(SUCCEEDED(checker->UnlockRect(0)),"two-UV checker commit");
 unsigned comparisons=0;
 for(unsigned alpha=0;alpha<2;++alpha)for(unsigned uvCase=0;uvCase<12;++uvCase){std::vector<unsigned> reference;
  for(unsigned mode=0;mode<2;++mode){
   ScopedSortedSubmitReference legacy(!mode);
   BYTE *bytes=nullptr;
   Require(SUCCEEDED(vb->Lock(0,0,&bytes,D3DLOCK_DISCARD)),"capture fresh VB revision");
   std::memset(bytes,0,count*stride);
   const UINT vertices[]={70017,65549,16};const float positions[][3]={{-.6f,-.5f,.4f},{.65f,-.45f,.4f},{-.05f,.65f,.4f}};
   for(unsigned i=0;i<3;++i){auto *v=bytes+vertices[i]*stride;std::memcpy(v,positions[i],12);const float normal[]={0,0,1};std::memcpy(v+12,normal,12);
    const DWORD color=(alpha?0x80:0xff)*0x01000000u|(i==0?0xe04030:(i==1?0x30d060:0x4070e0));std::memcpy(v+24,&color,4);
    const float uv[]={float(i)*.3f,.2f,float(i)*.2f,.7f};std::memcpy(v+28,uv,16);}
   Require(SUCCEEDED(vb->Unlock()),"commit VB revision");
   Require(SUCCEEDED(ib->Lock(0,0,&bytes,D3DLOCK_DISCARD)),"capture fresh IB revision");
   const UINT raw[]={UINT_MAX,UINT_MAX,70004,65536,3,3,65536,70004};std::memcpy(bytes,raw,sizeof(raw));
   Require(SUCCEEDED(ib->Unlock()),"commit IB revision");
   Require(SUCCEEDED(device->SetStreamSource(0,vb.Get(),stride))&&SUCCEEDED(device->SetIndices(ib.Get(),base)),"shared GPU offset bindings");
   Require(SUCCEEDED(device->SetVertexShader(fvf)),"shared fixed FVF");
   device->SetPixelShader(0);device->SetMaterial(&material);device->SetTransform(D3DTS_VIEW,&identity);device->SetTransform(D3DTS_PROJECTION,&identity);
   D3DVIEWPORT8 viewport{0,0,64,64,0,1};device->SetViewport(&viewport);
   device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_FOGENABLE,FALSE);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
   device->SetRenderState(D3DRS_ZENABLE,TRUE);device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);device->SetRenderState(D3DRS_ZWRITEENABLE,alpha?FALSE:TRUE);
   device->SetRenderState(D3DRS_ALPHABLENDENABLE,alpha?TRUE:FALSE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
   device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);device->SetRenderState(D3DRS_STENCILENABLE,FALSE);device->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
   for(unsigned stage=0;stage<8;++stage){device->SetTexture(stage,nullptr);device->SetTextureStageState(stage,D3DTSS_COLOROP,stage?D3DTOP_DISABLE:D3DTOP_SELECTARG1);device->SetTextureStageState(stage,D3DTSS_COLORARG1,D3DTA_DIFFUSE);device->SetTextureStageState(stage,D3DTSS_ALPHAOP,stage?D3DTOP_DISABLE:D3DTOP_SELECTARG1);device->SetTextureStageState(stage,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);}
   if(uvCase){
    device->SetTexture(0,checker.Get());device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE);
    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
   }
   device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,uvCase>=1&&uvCase<=8?uvCase-1:0);
   auto textureMatrix=identity;textureMatrix._11=.7f;textureMatrix._22=.8f;textureMatrix._41=.15f;textureMatrix._42=.2f;textureMatrix._44=.65f;
   device->SetTransform(D3DTS_TEXTURE0,&textureMatrix);
   device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,uvCase==9?D3DTTFF_COUNT3|D3DTTFF_PROJECTED:D3DTTFF_DISABLE);
   if(uvCase==10){device->SetTexture(1,checker.Get());device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_ADD);
    device->SetTextureStageState(1,D3DTSS_COLORARG1,D3DTA_TEXTURE);device->SetTextureStageState(1,D3DTSS_COLORARG2,D3DTA_CURRENT);
    device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(1,D3DTSS_ALPHAARG1,D3DTA_CURRENT);
    device->SetTextureStageState(1,D3DTSS_TEXCOORDINDEX,1);device->SetTextureStageState(1,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);}
   if(uvCase==11){D3DLIGHT8 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Direction.z=-1;light.Diffuse={.7f,.6f,.5f,1};
    device->SetLight(0,&light);device->LightEnable(0,TRUE);device->SetRenderState(D3DRS_LIGHTING,TRUE);device->SetRenderState(D3DRS_AMBIENT,0xff202020);}
   Require(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xff000000,1,0)),"shared captured fresh GPU frame");
   Require(SUCCEEDED(device->BeginScene()),"shared captured begin");
   for(unsigned draw=0;draw<5;++draw){
    if(draw==1){Require(SUCCEEDED(vb->Lock(vertices[0]*stride,4,&bytes,D3DLOCK_NOOVERWRITE)),"rewrite VB after prior packet queued");const float x=-.4f;std::memcpy(bytes,&x,4);Require(SUCCEEDED(vb->Unlock()),"commit rewritten VB");}
    if(draw==3||draw==4){Require(SUCCEEDED(ib->Lock(2*4,3*4,&bytes,D3DLOCK_NOOVERWRITE)),"rewrite IB after prior packet queued");const UINT changed[]={3,65536,draw==3?65536u:70004u};std::memcpy(bytes,changed,12);Require(SUCCEEDED(ib->Unlock()),"commit rewritten IB");}
    auto world=identity;if(uvCase==11){world._11=.9f;world._22=1.1f;world._12=.06f;world._13=.1f;}world._41=.05f*draw;world._43=-.015f*draw;device->SetTransform(D3DTS_WORLD,&world);
    ScopedNativeMeshSource scoped(source);long result=E_FAIL;
    if(mode)Require(nativeMeshDispatch.Try(device,NativeIndexedDraw{D3DPT_TRIANGLELIST,0,count-base,2,1},result),"actual mirrored sorted buffer dispatch");
    else result=device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,count-base,2,1);
    Require(SUCCEEDED(result),"shared captured high-index draw");
   }
   Require(SUCCEEDED(device->EndScene()),"shared captured end");
   ComPtr<IDirect3DSurface8> image;Require(SUCCEEDED(device->CreateImageSurface(64,64,D3DFMT_A8R8G8B8,image.GetAddressOf()))&&SUCCEEDED(device->GetFrontBuffer(image.Get())),"shared captured GPU readback");
   D3DLOCKED_RECT locked{};Require(SUCCEEDED(image->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"shared GPU pixels");std::vector<unsigned> pixels(4096);
   for(unsigned y=0;y<64;++y)std::memcpy(pixels.data()+y*64,static_cast<const BYTE*>(locked.pBits)+y*locked.Pitch,64*4);
   image->UnlockRect();unsigned colored=0;for(auto pixel:pixels)colored+=(pixel&0xffffff)!=0;
   Require(colored>500,"shared captured GPU reference nonempty");
   if(!mode)reference=std::move(pixels);else{Require(reference==pixels,"shared VB/IB GPU differs from sparse compatibility reference with base/revisions");++comparisons;}
   Require(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)),"shared captured hidden present");
  }
 }
 device->SetTexture(0,nullptr);device->SetTexture(1,nullptr);device->LightEnable(0,FALSE);device->SetRenderState(D3DRS_LIGHTING,FALSE);
 device->SetStreamSource(0,nullptr,0);device->SetIndices(nullptr,0);
 return comparisons;
}

// Verify unchanged setters still participate in real native state blocks.
static unsigned CheckNativeStateReuse(IDirect3DDevice8 *device){
 using namespace GenZH::Modern;
 DWORD original=0,recorded=0;Require(SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL,&original)),"save native state for dedup check");
 D3DMATERIAL8 material{};material.Diffuse=material.Ambient={1,1,1,1};material.Power=3;
 D3DMATRIX matrix{};matrix._11=matrix._22=matrix._33=matrix._44=1;
 D3DVIEWPORT8 viewport{0,0,64,64,0,1};D3DLIGHT8 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Direction.z=-1;light.Diffuse={.2f,.3f,.4f,1};light.Attenuation0=1;light.Range=1000;
 const auto set=[&](){
  Require(SUCCEEDED(device->SetMaterial(&material))&&SUCCEEDED(device->SetTransform(D3DTS_WORLD,&matrix))&&SUCCEEDED(device->SetViewport(&viewport)),"fixed native setters");
  Require(SUCCEEDED(device->SetLight(0,&light))&&SUCCEEDED(device->LightEnable(0,TRUE)),"native light setters");
  Require(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,TRUE))&&SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE)),"native render/stage setters");
 };
 set();set();Require(SUCCEEDED(device->BeginStateBlock()),"record native unchanged state");set();Require(SUCCEEDED(device->EndStateBlock(&recorded)),"end native unchanged block");
 auto changedMaterial=material;changedMaterial.Power=9;device->SetMaterial(&changedMaterial);
 auto changedMatrix=matrix;changedMatrix._41=.1f;device->SetTransform(D3DTS_WORLD,&changedMatrix);
 auto changedViewport=viewport;changedViewport.Width=63;device->SetViewport(&changedViewport);
 auto changedLight=light;changedLight.Diffuse.r=.8f;device->SetLight(0,&changedLight);device->LightEnable(0,FALSE);
 device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
 Require(SUCCEEDED(device->ApplyStateBlock(recorded)),"apply unchanged native recording");
 D3DMATERIAL8 m{};D3DMATRIX t{};D3DVIEWPORT8 v{};D3DLIGHT8 l{};BOOL enabled=FALSE;DWORD z=0,color=0;
 Require(SUCCEEDED(device->GetMaterial(&m))&&!std::memcmp(&m,&material,sizeof(m)),"recorded material not skipped");
 Require(SUCCEEDED(device->GetTransform(D3DTS_WORLD,&t))&&!std::memcmp(&t,&matrix,sizeof(t)),"recorded transform not skipped");
 Require(SUCCEEDED(device->GetViewport(&v))&&!std::memcmp(&v,&viewport,sizeof(v)),"recorded viewport not skipped");
 Require(SUCCEEDED(device->GetLight(0,&l))&&!std::memcmp(&l,&light,sizeof(l)),"recorded light not skipped");
 Require(SUCCEEDED(device->GetLightEnable(0,&enabled))&&enabled,"recorded enable not skipped");
 Require(SUCCEEDED(device->GetRenderState(D3DRS_ZENABLE,&z))&&z==TRUE,"recorded render state not skipped");
 Require(SUCCEEDED(device->GetTextureStageState(0,D3DTSS_COLOROP,&color))&&color==D3DTOP_MODULATE,"recorded stage state not skipped");
 Require(SUCCEEDED(device->ApplyStateBlock(original))&&SUCCEEDED(device->DeleteStateBlock(original))&&SUCCEEDED(device->DeleteStateBlock(recorded)),"restore state after dedup check");
 ComPtr<IDirect3DTexture8> texture;Require(SUCCEEDED(device->CreateTexture(16,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.GetAddressOf())),"live named native texture");
 for(const char *name:{"reuse_A.tga","reuse_A.tga","reuse_B.tga","reuse_A.tga"}){
  SetD3D11TextureName(texture.Get(),name);char actual[128]{};DWORD bytes=sizeof(actual);
  Require(SUCCEEDED(texture->GetPrivateData(D3D11TextureNameGUID,actual,&bytes))&&bytes==std::strlen(name)+1&&!std::strcmp(actual,name),"live name write/repeat/rename equality");
  if(!std::strcmp(name,"reuse_B.tga"))Require(SUCCEEDED(texture->FreePrivateData(D3D11TextureNameGUID)),"external name removal observed");
 }

 // Native API barriers must see the exact latest pending values, including
 // Create/Capture/Apply blocks and MultiplyTransform. A getter alone is not
 // sufficient coverage: state-block APIs read the raw native device directly.
 D3DMATRIX wa=matrix,wb=matrix;wa._41=.12f;wb._42=.15f;
 D3DMATERIAL8 ma=material,mb=material;ma.Power=7;mb.Power=11;
 for(const auto *w:{&wa,&wb})Require(SUCCEEDED(device->SetTransform(D3DTS_WORLD,w)),"warm native transform validation");
 for(const auto *m:{&ma,&mb})Require(SUCCEEDED(device->SetMaterial(m)),"warm native material validation");
 unsigned barriers=0;
 {ScopedNativeFixedWrites pending(device);
  Require(SUCCEEDED(device->SetTransform(D3DTS_WORLD,&wa))&&SUCCEEDED(device->SetMaterial(&ma)),"deferred state before getter");
  D3DMATRIX actual{};D3DMATERIAL8 actualMaterial{};
  Require(SUCCEEDED(device->GetTransform(D3DTS_WORLD,&actual))&&!std::memcmp(&actual,&wa,sizeof(wa)),"pending transform getter synchronizes");++barriers;
  Require(SUCCEEDED(device->GetMaterial(&actualMaterial))&&actualMaterial.Power==ma.Power,"pending material getter synchronizes");++barriers;
  DWORD capture=0,block=0;
  Require(SUCCEEDED(device->SetTransform(D3DTS_WORLD,&wb))&&SUCCEEDED(device->SetMaterial(&mb)),"pending before CreateStateBlock");
  Require(SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL,&capture)),"create captures pending native state");
  device->SetTransform(D3DTS_WORLD,&wa);device->SetMaterial(&ma);
  Require(SUCCEEDED(device->ApplyStateBlock(capture))&&SUCCEEDED(device->GetMaterial(&actualMaterial))&&actualMaterial.Power==mb.Power,"Create/Apply state block preserves pending material");++barriers;
  Require(SUCCEEDED(device->GetTransform(D3DTS_WORLD,&actual))&&!std::memcmp(&actual,&wb,sizeof(wb)),"Create/Apply state block preserves pending world");++barriers;
  device->SetTransform(D3DTS_WORLD,&wa);device->SetMaterial(&ma);
  Require(SUCCEEDED(device->CaptureStateBlock(capture)),"capture updates block after pending changes");
  device->SetTransform(D3DTS_WORLD,&wb);device->SetMaterial(&mb);
  Require(SUCCEEDED(device->ApplyStateBlock(capture))&&SUCCEEDED(device->GetMaterial(&actualMaterial))&&actualMaterial.Power==ma.Power,"Capture/Apply exact material");++barriers;
  Require(SUCCEEDED(device->GetTransform(D3DTS_WORLD,&actual))&&!std::memcmp(&actual,&wa,sizeof(wa)),"Capture/Apply exact transform");++barriers;
  device->SetMaterial(&mb);device->SetTransform(D3DTS_WORLD,&wb);
  Require(SUCCEEDED(device->BeginStateBlock()),"begin block synchronizes prior pending writes");
  device->SetMaterial(&ma);device->SetTransform(D3DTS_WORLD,&wa);
  Require(SUCCEEDED(device->EndStateBlock(&block)),"end recorded fixed state");
  device->SetMaterial(&mb);device->SetTransform(D3DTS_WORLD,&wb);
  Require(SUCCEEDED(device->ApplyStateBlock(block))&&SUCCEEDED(device->GetMaterial(&actualMaterial))&&actualMaterial.Power==ma.Power,"recording forwards exact material");++barriers;
  Require(SUCCEEDED(device->GetTransform(D3DTS_WORLD,&actual))&&!std::memcmp(&actual,&wa,sizeof(wa)),"recording forwards exact transform");++barriers;
  Require(SUCCEEDED(device->DeleteStateBlock(capture))&&SUCCEEDED(device->DeleteStateBlock(block)),"dispose barrier blocks");
 }

 {ScopedNativeFixedWrites pending(device);
  Require(SUCCEEDED(device->SetTransform(D3DTS_WORLD,&wb)),"pending before MultiplyTransform");
  Require(SUCCEEDED(device->MultiplyTransform(D3DTS_WORLD,&wa)),"multiply synchronizes pending transform");
  auto expected=wb;expected._41=wa._41;D3DMATRIX actual{};
  Require(SUCCEEDED(device->GetTransform(D3DTS_WORLD,&actual))&&!std::memcmp(&actual,&expected,sizeof(actual)),"multiply uses latest world, not stale native state");++barriers;
 }
 // Restore the caller's state once more after the additional barrier checks.
 Require(SUCCEEDED(device->SetTransform(D3DTS_WORLD,&matrix))&&SUCCEEDED(device->SetMaterial(&material)),"barrier fixture restores world and material");
 return 11+barriers;
}

// Interleaved triangles from three actual sorter nodes, one common upload.
// Baseline uses the same native dispatch and original sorted sequence; only
// the adjacent state batching switch differs. Read color and stencil probes.
static unsigned CheckOrderedSortBatches(IDirect3DDevice8 *device,bool large=false){
 using namespace GenZH::Modern;
 const unsigned first=70002,nodes=3,triangles=large?96:4,nodeVertices=triangles*3,totalIndices=nodeVertices*nodes,count=first+totalIndices+2;
 auto *vb=NEW_REF(SortingVertexBufferClass,(count));auto *ib=NEW_REF(SortingIndexBufferClass,(totalIndices));
 {VertexBufferClass::WriteLockClass lock(vb);auto *v=static_cast<VertexFormatXYZNDUV2*>(lock.Get_Vertex_Array());
  std::memset(v,0,sizeof(*v)*count);
  for(unsigned node=0;node<nodes;++node)for(unsigned triangle=0;triangle<triangles;++triangle){
   const float z=.2f+(large?.55f/static_cast<float>(nodes*triangles-1):.045f)*(triangle*nodes+node);const unsigned offset=first+node*nodeVertices+triangle*3;
   const float xy[][2]={{-.75f,-.7f},{.7f,-.6f},{-.05f,.75f}};
   for(unsigned i=0;i<3;++i){auto &p=v[offset+i];p.x=xy[i][0];p.y=xy[i][1];p.z=z;p.nz=1;
    p.diffuse=0x65000000u|(node==0?0xe04030:(node==1?0x30d060:0x4070e0));p.u1=.2f*i;p.v1=.3f;}
  }
 }
 {IndexBufferClass::WriteLockClass lock(ib);auto *out=lock.Get_Index_Array();for(unsigned i=0;i<totalIndices;++i)out[i]=first+i;}
 auto *material=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
 auto *other=NEW_REF(VertexMaterialClass,());other->Set_Lighting(false);other->Set_Opacity(.7f);
 auto *mapped=NEW_REF(VertexMaterialClass,());mapped->Set_Lighting(false);
 auto *mapper=NEW_REF(ScaleTextureMapperClass,(Vector2(1,1),0));mapped->Set_Mapper(mapper);mapper->Release_Ref();
 Matrix4x4 identity;identity.Make_Identity();
 NativeMeshSource source;source.root=600;source.parent=601;source.mesh=602;source.resource=603;source.drawable=77;source.sorted=true;
 source.sortedVertexCount=totalIndices;
 // Appearance is a shader input; diagnostic owner IDs are not. Evaluated
 // world/view and full material/light values are checked separately below.
 unsigned gates=0;
 Require(SameOrderedSortAppearance(source,source),"same sorted appearance accepted");
 for(unsigned gate=0;gate<9;++gate){auto b=source;
  if(gate==0)b.root=0;if(gate==1)b.mesh=0;if(gate==2)b.resource=0;if(gate==3)b.skinned=true;
  if(gate==4)b.sorted=false;if(gate==5)b.pass=UINT32_MAX;if(gate==6)++b.sortedVertexCount;
  if(gate==7)b.vehicleWear.age[0]=.4f;if(gate==8)b.vehicleWear.profile[0]=1;
  Require(!SameOrderedSortAppearance(source,b),"unsafe source/wear must remain separate");++gates;
 }
 auto *clone=material->Clone();
 ShaderClass shader;shader.Set_Texturing(ShaderClass::TEXTURING_DISABLE);shader.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
 shader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);shader.Set_Src_Blend_Func(ShaderClass::SRCBLEND_SRC_ALPHA);
 shader.Set_Dst_Blend_Func(ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA);
 unsigned comparisons=0,merged=0;
 for(unsigned variant=0;variant<14;++variant)for(unsigned stencil=0;stencil<2;++stencil)for(unsigned probe=0;probe<2;++probe){
  std::vector<unsigned> reference;
  for(unsigned mode=0;mode<2;++mode){
   ScopedOrderedSortReference original(!mode);
   ScopedSceneConstantReference constantReference(!mode);
   ScopedDeferredFixedReference fixedReference(!mode);
   ScopedSceneBindingsReference bindingsReference(!mode);
   ScopedResolvedStatesReference resolvedReference(!mode);
   ScopedSortedColorPaletteReference paletteReference(!mode);
   DX8Wrapper::Set_Transform(D3DTS_WORLD,identity);DX8Wrapper::Set_Transform(D3DTS_VIEW,identity);DX8Wrapper::Set_Transform(D3DTS_PROJECTION,identity);
   DX8Wrapper::Set_Shader(shader);DX8Wrapper::Set_Material(material);
   for(unsigned stage=0;stage<8;++stage)DX8Wrapper::Set_Texture(stage,nullptr);
   DX8Wrapper::Apply_Render_State_Changes(true);
   DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE,TRUE);DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE,FALSE);
   DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE,stencil);DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,D3DCMP_ALWAYS);
   DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
   DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,D3DSTENCILOP_INCR);DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK,255);
   DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK,255);DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,15);
   Require(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xff000000,1,0)),"fresh ordered batch frame");
   const auto before=orderedSortCounts;DX8Wrapper::Begin_Scene();
   MeshModelClass model;
   for(unsigned node=0;node<nodes;++node){
    DX8Wrapper::Set_Vertex_Buffer(vb);DX8Wrapper::Set_Index_Buffer(ib,0);
    auto native=source;native.mesh+=node;native.resource+=node;
    if(variant==1&&node==1)native.vehicleWear.age[0]=.6f;
    if(variant==9){native.root+=node;native.parent+=node;native.drawable+=node;native.pass+=node;native.lod+=node;native.bone+=node;}
    auto world=identity,view=identity;if(variant==2&&node==1)world[0][3]=.1f;
    if(variant==3&&node==1)view[1][3]=.07f;
    if(variant==10&&node==1){world[0][0]=.8f;world[1][1]=1.1f;}
    if(variant==11&&node==1){world[0][0]=.8f;world[0][1]=.6f;world[1][0]=-.6f;world[1][1]=.8f;}
    if(variant==12&&node==1)world[3][0]=.05f;
    if(variant==13&&node==1)world[0][0]=-1;
    DX8Wrapper::Set_Transform(D3DTS_WORLD,world);DX8Wrapper::Set_Transform(D3DTS_VIEW,view);
    DX8Wrapper::Set_Material(variant==4&&node==1?other:(variant==5?mapped:(variant==8&&node==1?clone:material)));
    auto current=shader;if(variant==6&&node==1)current.Set_Dst_Blend_Func(ShaderClass::DSTBLEND_ONE);
    DX8Wrapper::Set_Shader(current);
    D3DLIGHT8 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Diffuse={.2f,.3f,.4f,1};light.Direction.z=-1;
    if(variant==7&&node==1)light.Diffuse.r=.7f;DX8Wrapper::Set_Light(0,&light);
    DX8PolygonRendererClass renderer(nodeVertices,&model,nullptr,0,node*nodeVertices,false,0);renderer.Set_Vertex_Index_Range(first+node*nodeVertices,nodeVertices);
    ScopedNativeMeshSource scoped(native);renderer.Render_Sorted(0,SphereClass(Vector3(0,0,.5f),2));
   }
   SortingRendererClass::Flush();const auto after=orderedSortCounts;
   const auto originalDraws=after.beforeDraws-before.beforeDraws,actualDraws=after.afterDraws-before.afterDraws;
   Require(originalDraws>=9,"fixture truly interleaves three node index streams");
   if(mode&&(variant==0||variant==2||variant==8||variant==9)){Require(actualDraws==1,"equivalent ordered nodes batch into exactly one real draw");++merged;}
   if(!mode)Require(actualDraws==originalDraws,"reference preserves original fragment draws");
   if(mode&&variant!=0&&variant!=2&&variant!=8&&variant!=9)Require(actualDraws>1,("state/mapper/wear mismatch is not batched away: variant="+std::to_string(variant)).c_str());
   if(probe){
    Require(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0)),"ordered stencil probe retains depth/stencil");
    DX8Wrapper::Set_Transform(D3DTS_WORLD,identity);DX8Wrapper::Set_Transform(D3DTS_VIEW,identity);DX8Wrapper::Set_Material(material);DX8Wrapper::Set_Shader(shader);
    DX8Wrapper::Set_Vertex_Buffer(vb);DX8Wrapper::Set_Index_Buffer(ib,0);DX8Wrapper::Apply_Render_State_Changes(true);
    DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE,FALSE);DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE,TRUE);
    DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,D3DCMP_EQUAL);DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF,(nodes*triangles)&255);
    DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK,0);
    ScopedNativeMeshSource scoped(source);DX8Wrapper::Draw_Triangles(0,1,first,3);
   }
   DX8Wrapper::End_Scene(false);
   Microsoft::WRL::ComPtr<IDirect3DSurface8> image;Require(SUCCEEDED(device->CreateImageSurface(64,64,D3DFMT_A8R8G8B8,image.GetAddressOf()))&&SUCCEEDED(device->GetFrontBuffer(image.Get())),"ordered batch GPU pixels");
   D3DLOCKED_RECT locked{};Require(SUCCEEDED(image->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"ordered batch readback");
   std::vector<unsigned> pixels(4096);for(unsigned y=0;y<64;++y)std::memcpy(pixels.data()+y*64,static_cast<const BYTE*>(locked.pBits)+y*locked.Pitch,256);image->UnlockRect();
   unsigned colored=0;for(auto pixel:pixels)colored+=(pixel&0xffffff)!=0;
   if(!mode){reference=pixels;if(!probe||(stencil&&variant==0))Require(colored>300,"ordered color/stencil reference must be nonempty");}
   else{if(reference!=pixels){unsigned diff=0,first=0;for(unsigned n=0;n<4096;++n)if(reference[n]!=pixels[n]){if(!diff)first=n;++diff;} throw std::runtime_error("palette difference: variant="+std::to_string(variant)+" stencil="+std::to_string(stencil)+" probe="+std::to_string(probe)+" count="+std::to_string(diff)+" first="+std::to_string(first)+" ref="+std::to_string(reference[first])+" actual="+std::to_string(pixels[first])+" paletteDraws="+std::to_string(sortedColorPaletteCounts.draws));}++comparisons;}
   Require(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)),"ordered batch hidden present");
  }
 }
 DX8Wrapper::Set_Vertex_Buffer(nullptr);DX8Wrapper::Set_Index_Buffer(nullptr,0);DX8Wrapper::Set_Material(nullptr);DX8Wrapper::Apply_Render_State_Changes();
 vb->Release_Ref();ib->Release_Ref();material->Release_Ref();other->Release_Ref();mapped->Release_Ref();clone->Release_Ref();
 Require(gates==9&&merged==16,"all identity gates and active batching cases exercised");
 return comparisons;
}

int RunSorterPortChecks(const char *path){
 const std::filesystem::path folder(path);std::filesystem::create_directories(folder);
 const auto trace=[&](const char *stage){std::ofstream(folder/"sorter-check-stages.txt",std::ios::app)<<stage<<'\n';};
 bool initialized=false;
 try{
  SetEnvironmentVariableA("GENZH_USER_DATA_DIR",folder.string().c_str());
  SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *error)->LONG{
   char root[32768]{};GetEnvironmentVariableA("GENZH_USER_DATA_DIR",root,32768);
   HMODULE module=nullptr;GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
       static_cast<LPCSTR>(error->ExceptionRecord->ExceptionAddress),&module);
   char name[MAX_PATH]{};GetModuleFileNameA(module,name,MAX_PATH);
   std::ofstream(std::filesystem::path(root)/"sorter-exception.txt")<<name<<'\n'
       <<std::hex<<reinterpret_cast<std::uintptr_t>(error->ExceptionRecord->ExceptionAddress)-reinterpret_cast<std::uintptr_t>(module)<<'\n';
   return EXCEPTION_EXECUTE_HANDLER;
  });
  SetEnvironmentVariableA("GENZH_RENDERER","d3d11");SetEnvironmentVariableA("GENZH_NATIVE_GEOMETRY","on");
  SetEnvironmentVariableA("GENZH_NATIVE_SUBMIT","on");SetEnvironmentVariableA("GENZH_SHADOWS","off");
  HWND window=CreateWindowExA(0,"STATIC","WW3D sorter integration file check",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  Require(window!=nullptr,"hidden sorter target");
  WW3DAssetManager assets;
  Require(DX8Wrapper::Init(window,false),"native wrapper init");initialized=true;
  Require(WW3D::Set_Render_Device(0,64,64,32,1,false,false,false)==WW3D_ERROR_OK,"hidden native wrapper device");
  trace("device initialized");
  auto *device=DX8Wrapper::_Get_D3D_Device8();Require(device&&GenZH::Modern::nativeMeshDispatch.GeometryEnabled(),"modern sorter dispatch connected");
  WW3D::Enable_Sorting(true);
  Matrix4x4 identity;identity.Make_Identity();DX8Wrapper::Set_Transform(D3DTS_WORLD,identity);DX8Wrapper::Set_Transform(D3DTS_VIEW,identity);
  DX8Wrapper::Set_Transform(D3DTS_PROJECTION,identity);
  DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE,D3DZB_TRUE);DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE,FALSE);
  DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE,FALSE);DX8Wrapper::Set_DX8_Render_State(D3DRS_LIGHTING,FALSE);
  DX8Wrapper::Set_DX8_Render_State(D3DRS_CLIPPLANEENABLE,0);DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,15);
  D3DVIEWPORT8 viewport{0,0,64,64,0,1};DX8Wrapper::Set_Viewport(&viewport);
  ShaderClass shader;shader.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);shader.Set_Texturing(ShaderClass::TEXTURING_DISABLE);
  DX8Wrapper::Set_Shader(shader);
  auto *material=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);DX8Wrapper::Set_Material(material);material->Release_Ref();
  for(unsigned i=0;i<2;++i)DX8Wrapper::Set_Texture(i,nullptr);
  {
   constexpr unsigned count=70005,first=70002;MeshModelClass model;
   std::vector<Vector3> positions(count),normals(count,Vector3(0,0,1));std::vector<Vector2> uv0(count),uv1(count);
   std::vector<unsigned> diffuse(count,0xffff8040u);
   positions[first]=Vector3(-.8f,-.8f,.5f);positions[first+1]=Vector3(.8f,-.8f,.5f);positions[first+2]=Vector3(-.8f,.8f,.5f);
   const std::array<std::uint32_t,3> triangle[]={{first,first+1,first+2}};
   auto recipe=std::make_shared<GenZH::Modern::NativeGeometryRecipe>();recipe->polygons={0};
   GenZH::Modern::NativeGeometryView view;view.recipe=recipe;view.positions=positions.data();view.normals=normals.data();view.triangles=triangle;
   view.uv[0]=uv0.data();view.uv[1]=uv1.data();view.diffuse=diffuse.data();view.fvf=dynamic_fvf_type;
   view.model=17;view.vertices=count;view.triangleCount=1;view.first=first;view.range=3;view.legacyFirstVertex=first;
   auto *vb=NEW_REF(SortingVertexBufferClass,(count));auto *ib=NEW_REF(SortingIndexBufferClass,(3));
   {VertexBufferClass::WriteLockClass lock(vb);auto *out=static_cast<VertexFormatXYZNDUV2*>(lock.Get_Vertex_Array());
    std::memset(out,0,sizeof(VertexFormatXYZNDUV2)*count);for(unsigned i=0;i<count;++i){out[i].x=positions[i].X;out[i].y=positions[i].Y;out[i].z=positions[i].Z;out[i].nz=1;out[i].diffuse=diffuse[i];}}
   {IndexBufferClass::WriteLockClass lock(ib);auto *out=lock.Get_Index_Array();for(unsigned i=0;i<3;++i)out[i]=triangle[0][i];}
   DX8PolygonRendererClass renderer(3,&model,nullptr,0,0,false,0);renderer.Set_Vertex_Index_Range(first,3);renderer.Set_Native_Geometry(view);
   GenZH::Modern::NativeMeshSource source;source.root=11;source.mesh=12;source.resource=17;source.drawable=1;source.pass=0;

   struct Case {DWORD stencil,compare,mask,func,fail,zfail,pass;};
   const Case cases[]={
    {0,D3DCMP_LESSEQUAL,0xffffffff,D3DCMP_ALWAYS,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP,D3DSTENCILOP_REPLACE},
    {1,D3DCMP_LESS,0xffffffff,D3DCMP_ALWAYS,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP,D3DSTENCILOP_REPLACE},
    {1,D3DCMP_LESSEQUAL,0xffffffff,D3DCMP_ALWAYS,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP,D3DSTENCILOP_REPLACE},
    {1,D3DCMP_LESSEQUAL,0x80,D3DCMP_ALWAYS,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP,D3DSTENCILOP_REPLACE},
    {1,D3DCMP_LESSEQUAL,0,D3DCMP_ALWAYS,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP},
    {1,D3DCMP_LESSEQUAL,0xffffffff,D3DCMP_NEVER,D3DSTENCILOP_REPLACE,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP},
    {1,D3DCMP_LESSEQUAL,0xffffffff,D3DCMP_ALWAYS,D3DSTENCILOP_KEEP,D3DSTENCILOP_KEEP,D3DSTENCILOP_INCR},
    {1,D3DCMP_LESSEQUAL,0xffffffff,D3DCMP_ALWAYS,D3DSTENCILOP_KEEP,D3DSTENCILOP_INCR,D3DSTENCILOP_REPLACE}};
   unsigned pixelComparisons=0,probeColored=0;
   const auto bindMaterial=[](){auto *m=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);DX8Wrapper::Set_Material(m);m->Release_Ref();};
   // Each color or stencil-probe comparison owns a fresh frame. Baseline uses
   // the actual original triangle sorter, not a separate handcrafted draw.
   for(unsigned transparent=0;transparent<2;++transparent)for(unsigned c=0;c<8;++c)for(unsigned probe=0;probe<4;++probe){
    if(transparent){VertexBufferClass::WriteLockClass lock(vb);auto *out=static_cast<VertexFormatXYZNDUV2*>(lock.Get_Vertex_Array());for(unsigned i=0;i<3;++i)out[first+i].diffuse=0x8040a0f0u;}
    std::vector<unsigned> reference;
    for(unsigned mode=0;mode<2;++mode){
     trace(("case="+std::to_string(c)+" probe="+std::to_string(probe)+" mode="+std::to_string(mode)).c_str());
     const auto &test=cases[c];GenZH::Modern::ScopedOpaqueSortReference baseline(!mode);
     GenZH::Modern::ScopedSortedSubmitReference snapshotReference(!mode);
     DX8Wrapper::Set_Vertex_Buffer(vb);DX8Wrapper::Set_Index_Buffer(ib,0);
     bindMaterial();
     auto caseShader=shader;caseShader.Set_Depth_Compare(static_cast<ShaderClass::DepthCompareType>(test.compare-1));
     if(transparent){caseShader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);caseShader.Set_Src_Blend_Func(ShaderClass::SRCBLEND_SRC_ALPHA);caseShader.Set_Dst_Blend_Func(ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA);}
     DX8Wrapper::Set_Shader(caseShader);DX8Wrapper::Apply_Render_State_Changes(true);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE,D3DZB_TRUE);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,transparent?FALSE:TRUE);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,test.compare);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE,test.stencil);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,test.func);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,test.fail);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL,test.zfail);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,test.pass);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK,0xffffffff);
     DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK,test.mask);
     Require(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xff000000,1,0x18)),"fresh depth/stencil fixture");
     DX8Wrapper::Begin_Scene();
     for(unsigned draw=0;draw<3;++draw){
      // The original Flush binds its temporary sorting buffers. A subsequent
      // category must restore the source streams, as the real mesh renderer does.
      DX8Wrapper::Set_Vertex_Buffer(vb);DX8Wrapper::Set_Index_Buffer(ib,0);
      bindMaterial();
      Matrix4x4 world=identity;world[0][3]=draw==1?.3f:(draw==2?-.25f:0);
      // A far overlap fails depth, a near overlap wins; the last strict-depth
      // duplicate must not change ownership. All categories keep their order.
      world[2][3]=draw==1?.1f:(draw==2?-.1f:0);
      DX8Wrapper::Set_Transform(D3DTS_WORLD,world);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF,draw==0?8:(draw==1?16:0xff));
      {GenZH::Modern::ScopedNativeMeshSource scoped(source);renderer.Render_Sorted(0,SphereClass(Vector3(0,0,.5f),1));}
      SortingRendererClass::Flush();
      trace("mark and flush done");
     }
     if(probe){
      // Force queued color/depth/stencil to the GPU, clear only color, then
      // visualize the retained stencil bits with the original pixel path.
      Require(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0)),"probe retains depth and stencil");
      trace("probe clear done");
      DX8Wrapper::Set_Transform(D3DTS_WORLD,identity);
      DX8Wrapper::Set_Vertex_Buffer(vb);DX8Wrapper::Set_Index_Buffer(ib,0);
      bindMaterial();
      auto probeShader=shader;probeShader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);
      DX8Wrapper::Set_Shader(probeShader);DX8Wrapper::Apply_Render_State_Changes(true);
      trace("probe shader done");
      DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE,FALSE);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE,TRUE);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,D3DCMP_EQUAL);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK,0);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK,probe==3?0x80:0xff);
      DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF,probe==1?8:(probe==2?0x18:0x80));
      {GenZH::Modern::ScopedNativeMeshSource scoped(source);renderer.Render(0);}
      trace("probe draw done");
     }
     DX8Wrapper::End_Scene(false);
     trace("end scene done");
     IDirect3DSurface8 *image=nullptr;Require(SUCCEEDED(device->CreateImageSurface(64,64,D3DFMT_A8R8G8B8,&image)),"sorter readback image");
     Require(SUCCEEDED(device->GetFrontBuffer(image)),"actual sorter GPU readback");D3DLOCKED_RECT locked{};Require(SUCCEEDED(image->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"sorter GPU pixels");
     std::vector<unsigned> pixels(64*64);for(unsigned y=0;y<64;++y)std::memcpy(pixels.data()+y*64,static_cast<const unsigned char*>(locked.pBits)+y*locked.Pitch,64*4);
     image->UnlockRect();image->Release();
     unsigned colored=0;for(auto p:pixels)colored+=(p&0xffffff)!=0;
     if(!mode){reference=std::move(pixels);if(c==0&&!probe)Require(colored>500,("fixture draws visible pixels: "+std::to_string(colored)).c_str());}
     else{Require(pixels==reference,("real sorter/native pixels differ: case="+std::to_string(c)+" probe="+std::to_string(probe)).c_str());++pixelComparisons;if(probe)probeColored+=colored;}
     Require(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)),"complete hidden frame");
     trace("present done");
    }
   }
   Require(pixelComparisons==64&&probeColored>500,"stencil comparisons include visible nonempty probes");
   trace("64 original-sorter versus native opaque/transparent color and stencil comparisons passed");
   DX8Wrapper::Set_Vertex_Buffer(nullptr);DX8Wrapper::Set_Index_Buffer(nullptr,0);DX8Wrapper::Apply_Render_State_Changes();
   vb->Release_Ref();ib->Release_Ref();
  }
  trace("fixture resources destroyed");
  const auto bindingsBefore=GenZH::Modern::sceneBindingsSkipped;
  const auto resolvedBefore=GenZH::Modern::resolvedStateHits;
  const auto deferredBefore=GenZH::Modern::deferredFixedWritesObserved;
  const auto orderedBatchComparisons=CheckOrderedSortBatches(device);
  Require(GenZH::Modern::sceneConstantCounts.uploads>0&&GenZH::Modern::sceneConstantCounts.binds>0&&GenZH::Modern::sceneConstantCounts.slices>=GenZH::Modern::sceneConstantCounts.binds&&GenZH::Modern::sceneConstantCounts.failures==0,"scene constant arena actually uploads and binds exact per-draw slices");
  Require(orderedBatchComparisons==56,"ordered sorter color/stencil comparisons");
  const auto largeDepthComparisons=CheckOrderedSortBatches(device,true);
  Require(largeDepthComparisons==56&&GenZH::Modern::nativeDepthRadixBatches>0,"real large sorter pool exercises radix and uint32 on GPU");
  const auto paletteGPUComparisons=CheckSortedPaletteGPU(device);
  const auto materialPaletteGPUComparisons=CheckSortedPaletteGPU(device,true);
  Require(materialPaletteGPUComparisons==24,"GPU material palette color/alpha/UV/normal cases");
  Require(paletteGPUComparisons==24,"actual world/normal palette high-base R32 and UV/alpha comparisons");
  const auto sharedBufferComparisons=CheckCapturedSortedBuffers(device);
  const auto nativeStateReuseChecks=CheckNativeStateReuse(device);
  Require(sharedBufferComparisons==24,"opaque/alpha captured sorted buffer GPU comparisons");
  trace("shared VB/IB offset/base/queued revision GPU comparisons passed");
  DX8Wrapper::Shutdown();initialized=false;
  trace("wrapper shutdown");
  const auto terrainMaskChecks=GenZH::Modern::TerrainExclusionCheckAccess::Run();
  const auto constantArenaChecks=GenZH::Modern::SceneConstantArenaCheckAccess::Run();
  Require(constantArenaChecks>=200,"constant arena GPU/range/fallback coverage");
  Require(terrainMaskChecks>40,"terrain resource lifetime and GPU pixel checks");
  // Close fewer than 120 samples only after they are already visible on disk.
  const auto telemetryFolder=folder/"frame-checks";std::filesystem::create_directories(telemetryFolder);
  SetEnvironmentVariableA("GENZH_USER_DATA_DIR",telemetryFolder.string().c_str());
  {GenZH::Modern::PresentedFrameTelemetry telemetry;auto tick=std::chrono::steady_clock::now();
   for(unsigned i=0;i<400;++i)telemetry.Add(2,.1,tick+std::chrono::milliseconds(i*10));
   telemetry.Add(4500,.1,tick+std::chrono::seconds(9));Require(telemetry.Pending()==0,"slow frame flushed without waiting for 120 samples or shutdown");
   GenZHLoad::AppendQueue::Instance().Drain(); // perf91: flushed rows are written by the telemetry thread
   std::ifstream file(telemetryFolder/"renderer-frame-v21.csv");std::string line;unsigned lines=0;while(std::getline(file,line))++lines;
   Require(lines==402,"all 401 samples, including beyond 360, persisted before teardown");}
  Require(GenZH::Modern::sceneBindingsSkipped>bindingsBefore,"actual sorted GPU draws exercise scene binding cache");
  Require(GenZH::Modern::resolvedStateHits>resolvedBefore,"actual sorted GPU draws exercise resolved immutable states");
  Require(GenZH::Modern::deferredFixedWritesObserved>deferredBefore,"real final EXE sorted GPU draws exercised deferred fixed writes");
  std::ofstream(folder/"sorter-port-result.json")<<"{\"passed\":true,\"pointer_bits\":"<<sizeof(void*)*8<<",\"radix_GPU_comparisons\":"<<largeDepthComparisons<<",\"radix_native_batches\":"<<GenZH::Modern::nativeDepthRadixBatches<<",\"actual_WW3D_sorter\":true,\"actual_Render_Sorted_scope\":true,\"opaque_pixel_exact\":true,\"stencil_pixel_exact\":true,\"stencil_cases\":8,\"pixel_comparisons\":64,\"transparent_pixel_exact\":true,\"transparent_comparisons\":32,\"terrain_GPU_checks\":"<<terrainMaskChecks<<",\"sorted_palette_GPU_comparisons\":"<<paletteGPUComparisons<<",\"material_palette_GPU_comparisons\":"<<materialPaletteGPUComparisons<<",\"shared_buffer_GPU_comparisons\":"<<sharedBufferComparisons<<",\"native_state_reuse_checks\":"<<nativeStateReuseChecks<<",\"ordered_batch_GPU_comparisons\":"<<orderedBatchComparisons<<",\"resolved_states_GPU_pixel_exact\":true,\"resolved_states_GPU_comparisons\":56,\"scene_bindings_GPU_pixel_exact\":true,\"scene_bindings_GPU_comparisons\":56,\"deferred_fixed_GPU_pixel_exact\":true,\"deferred_fixed_GPU_comparisons\":56,\"scene_constant_arena_GPU_pixel_exact\":true,\"scene_constant_arena_GPU_comparisons\":56,\"ordered_batch_appearance_gates\":9"<<",\"shared_buffer_GPU_pixel_exact\":true,\"terrain_GPU_pixel_exact\":true,\"original_sorter_reference\":true,\"high_index\":70004,\"timed_live_frame_flush\":true,\"frame_samples\":401,\"game_started\":false,\"visible_window\":false}\n";
  return 0;
 }catch(const std::exception &error){std::ofstream(folder/"sorter-port-error.txt")<<error.what();if(initialized)DX8Wrapper::Shutdown();return 2;}
}

// perf82: the accelerated brute-force mesh ray cast must return bit-identical
// results (hit, Fraction, Normal, SurfaceType, StartBad, ContactPoint) to the
// original loop for real published models and adversarial tie/sliver fixtures.
namespace {
struct RayCastTotals {std::uint64_t files=0,meshes=0,cullTreeMeshes=0,smallMeshes=0,comparedMeshes=0,triangles=0,rays=0,hits=0,contactRays=0;double bruteMs=0,acceleratedMs=0;};
struct RayRecord {bool hit=false;CastResultStruct result;};
std::uint64_t NextRandom(std::uint64_t &state){state^=state<<13;state^=state>>7;state^=state<<17;return state;}
float UnitRandom(std::uint64_t &state){return static_cast<float>(NextRandom(state)>>40)/16777216.0f;}
bool SameRay(const RayRecord &a,const RayRecord &b){
    return a.hit==b.hit&&a.result.StartBad==b.result.StartBad&&std::memcmp(&a.result.Fraction,&b.result.Fraction,sizeof(float))==0&&
        std::memcmp(&a.result.Normal,&b.result.Normal,sizeof(Vector3))==0&&a.result.SurfaceType==b.result.SurfaceType&&
        a.result.ComputeContactPoint==b.result.ComputeContactPoint&&std::memcmp(&a.result.ContactPoint,&b.result.ContactPoint,sizeof(Vector3))==0;
}
Vector3 TrianglePoint(MeshModelClass &mesh,std::uint64_t &state){
    const auto *v=mesh.Get_Vertex_Array();const auto &t=mesh.Get_Polygon_Array()[NextRandom(state)%mesh.Get_Polygon_Count()];
    const Vector3 &a=v[t.I],&b=v[t.J],&c=v[t.K];float u=UnitRandom(state),w=UnitRandom(state);
    const auto kind=NextRandom(state)%8;
    if(kind==0){u=0;w=0;}else if(kind==1){w=0;}else if(kind==2){w=1-u;}else if(u+w>1){u=1-u;w=1-w;}
    return a+(b-a)*u+(c-a)*w;
}
Vector3 RandomDirection(std::uint64_t &state){
    for(;;){Vector3 d(UnitRandom(state)*2-1,UnitRandom(state)*2-1,UnitRandom(state)*2-1);const float l=d.Length();if(l>.05f&&l<=1)return d*(1.0f/l);}
}
std::vector<LineSegClass> BuildRays(MeshModelClass &mesh,std::uint64_t seed,unsigned count){
    std::uint64_t state=seed*0x9E3779B97F4A7C15ull+1;const auto *v=mesh.Get_Vertex_Array();
    Vector3 lo=v[0],hi=v[0];
    for(int i=1;i<mesh.Get_Vertex_Count();++i){lo.X=std::min(lo.X,v[i].X);lo.Y=std::min(lo.Y,v[i].Y);lo.Z=std::min(lo.Z,v[i].Z);hi.X=std::max(hi.X,v[i].X);hi.Y=std::max(hi.Y,v[i].Y);hi.Z=std::max(hi.Z,v[i].Z);}
    const Vector3 span=hi-lo;const float diag=std::max(span.Length(),1.0f);
    auto inside=[&](){return Vector3(lo.X+span.X*UnitRandom(state),lo.Y+span.Y*UnitRandom(state),lo.Z+span.Z*UnitRandom(state));};
    std::vector<LineSegClass> rays;rays.reserve(count);
    for(unsigned i=0;i<count;++i){
        switch(i%10){
        case 0:case 1:case 2:case 3:{const auto q=TrianglePoint(mesh,state);const auto d=RandomDirection(state);rays.emplace_back(q+d*(diag*2+1),q-d*(diag*.5f));break;}
        case 4:case 5:{const auto target=inside();Vector3 eye=target;eye.X+=span.X*(UnitRandom(state)-.5f);eye.Y+=span.Y*(UnitRandom(state)-.5f);eye.Z=hi.Z+diag*10;
            rays.emplace_back(eye,target+(target-eye)*(1.0f+UnitRandom(state)));break;}
        case 6:{auto p0=inside();auto p1=p0;const auto axis=NextRandom(state)%3;p0[axis]=lo[axis]-diag;p1[axis]=hi[axis]+diag;if(NextRandom(state)&1)std::swap(p0,p1);rays.emplace_back(p0,p1);break;}
        case 7:{const auto q=TrianglePoint(mesh,state);rays.emplace_back(q,q);break;}
        case 8:rays.emplace_back(inside(),inside());break;
        default:{const auto q=TrianglePoint(mesh,state);const auto d=RandomDirection(state);rays.emplace_back(q+d*1.0e5f,q-d*1.0e5f);break;}
        }
    }
    return rays;
}
void CompareMeshRays(MeshModelClass &mesh,std::uint64_t seed,RayCastTotals &totals){
    ++totals.meshes;
    if(mesh.Has_Cull_Tree()){++totals.cullTreeMeshes;return;}
    if(mesh.Get_Polygon_Count()<MeshRayCastAccel::MinimumTriangles){++totals.smallMeshes;return;}
    // Bound brute-force work per mesh (~2e7 triangle tests), never fewer than 60 rays.
    const unsigned count=static_cast<unsigned>(std::clamp<long long>(20000000ll/mesh.Get_Polygon_Count(),60,600));
    const auto rays=BuildRays(mesh,seed,count);
    std::vector<RayRecord> brute(rays.size()),fast(rays.size());
    auto run=[&](std::vector<RayRecord> &out,int mode)->double{
        MeshRayCastAccelOverride=mode;const auto start=std::chrono::steady_clock::now();
        for(std::size_t i=0;i<rays.size();++i){out[i].result.ComputeContactPoint=(i&1)!=0;RayCollisionTestClass test(rays[i],&out[i].result);out[i].hit=mesh.Cast_Ray(test);}
        MeshRayCastAccelOverride=-1;return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    totals.bruteMs+=run(brute,0);totals.acceleratedMs+=run(fast,1);
    for(std::size_t i=0;i<rays.size();++i){
        if(!SameRay(brute[i],fast[i])){
            char detail[256];std::snprintf(detail,sizeof(detail),"accelerated mesh ray differs: mesh %s ray %u hit %d/%d fraction %.9g/%.9g surface %u/%u",
                mesh.Get_Name()?mesh.Get_Name():"?",static_cast<unsigned>(i),brute[i].hit,fast[i].hit,brute[i].result.Fraction,fast[i].result.Fraction,brute[i].result.SurfaceType,fast[i].result.SurfaceType);
            throw std::runtime_error(detail);
        }
        totals.hits+=brute[i].hit;totals.contactRays+=brute[i].result.ComputeContactPoint;
    }
    totals.rays+=rays.size();totals.triangles+=mesh.Get_Polygon_Count();++totals.comparedMeshes;
}
// No collision bits, so no AABTree: grid on z=0, exact duplicate triangles with
// other surface types (equal-fraction ties), collinear and sliver triangles, walls.
void RayTieFixture(const std::filesystem::path &folder,RayCastTotals &totals){
    std::vector<Vector3> positions;std::vector<W3dTriStruct> triangles;
    auto vertex=[&](float x,float y,float z){positions.emplace_back(x,y,z);return static_cast<std::uint32_t>(positions.size()-1);};
    auto triangle=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c,unsigned surface){W3dTriStruct t{};t.Vindex[0]=a;t.Vindex[1]=b;t.Vindex[2]=c;t.Attributes=surface;triangles.push_back(t);};
    for(unsigned y=0;y<33;++y)for(unsigned x=0;x<65;++x)vertex(float(x),float(y),0);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x){const auto i=y*65+x;triangle(i,i+1,i+66,1);triangle(i,i+66,i+65,2);}
    for(unsigned i=0;i<700;++i){const auto &t=triangles[(i*37)%4096];triangle(t.Vindex[0],t.Vindex[1],t.Vindex[2],3+i%9);}
    for(unsigned i=0;i<64;++i){const float x=float(i);triangle(vertex(x,40,0),vertex(x+.5f,40,0),vertex(x+1,40,0),20);
        triangle(vertex(x,41,0),vertex(x+1,41,1e-6f),vertex(x+2,41,0),21);triangle(vertex(x,0,0),vertex(x+1,0,0),vertex(x,0,3),22);}
    std::vector<Vector3> normals(positions.size(),Vector3(0,0,1));std::vector<W3dTexCoordStruct> uv(positions.size());
    W3dMeshHeader3Struct header{};header.Version=W3D_MAKE_VERSION(4,2);header.Attributes=0;
    std::strcpy(header.MeshName,"RAYTIES");std::strcpy(header.ContainerName,"PERF82");
    header.NumTris=static_cast<uint32>(triangles.size());header.NumVertices=static_cast<uint32>(positions.size());header.Max.X=66;header.Max.Y=42;header.Max.Z=3;header.SphRadius=80;
    const auto path=folder/"ray-ties.w3d";
    {RawFileClass file(path.string().c_str());Require(file.Open(FileClass::WRITE)!=0,"open ray fixture write");ChunkSaveClass save(&file);
    Require(save.Begin_Chunk(W3D_CHUNK_MESH),"begin ray mesh");
    Chunk(save,W3D_CHUNK_MESH_HEADER3,&header,sizeof(header));
    Chunk(save,W3D_CHUNK_VERTICES,positions.data(),static_cast<unsigned>(positions.size()*sizeof(Vector3)));
    Chunk(save,W3D_CHUNK_VERTEX_NORMALS,normals.data(),static_cast<unsigned>(normals.size()*sizeof(Vector3)));
    Chunk(save,W3D_CHUNK_TRIANGLES,triangles.data(),static_cast<unsigned>(triangles.size()*sizeof(W3dTriStruct)));
    W3dMaterialInfoStruct material{};material.PassCount=1;Chunk(save,W3D_CHUNK_MATERIAL_INFO,&material,sizeof(material));
    Require(save.Begin_Chunk(W3D_CHUNK_MATERIAL_PASS)&&save.Begin_Chunk(W3D_CHUNK_TEXTURE_STAGE),"begin ray fixture pass");
    Chunk(save,W3D_CHUNK_STAGE_TEXCOORDS,uv.data(),static_cast<unsigned>(uv.size()*sizeof(W3dTexCoordStruct)));
    Require(save.End_Chunk()&&save.End_Chunk()&&save.End_Chunk(),"finish ray fixture");}
    MeshModelClass mesh;RawFileClass file(path.string().c_str());Require(file.Open(FileClass::READ)!=0,"open ray fixture");ChunkLoadClass load(&file);
    Require(load.Open_Chunk()&&load.Cur_Chunk_ID()==W3D_CHUNK_MESH&&mesh.Load_W3D(load)==WW3D_ERROR_OK,"load ray fixture");
    Require(!mesh.Has_Cull_Tree()&&mesh.Get_Polygon_Count()==static_cast<int>(triangles.size()),"ray fixture must use the brute-force path");
    // Exact ties: straight down through duplicated triangle interiors, edges and vertices.
    std::vector<LineSegClass> rays;
    for(unsigned i=0;i<700;++i){const auto &t=triangles[(i*37)%4096];const auto &a=positions[t.Vindex[0]],&b=positions[t.Vindex[1]],&c=positions[t.Vindex[2]];
        const Vector3 q=i%3==0?(a+b+c)*(1.0f/3):(i%3==1?(a+b)*.5f:a);rays.emplace_back(q+Vector3(0,0,50),q-Vector3(0,0,5));}
    for(unsigned i=0;i<64;++i){rays.emplace_back(Vector3(i+.25f,40,5),Vector3(i+.25f,40,-5));rays.emplace_back(Vector3(i+1.0f,41,5),Vector3(i+1.0f,41,-5));
        rays.emplace_back(Vector3(i+.3f,-5,1),Vector3(i+.3f,5,1));rays.emplace_back(Vector3(-1,i*.5f,0),Vector3(70,i*.5f,0));}
    std::vector<RayRecord> brute(rays.size()),fast(rays.size());
    for(int mode=0;mode<2;++mode){auto &out=mode?fast:brute;MeshRayCastAccelOverride=mode;
        for(std::size_t i=0;i<rays.size();++i){out[i].result.ComputeContactPoint=(i&1)!=0;RayCollisionTestClass test(rays[i],&out[i].result);out[i].hit=mesh.Cast_Ray(test);}}
    MeshRayCastAccelOverride=-1;
    unsigned tieHits=0;
    for(std::size_t i=0;i<rays.size();++i){Require(SameRay(brute[i],fast[i]),"accelerated tie/sliver fixture ray differs");tieHits+=brute[i].hit;}
    Require(tieHits>=700,"fixture rays must actually hit the duplicated triangles");
    // Equal-fraction duplicates keep the first (lower-index) triangle's surface type.
    Require(brute[0].hit&&(brute[0].result.SurfaceType==1||brute[0].result.SurfaceType==2),"tie keeps the first triangle");
    totals.rays+=rays.size();totals.hits+=tieHits;totals.triangles+=mesh.Get_Polygon_Count();++totals.comparedMeshes;
    CompareMeshRays(mesh,977,totals);
}
}
int RunRayCastChecks(const char *output,const char *models){
    std::filesystem::path folder(output);std::filesystem::create_directories(folder);
    std::filesystem::remove(folder/"ray-cast-result.json");std::filesystem::remove(folder/"ray-cast-error.txt");
    try{
        WW3DAssetManager assets;RayCastTotals totals;
        RayTieFixture(folder,totals);
        std::uint64_t seed=1;
        for(const auto &entry:std::filesystem::directory_iterator(models)){
            if(!entry.is_regular_file())continue;auto extension=entry.path().extension().string();
            for(auto &c:extension)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));if(extension!=".w3d")continue;
            RawFileClass source(entry.path().string().c_str());Require(source.Open(FileClass::READ)!=0,"open model for ray checks");ChunkLoadClass chunks(&source);
            while(chunks.Open_Chunk()){
                if(chunks.Cur_Chunk_ID()==W3D_CHUNK_MESH){MeshModelClass mesh;Require(mesh.Load_W3D(chunks)==WW3D_ERROR_OK,"load model mesh for ray checks");CompareMeshRays(mesh,++seed,totals);}
                chunks.Close_Chunk();
            }
            ++totals.files;
        }
        Require(totals.files>0&&totals.comparedMeshes>1&&totals.hits>0,"ray checks need real high-detail meshes");
        std::ofstream(folder/"ray-cast-result.json")<<"{\"passed\":true,\"pointer_bits\":"<<sizeof(void*)*8<<",\"model_files\":"<<totals.files<<",\"meshes\":"<<totals.meshes
            <<",\"cull_tree_meshes\":"<<totals.cullTreeMeshes<<",\"small_meshes\":"<<totals.smallMeshes<<",\"compared_meshes\":"<<totals.comparedMeshes<<",\"compared_triangles\":"<<totals.triangles
            <<",\"rays\":"<<totals.rays<<",\"hits\":"<<totals.hits<<",\"contact_point_rays\":"<<totals.contactRays<<",\"bit_identical\":true,\"tie_fixture\":true"
            <<",\"brute_force_ms\":"<<totals.bruteMs<<",\"accelerated_ms\":"<<totals.acceleratedMs<<",\"game_started\":false}\n";
        return 0;
    }catch(const std::exception &error){MeshRayCastAccelOverride=-1;std::ofstream(folder/"ray-cast-error.txt")<<error.what();return 2;}
}
