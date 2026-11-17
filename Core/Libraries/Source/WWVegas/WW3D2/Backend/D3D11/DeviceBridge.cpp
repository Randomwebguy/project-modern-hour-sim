// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeviceBridge.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include "CompatibilityRenderer.h"
#include "NativeStateMirror.h"
#include "DeferredFixedWrites.h"
#include "NativeMeshSubmit.h"
#include "NativeSubmitGate.h"
#include "NativeMeshGeometry.h"
#include "NativeSharedGeometry.h"
#include "SortedBufferGeometry.h"
#include "SortedRunTemplates.h"
#include "CopyRectsRepeat.h"
#include "DeferredLegacyBuffer.h"
#include "GeometryMemoryMonitor.h"
#include "GroundBlockRegistry.h"
#include "LoadTelemetry.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <cstdio>
#include <set>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>

const GUID D3D11TextureNameGUID={0x665e2026,0x8d11,0x4f8a,{0x9c,0x34,0x3b,0x2a,0x1c,0x27,0x66,0x18}};
const GUID D3D11TerrainAtlasGUID={0x665e2027,0x8d11,0x4f8a,{0x9c,0x34,0x3b,0x2a,0x1c,0x27,0x66,0x19}};
const GUID D3D11TerrainCellsGUID={0x665e2028,0x8d11,0x4f8a,{0x9c,0x34,0x3b,0x2a,0x1c,0x27,0x66,0x20}};
const GUID D3D11TerrainElevationGUID={0x665e2029,0x8d11,0x4f8a,{0x9c,0x34,0x3b,0x2a,0x1c,0x27,0x66,0x21}};
const GUID D3D11ShroudMappingGUID={0x665e2029,0x8d11,0x4f8a,{0x9c,0x34,0x3b,0x2a,0x1c,0x27,0x66,0x21}};
static std::uint64_t shroudMappingRevision=1;
std::uint64_t GetD3D11ShroudRevision(){return shroudMappingRevision;}
void SetD3D11ShroudMapping(IDirect3DBaseTexture8 *texture,const D3D11ShroudMapping &mapping){
 if(!texture)return;
 D3D11ShroudMapping previous;DWORD bytes=sizeof(previous);
 if(SUCCEEDED(texture->GetPrivateData(D3D11ShroudMappingGUID,&previous,&bytes))&&bytes==sizeof(previous)&&std::memcmp(&previous,&mapping,sizeof(mapping))==0)return;
 if(SUCCEEDED(texture->SetPrivateData(D3D11ShroudMappingGUID,&mapping,sizeof(mapping),0)))++shroudMappingRevision;
}
// Client thread only (W3DDisplay draw and the renderer's EndScene).
static D3D11ShroudVisibility shroudVisibility;
void SetD3D11ShroudVisibility(const std::uint8_t *cells,std::uint32_t cellsX,std::uint32_t cellsY,std::uint8_t border){
 if(!cells||!cellsX||!cellsY||cellsX>4096||cellsY>4096)return;
 const std::size_t count=std::size_t(cellsX)*cellsY;auto &v=shroudVisibility;
 if(v.cellsX==cellsX&&v.cellsY==cellsY&&v.border==border&&v.cells.size()==count&&std::memcmp(v.cells.data(),cells,count)==0)return;
 v.cells.assign(cells,cells+count);v.cellsX=cellsX;v.cellsY=cellsY;v.border=border;++v.revision;
}
const D3D11ShroudVisibility &GetD3D11ShroudVisibility(){return shroudVisibility;}
// TerrainVisual update and draw submission run on the client thread.
static D3D11GroundBlockRegistry groundBlocks;
void SetD3D11GroundBlocks(const std::vector<D3D11GroundBlock> &blocks){groundBlocks.Assign(blocks);}
const std::vector<D3D11GroundBlock> &GetD3D11GroundBlocks(){return groundBlocks.Blocks();}
std::uint64_t GetD3D11GroundBlocksRevision(){return groundBlocks.Revision();}
static D3D11MapWeather mapWeather;
void ResetD3D11MapWeather(){++mapWeather.epoch;mapWeather.snowEnabled=false;GenZHLoad::Record("weather_map_reset",std::to_string(mapWeather.epoch),0);}
void SetD3D11NativeSnow(bool enabled){if(mapWeather.snowEnabled!=enabled){mapWeather.snowEnabled=enabled;GenZHLoad::Record("weather_mission_snow",enabled?"enabled":"disabled",0);}}
const D3D11MapWeather &GetD3D11MapWeather(){return mapWeather;}
static D3D11WaterSurface waterSurface;
void SetD3D11WaterPrograms(std::uint32_t trapezoid,std::uint32_t river,std::uint32_t sea){
 waterSurface.programs[0]=trapezoid;waterSurface.programs[1]=river;waterSurface.programs[2]=sea;
 GenZHLoad::Record("water_programs_v107",std::to_string(trapezoid)+" "+std::to_string(river)+" "+std::to_string(sea),0);
}
void SetD3D11WaterTransparency(float transparentDepth,float minOpacity,bool additive,bool softEdge){
 waterSurface.transparentDepth=transparentDepth;waterSurface.minOpacity=minOpacity;waterSurface.additive=additive;waterSurface.softEdge=softEdge;
}
const D3D11WaterSurface &GetD3D11WaterSurface(){return waterSurface;}
bool IsD3D11WaterProgram(std::uint32_t program){return program&&(program==waterSurface.programs[0]||program==waterSurface.programs[1]||program==waterSurface.programs[2]);}
static std::uint64_t groundOverlayRevision=1;
std::uint64_t GetD3D11GroundOverlayRevision(){return groundOverlayRevision;}
static std::set<std::string> groundOverlayNames={"tbbib","tbredbib"};
static std::string GroundOverlayName(const char *name){
 std::string value=name?name:"";auto slash=value.find_last_of("/\\");if(slash!=std::string::npos)value.erase(0,slash+1);
 auto extension=value.find_last_of('.');if(extension!=std::string::npos)value.resize(extension);
 for(auto &letter:value)letter=static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));return value;
}
void RegisterD3D11GroundOverlayTexture(const char *name){auto value=GroundOverlayName(name);if(!value.empty()&&groundOverlayNames.insert(std::move(value)).second)++groundOverlayRevision;}
bool IsD3D11GroundOverlayTexture(const char *name){return groundOverlayNames.find(GroundOverlayName(name))!=groundOverlayNames.end();}
void SetD3D11TerrainCells(IDirect3DBaseTexture8 *texture,const D3D11TerrainCell *cells,std::uint32_t count){
 if(texture&&cells&&count&&count<=1024*1024)texture->SetPrivateData(D3D11TerrainCellsGUID,cells,count*sizeof(D3D11TerrainCell),0);
}
void SetD3D11TerrainElevation(IDirect3DBaseTexture8 *texture,std::uint32_t cellsX,std::uint32_t cellsY,const float *heights,const std::uint8_t *flips){
 if(!texture||!heights||!flips||!cellsX||!cellsY||cellsX>1024||cellsY>1024)return;
 std::vector<std::uint8_t> blob(D3D11TerrainElevationBytes(cellsX,cellsY));
 D3D11TerrainElevationHeader header;header.cellsX=cellsX;header.cellsY=cellsY;
 std::memcpy(blob.data(),&header,sizeof(header));
 const auto heightBytes=std::size_t(cellsX+1)*(cellsY+1)*sizeof(float);
 std::memcpy(blob.data()+sizeof(header),heights,heightBytes);std::memcpy(blob.data()+sizeof(header)+heightBytes,flips,std::size_t(cellsX)*cellsY);
 texture->SetPrivateData(D3D11TerrainElevationGUID,blob.data(),static_cast<DWORD>(blob.size()),0);
}
void SetD3D11TerrainAtlas(IDirect3DBaseTexture8 *texture,const D3D11TerrainAtlas &atlas){
 if(texture&&atlas.count<=200)texture->SetPrivateData(D3D11TerrainAtlasGUID,&atlas,sizeof(atlas),0);
}
void SetD3D11TextureName(IDirect3DBaseTexture8 *texture,const char *name){
 if(!texture||!name||!*name)return;
 const auto wanted=std::strlen(name)+1;
 // TextureClass::Apply tags repeated sorted submissions. The legacy device
 // allocated/freed private-data storage even when the name was identical.
 // Read the live property (no pointer-identity cache) before skipping a write.
 if(wanted<=1024){char previous[1024];DWORD bytes=sizeof(previous);
  if(SUCCEEDED(texture->GetPrivateData(D3D11TextureNameGUID,previous,&bytes))&&bytes==wanted&&std::memcmp(previous,name,wanted)==0)return;
 }
 texture->SetPrivateData(D3D11TextureNameGUID,name,static_cast<DWORD>(wanted),0);
}
void D3D11Diagnostic(const char *message){
 OutputDebugStringA(message);
 // Diagnostics stay with the isolated profile; never write in the retail folder.
 static std::string previous;static UINT count=0;
 if(!message||previous==message||count>=256)return;previous=message;++count;
 wchar_t profile[32768]{};DWORD length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",profile,32768);
 if(!length||length>=32768)return;
 std::wstring path=std::wstring(profile)+L"\\D3D11-renderer.log";FILE *file=nullptr;
 if(_wfopen_s(&file,path.c_str(),L"ab")==0&&file){std::fputs(message,file);std::fputc('\n',file);std::fclose(file);}
}

namespace GenZH::Modern {
class DeviceProxy;
struct Registry {
 std::unordered_map<void*,std::weak_ptr<BufferData>> vertex,index;
 std::unordered_map<void*,std::weak_ptr<TextureData>> textures;
 std::unordered_map<void*,void*> proxies;
 std::unordered_map<void*,IUnknown*> nativeProxies;
};

template<class Interface,class Desc>class BufferProxy final:public Interface {
public:
 BufferProxy(Interface *native,IDirect3DDevice8 *owner,std::shared_ptr<Registry> registry,bool vertex):owner_(owner),registry_(std::move(registry)),vertex_(vertex){
  owner_->AddRef();native_.Attach(native);Desc desc{};native_->GetDesc(&desc);
  auto &map=vertex_?registry_->vertex:registry_->index;data_=map[native].lock();
  if(!data_){data_=std::make_shared<BufferData>();data_->native=native_.Get();data_->bytes.resize(desc.Size);data_->usage=desc.Usage;}
  if constexpr(std::is_same_v<Desc,D3DVERTEXBUFFER_DESC>)data_->fvf=desc.FVF;else data_->format=desc.Format;
  map[native_.Get()]=data_;registry_->proxies[this]=native_.Get();registry_->nativeProxies[native_.Get()]=this;
 }
 ~BufferProxy(){registry_->proxies.erase(this);if(registry_->nativeProxies[native_.Get()]==this)registry_->nativeProxies.erase(native_.Get());owner_->Release();}
 std::shared_ptr<BufferData> Data()const{return data_;}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{
  if(!out)return E_POINTER;*out=nullptr;
  if(iid==IID_IUnknown||iid==IID_IDirect3DResource8||
   (vertex_?iid==IID_IDirect3DVertexBuffer8:iid==IID_IDirect3DIndexBuffer8)){*out=static_cast<Interface*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG count=--refs_;if(!count)delete this;return count;}
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **out)override{if(!out)return E_POINTER;*out=owner_;owner_->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,const void *data,DWORD bytes,DWORD flags)override{return native_->SetPrivateData(key,data,bytes,flags);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,void *data,DWORD *bytes)override{return native_->GetPrivateData(key,data,bytes);}
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID key)override{return native_->FreePrivateData(key);}
 DWORD STDMETHODCALLTYPE SetPriority(DWORD priority)override{return native_->SetPriority(priority);}
 DWORD STDMETHODCALLTYPE GetPriority()override{return native_->GetPriority();}
 void STDMETHODCALLTYPE PreLoad()override{native_->PreLoad();}
 D3DRESOURCETYPE STDMETHODCALLTYPE GetType()override{return native_->GetType();}
 HRESULT STDMETHODCALLTYPE GetDesc(Desc *desc)override{return native_->GetDesc(desc);}
 HRESULT STDMETHODCALLTYPE Lock(UINT offset,UINT bytes,BYTE **out,DWORD flags)override{
  if(locked_||offset>data_->bytes.size()||bytes>data_->bytes.size()-offset)return D3DERR_INVALIDCALL;
  HRESULT hr=native_->Lock(offset,bytes,out,flags);if(FAILED(hr))return hr;
  if(flags&D3DLOCK_DISCARD)std::fill(data_->bytes.begin(),data_->bytes.end(),std::uint8_t(0));
  locked_=*out;offset_=offset;bytes_=bytes?bytes:static_cast<UINT>(data_->bytes.size()-offset);
  // The caller writes the CPU mirror and Unlock streams it to the native buffer.
  // Copying back from the native dynamic memory (write-combined) at Unlock read
  // whole locked ranges at uncached speed every frame. Nothing reads native
  // vertex/index contents (ProcessVertices is not implemented by the bridge).
  mirroredLock_=mirroredBufferLocks.load(std::memory_order_relaxed);
  if(mirroredLock_)*out=data_->bytes.data()+offset;
  return hr;
 }
 HRESULT STDMETHODCALLTYPE Unlock()override{
  if(!locked_)return D3DERR_INVALIDCALL;
  if(mirroredLock_)std::memcpy(locked_,data_->bytes.data()+offset_,bytes_);
  else std::memcpy(data_->bytes.data()+offset_,locked_,bytes_);
  locked_=nullptr;
  HRESULT hr=native_->Unlock();if(SUCCEEDED(hr))++data_->revision;return hr;
 }
private:
 std::atomic<ULONG> refs_{1};IDirect3DDevice8 *owner_;std::shared_ptr<Registry> registry_;bool vertex_;
 ComPtr<Interface> native_;std::shared_ptr<BufferData> data_;BYTE *locked_=nullptr;UINT offset_=0,bytes_=0;bool mirroredLock_=false;
};

class SurfaceProxy final:public IDirect3DSurface8 {
public:
 void *ResourceNative(bool changed){if(changed){if(texture_)++texture_->revision;}return native_.Get();}

 SurfaceProxy(IDirect3DSurface8 *native,IDirect3DDevice8 *owner,std::shared_ptr<Registry> registry,std::shared_ptr<TextureData> texture):owner_(owner),registry_(std::move(registry)),texture_(std::move(texture)){
  native_.Attach(native);owner_->AddRef();registry_->proxies[this]=native;registry_->nativeProxies[native]=this;
 }
 ~SurfaceProxy(){registry_->proxies.erase(this);if(registry_->nativeProxies[native_.Get()]==this)registry_->nativeProxies.erase(native_.Get());owner_->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{if(!out)return E_POINTER;*out=nullptr;
  if(iid==IID_IUnknown||iid==IID_IDirect3DSurface8){*out=static_cast<IDirect3DSurface8*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG count=--refs_;if(!count)delete this;return count;}
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **out)override{if(!out)return E_POINTER;*out=owner_;owner_->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,const void *data,DWORD bytes,DWORD flags)override{return native_->SetPrivateData(key,data,bytes,flags);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,void *data,DWORD *bytes)override{return native_->GetPrivateData(key,data,bytes);}
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID key)override{return native_->FreePrivateData(key);}
 HRESULT STDMETHODCALLTYPE GetContainer(REFIID iid,void **out)override{return native_->GetContainer(iid,out);}
 HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *desc)override{return native_->GetDesc(desc);}
 HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *rect,const RECT *region,DWORD flags)override{readOnly_=(flags&D3DLOCK_READONLY)!=0;return native_->LockRect(rect,region,flags);}
 HRESULT STDMETHODCALLTYPE UnlockRect()override{HRESULT hr=native_->UnlockRect();if(SUCCEEDED(hr)&&!readOnly_&&texture_)++texture_->revision;return hr;}
private:
 std::atomic<ULONG> refs_{1};ComPtr<IDirect3DSurface8> native_;IDirect3DDevice8 *owner_;std::shared_ptr<Registry> registry_;
 std::shared_ptr<TextureData> texture_;bool readOnly_=false;
};

class TextureProxy final:public IDirect3DTexture8 {
public:
 void *ResourceNative(bool changed){if(changed){++data_->revision;}return native_.Get();}

 TextureProxy(IDirect3DTexture8 *native,IDirect3DDevice8 *owner,std::shared_ptr<Registry> registry):owner_(owner),registry_(std::move(registry)){
  owner_->AddRef();native_.Attach(native);data_=registry_->textures[native].lock();if(!data_){data_=std::make_shared<TextureData>();data_->native=native_.Get();}
  registry_->textures[native_.Get()]=data_;registry_->proxies[static_cast<IDirect3DBaseTexture8*>(this)]=native_.Get();registry_->nativeProxies[native_.Get()]=this;
 }
 ~TextureProxy(){registry_->proxies.erase(static_cast<IDirect3DBaseTexture8*>(this));if(registry_->nativeProxies[native_.Get()]==this)registry_->nativeProxies.erase(native_.Get());owner_->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{
  if(!out)return E_POINTER;*out=nullptr;
  if(iid==IID_IUnknown||iid==IID_IDirect3DResource8||iid==IID_IDirect3DBaseTexture8||iid==IID_IDirect3DTexture8){*out=static_cast<IDirect3DTexture8*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG count=--refs_;if(!count)delete this;return count;}
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **out)override{if(!out)return E_POINTER;*out=owner_;owner_->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,const void *data,DWORD bytes,DWORD flags)override{return native_->SetPrivateData(key,data,bytes,flags);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,void *data,DWORD *bytes)override{return native_->GetPrivateData(key,data,bytes);}
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID key)override{return native_->FreePrivateData(key);}
 DWORD STDMETHODCALLTYPE SetPriority(DWORD priority)override{return native_->SetPriority(priority);}
 DWORD STDMETHODCALLTYPE GetPriority()override{return native_->GetPriority();}
 void STDMETHODCALLTYPE PreLoad()override{native_->PreLoad();}
 D3DRESOURCETYPE STDMETHODCALLTYPE GetType()override{return native_->GetType();}
 DWORD STDMETHODCALLTYPE SetLOD(DWORD lod)override{return native_->SetLOD(lod);}
 DWORD STDMETHODCALLTYPE GetLOD()override{return native_->GetLOD();}
 DWORD STDMETHODCALLTYPE GetLevelCount()override{return native_->GetLevelCount();}
 HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level,D3DSURFACE_DESC *desc)override{return native_->GetLevelDesc(level,desc);}
 HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level,IDirect3DSurface8 **surface)override{
  if(!surface)return E_POINTER;*surface=nullptr;IDirect3DSurface8 *original=nullptr;HRESULT hr=native_->GetSurfaceLevel(level,&original);
  if(SUCCEEDED(hr))*surface=new SurfaceProxy(original,owner_,registry_,data_);return hr;
 }
 HRESULT STDMETHODCALLTYPE LockRect(UINT level,D3DLOCKED_RECT *rect,const RECT *region,DWORD flags)override{return native_->LockRect(level,rect,region,flags);}
 HRESULT STDMETHODCALLTYPE UnlockRect(UINT level)override{HRESULT hr=native_->UnlockRect(level);if(SUCCEEDED(hr)){++data_->revision;}return hr;}
 HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *rect)override{++data_->revision;return native_->AddDirtyRect(rect);}
private:
 std::atomic<ULONG> refs_{1};IDirect3DDevice8 *owner_;std::shared_ptr<Registry> registry_;
 ComPtr<IDirect3DTexture8> native_;std::shared_ptr<TextureData> data_;
};

class CubeTextureProxy final:public IDirect3DCubeTexture8 {
public:
 void *ResourceNative(bool changed){if(changed){++data_->revision;}return native_.Get();}

 CubeTextureProxy(IDirect3DCubeTexture8 *native,IDirect3DDevice8 *owner,std::shared_ptr<Registry> registry):owner_(owner),registry_(std::move(registry)){
  owner_->AddRef();native_.Attach(native);data_=registry_->textures[native].lock();if(!data_){data_=std::make_shared<TextureData>();data_->native=native_.Get();}
  registry_->textures[native_.Get()]=data_;registry_->proxies[static_cast<IDirect3DBaseTexture8*>(this)]=native_.Get();registry_->nativeProxies[native_.Get()]=this;
 }
 ~CubeTextureProxy(){registry_->proxies.erase(static_cast<IDirect3DBaseTexture8*>(this));if(registry_->nativeProxies[native_.Get()]==this)registry_->nativeProxies.erase(native_.Get());owner_->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{
  if(!out)return E_POINTER;*out=nullptr;
  if(iid==IID_IUnknown||iid==IID_IDirect3DResource8||iid==IID_IDirect3DBaseTexture8||iid==IID_IDirect3DCubeTexture8){*out=static_cast<IDirect3DCubeTexture8*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG count=--refs_;if(!count)delete this;return count;}
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **out)override{if(!out)return E_POINTER;*out=owner_;owner_->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,const void *data,DWORD bytes,DWORD flags)override{return native_->SetPrivateData(key,data,bytes,flags);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,void *data,DWORD *bytes)override{return native_->GetPrivateData(key,data,bytes);}
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID key)override{return native_->FreePrivateData(key);}
 DWORD STDMETHODCALLTYPE SetPriority(DWORD priority)override{return native_->SetPriority(priority);}
 DWORD STDMETHODCALLTYPE GetPriority()override{return native_->GetPriority();}
 void STDMETHODCALLTYPE PreLoad()override{native_->PreLoad();}
 D3DRESOURCETYPE STDMETHODCALLTYPE GetType()override{return native_->GetType();}
 DWORD STDMETHODCALLTYPE SetLOD(DWORD lod)override{return native_->SetLOD(lod);}
 DWORD STDMETHODCALLTYPE GetLOD()override{return native_->GetLOD();}
 DWORD STDMETHODCALLTYPE GetLevelCount()override{return native_->GetLevelCount();}
 HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level,D3DSURFACE_DESC *desc)override{return native_->GetLevelDesc(level,desc);}
 HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face,UINT level,IDirect3DSurface8 **surface)override{
  if(!surface)return E_POINTER;*surface=nullptr;IDirect3DSurface8 *original=nullptr;HRESULT hr=native_->GetCubeMapSurface(face,level,&original);
  if(SUCCEEDED(hr))*surface=new SurfaceProxy(original,owner_,registry_,data_);return hr;
 }
 HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face,UINT level,D3DLOCKED_RECT *rect,const RECT *region,DWORD flags)override{return native_->LockRect(face,level,rect,region,flags);}
 HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face,UINT level)override{HRESULT hr=native_->UnlockRect(face,level);if(SUCCEEDED(hr))++data_->revision;return hr;}
 HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face,const RECT *rect)override{++data_->revision;return native_->AddDirtyRect(face,rect);}
private:
 std::atomic<ULONG> refs_{1};IDirect3DDevice8 *owner_;std::shared_ptr<Registry> registry_;
 ComPtr<IDirect3DCubeTexture8> native_;std::shared_ptr<TextureData> data_;
};

class VolumeProxy final:public IDirect3DVolume8 {
public:
 void *ResourceNative(bool changed){if(changed&&data_)++data_->revision;return native_.Get();}

 VolumeProxy(IDirect3DVolume8 *native,IDirect3DDevice8 *owner,std::shared_ptr<TextureData> data):owner_(owner),data_(std::move(data)){native_.Attach(native);owner_->AddRef();}
 ~VolumeProxy(){owner_->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IDirect3DVolume8){*out=static_cast<IDirect3DVolume8*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG count=--refs_;if(!count)delete this;return count;}
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **out)override{if(!out)return E_POINTER;*out=owner_;owner_->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,const void *data,DWORD bytes,DWORD flags)override{return native_->SetPrivateData(key,data,bytes,flags);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,void *data,DWORD *bytes)override{return native_->GetPrivateData(key,data,bytes);}
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID key)override{return native_->FreePrivateData(key);}
 HRESULT STDMETHODCALLTYPE GetContainer(REFIID iid,void **out)override{return native_->GetContainer(iid,out);}
 HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *desc)override{return native_->GetDesc(desc);}
 HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *box,const D3DBOX *region,DWORD flags)override{return native_->LockBox(box,region,flags);}
 HRESULT STDMETHODCALLTYPE UnlockBox()override{HRESULT hr=native_->UnlockBox();if(SUCCEEDED(hr))++data_->revision;return hr;}
private:
 std::atomic<ULONG> refs_{1};ComPtr<IDirect3DVolume8> native_;IDirect3DDevice8 *owner_;std::shared_ptr<TextureData> data_;
};

class VolumeTextureProxy final:public IDirect3DVolumeTexture8 {
public:
 void *ResourceNative(bool changed){if(changed){++data_->revision;}return native_.Get();}

 VolumeTextureProxy(IDirect3DVolumeTexture8 *native,IDirect3DDevice8 *owner,std::shared_ptr<Registry> registry):owner_(owner),registry_(std::move(registry)){
  owner_->AddRef();native_.Attach(native);data_=registry_->textures[native].lock();if(!data_){data_=std::make_shared<TextureData>();data_->native=native_.Get();}
  registry_->textures[native_.Get()]=data_;registry_->proxies[static_cast<IDirect3DBaseTexture8*>(this)]=native_.Get();registry_->nativeProxies[native_.Get()]=this;
 }
 ~VolumeTextureProxy(){registry_->proxies.erase(static_cast<IDirect3DBaseTexture8*>(this));if(registry_->nativeProxies[native_.Get()]==this)registry_->nativeProxies.erase(native_.Get());owner_->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{
  if(!out)return E_POINTER;*out=nullptr;
  if(iid==IID_IUnknown||iid==IID_IDirect3DResource8||iid==IID_IDirect3DBaseTexture8||iid==IID_IDirect3DVolumeTexture8){*out=static_cast<IDirect3DVolumeTexture8*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG count=--refs_;if(!count)delete this;return count;}
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **out)override{if(!out)return E_POINTER;*out=owner_;owner_->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,const void *data,DWORD bytes,DWORD flags)override{return native_->SetPrivateData(key,data,bytes,flags);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,void *data,DWORD *bytes)override{return native_->GetPrivateData(key,data,bytes);}
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID key)override{return native_->FreePrivateData(key);}
 DWORD STDMETHODCALLTYPE SetPriority(DWORD priority)override{return native_->SetPriority(priority);}
 DWORD STDMETHODCALLTYPE GetPriority()override{return native_->GetPriority();}
 void STDMETHODCALLTYPE PreLoad()override{native_->PreLoad();}
 D3DRESOURCETYPE STDMETHODCALLTYPE GetType()override{return native_->GetType();}
 DWORD STDMETHODCALLTYPE SetLOD(DWORD lod)override{return native_->SetLOD(lod);}
 DWORD STDMETHODCALLTYPE GetLOD()override{return native_->GetLOD();}
 DWORD STDMETHODCALLTYPE GetLevelCount()override{return native_->GetLevelCount();}
 HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level,D3DVOLUME_DESC *desc)override{return native_->GetLevelDesc(level,desc);}
 HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level,IDirect3DVolume8 **out)override{
  if(!out)return E_POINTER;*out=nullptr;IDirect3DVolume8 *original=nullptr;HRESULT hr=native_->GetVolumeLevel(level,&original);
  if(SUCCEEDED(hr))*out=new VolumeProxy(original,owner_,data_);return hr;
 }
 HRESULT STDMETHODCALLTYPE LockBox(UINT level,D3DLOCKED_BOX *box,const D3DBOX *region,DWORD flags)override{return native_->LockBox(level,box,region,flags);}
 HRESULT STDMETHODCALLTYPE UnlockBox(UINT level)override{HRESULT hr=native_->UnlockBox(level);if(SUCCEEDED(hr))++data_->revision;return hr;}
 HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *box)override{++data_->revision;return native_->AddDirtyBox(box);}
private:
 std::atomic<ULONG> refs_{1};IDirect3DDevice8 *owner_;std::shared_ptr<Registry> registry_;
 ComPtr<IDirect3DVolumeTexture8> native_;std::shared_ptr<TextureData> data_;
};

class DeviceProxy final:public IDirect3DDevice8 {
public:
 explicit DeviceProxy(IDirect3DDevice8 *native):native_(native),registry_(std::make_shared<Registry>()){for(UINT i=0;i<256;++i)gamma_.red[i]=gamma_.green[i]=gamma_.blue[i]=static_cast<WORD>(i*257);}
 HRESULT Initialize(HWND window){
  window_=window;D3DDEVICE_CREATION_PARAMETERS parameters{};native_->GetCreationParameters(&parameters);
  ComPtr<IDirect3DSurface8> back;D3DSURFACE_DESC desc{};HRESULT hr=native_->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,back.GetAddressOf());
  if(SUCCEEDED(hr))hr=back->GetDesc(&desc);if(FAILED(hr))return hr;
  width_=desc.Width;height_=desc.Height;hr=renderer_.Initialize(native_.Get(),window,width_,height_);
  if(SUCCEEDED(hr)){
   char mode[32]{};GetEnvironmentVariableA("GENZH_NATIVE_SUBMIT",mode,sizeof(mode));nativeSubmitEnabled_=std::strcmp(mode,"off")!=0;
   char sortedMode[32]{};GetEnvironmentVariableA("GENZH_SORTED_SUBMIT",sortedMode,sizeof(sortedMode));nativeSortedEnabled_=std::strcmp(sortedMode,"off")!=0;
   char twoUVGeometry[32]{};GetEnvironmentVariableA("GENZH_TWO_UV_SORTED",twoUVGeometry,sizeof(twoUVGeometry));twoUVSortedEnabled_=std::strcmp(twoUVGeometry,"off")!=0;
   char sortedGeometry[32]{};GetEnvironmentVariableA("GENZH_SHARED_SORTED_GEOMETRY",sortedGeometry,sizeof(sortedGeometry));sharedSortedGeometryEnabled_=std::strcmp(sortedGeometry,"off")!=0;
   mode[0]=0;GetEnvironmentVariableA("GENZH_SHARED_NATIVE_VERTICES",mode,sizeof(mode));nativeSharedVerticesEnabled_=std::strcmp(mode,"off")!=0;
   mode[0]=0;GetEnvironmentVariableA("GENZH_NATIVE_GEOMETRY",mode,sizeof(mode));nativeGeometryEnabled_=std::strcmp(mode,"off")!=0;
   mode[0]=0;GetEnvironmentVariableA("GENZH_DEFER_FIXED_WRITES",mode,sizeof(mode));deferFixedEnabled_=std::strcmp(mode,"off")!=0;
   nativeMeshDispatch.Connect(static_cast<IDirect3DDevice8*>(this),this,[](void *owner,const NativeIndexedDraw &draw,long &result){return static_cast<DeviceProxy*>(owner)->TryNativeIndexed(draw,result);},nativeSubmitEnabled_&&nativeGeometryEnabled_,[]{char mode[32]{};GetEnvironmentVariableA("GENZH_LAZY_LEGACY_BUFFERS",mode,sizeof(mode));return std::strcmp(mode,"off")!=0;}(),
    [](void *owner){auto &d=*static_cast<DeviceProxy*>(owner);return d.BeginFixedWrites();},
    [](void *owner){auto &d=*static_cast<DeviceProxy*>(owner);if(FAILED(d.deferredFixed_.End(*d.native_.Get())))D3D11Diagnostic("Deferred native fixed state synchronization failed");});
  }return hr;
 }
 const std::string &Error()const{return renderer_.LastError();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{
  if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IDirect3DDevice8){*out=static_cast<IDirect3DDevice8*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG count=--refs_;if(!count)delete this;return count;}
 HRESULT STDMETHODCALLTYPE TestCooperativeLevel()override{
  const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;
  HRESULT hr=renderer_.Renderer().DeviceStatus();return FAILED(hr)?D3DERR_DEVICENOTRESET:native_->TestCooperativeLevel();
 }
 HRESULT STDMETHODCALLTYPE BeginScene()override{
  const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;
  geometryCache_.PruneExpired();nativeVertexPool_.Prune();sortedBufferPool_.Prune();
  if(geometryMemory_.Update(renderer_.Renderer().Device(),geometryCache_)){
   const auto message=std::string("Geometry cache budget: ")+std::to_string(geometryCache_.Limit()/(1024*1024))+" MiB combined CPU/GPU; OS headroom policy";
   D3D11Diagnostic(message.c_str());
  }
  sortedTemplates_.clear();
  geometryCache_.BeginSceneBudget();HRESULT hr=native_->BeginScene();return FAILED(hr)?hr:renderer_.BeginScene();
 }
 HRESULT STDMETHODCALLTYPE EndScene()override{
  const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;
  renderer_.GeometryCacheState(geometryCache_.Bytes(),geometryCache_.Size(),geometryCache_.Evictions());
  sortedTemplates_.clear();RecordSortedRunTemplates();
  HRESULT hr=renderer_.EndScene();HRESULT native=native_->EndScene();geometryMemory_.Record(geometryCache_);RecordNativeSubmit();return FAILED(hr)?hr:native;
 }
 HRESULT STDMETHODCALLTYPE Present(const RECT*,const RECT*,HWND,const RGNDATA*)override{return renderer_.Present();}
 HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *parameters)override{
  if(!parameters)return E_POINTER;
  boundVertices_.fill(nullptr);boundIndex_.reset();boundTextures_.fill(nullptr);sortedTemplates_.clear();
  deferredFixed_.Reset();
  blocks_.clear();geometryCache_.Clear();nativeVertexPool_.Clear();sortedBufferPool_.Clear();geometryMemory_.Reset();fixedState_.Reset();nativeBindingsReady_=nativeTexturesReady_=false;renderer_.PrepareReset();
  HRESULT hr=native_->Reset(parameters);if(FAILED(hr))return hr;stateMirror_.Reset();
  width_=parameters->BackBufferWidth;height_=parameters->BackBufferHeight;
  hr=renderer_.Reset(window_,width_,height_);return FAILED(hr)?hr:renderer_.Renderer().SetGamma(gamma_.red,gamma_.green,gamma_.blue);
 }
 HRESULT STDMETHODCALLTYPE Clear(DWORD count,const D3DRECT*,DWORD flags,D3DCOLOR color,float depth,DWORD stencil)override{
  if(count)return D3DERR_INVALIDCALL;return renderer_.Clear(flags,color,depth,stencil);
 }
 HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8 *target,IDirect3DSurface8 *depth)override{
  const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;
  auto *color=Unwrap(target);auto *ds=Unwrap(depth);HRESULT hr=native_->SetRenderTarget(color,ds);
  if(FAILED(hr))return hr;fixedState_.Invalidate();nativeBindingsReady_=nativeTexturesReady_=false;return renderer_.SetTarget(color,ds);
 }
 HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT bytes,DWORD usage,DWORD fvf,D3DPOOL pool,IDirect3DVertexBuffer8 **out)override{
  if(!out)return E_POINTER;*out=nullptr;IDirect3DVertexBuffer8 *native=nullptr;
  HRESULT hr=native_->CreateVertexBuffer(bytes,usage,fvf,pool,&native);if(FAILED(hr))return hr;
  *out=new BufferProxy<IDirect3DVertexBuffer8,D3DVERTEXBUFFER_DESC>(native,this,registry_,true);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT bytes,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DIndexBuffer8 **out)override{
  if(!out)return E_POINTER;*out=nullptr;IDirect3DIndexBuffer8 *native=nullptr;
  HRESULT hr=native_->CreateIndexBuffer(bytes,usage,format,pool,&native);if(FAILED(hr))return hr;
  *out=new BufferProxy<IDirect3DIndexBuffer8,D3DINDEXBUFFER_DESC>(native,this,registry_,false);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,IDirect3DVertexBuffer8 *buffer,UINT stride)override{
  if(stream>=boundVertices_.size())return D3DERR_INVALIDCALL;auto *original=Unwrap(buffer);
  HRESULT hr=native_->SetStreamSource(stream,original,stride);if(SUCCEEDED(hr)){boundVertices_[stream]=original?registry_->vertex[original].lock():nullptr;boundStrides_[stream]=stride;if(fixedState_.Recording())nativeBindingsReady_=false;}return hr;
 }
 HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream,IDirect3DVertexBuffer8 **out,UINT *stride)override{
  HRESULT hr=native_->GetStreamSource(stream,out,stride);if(FAILED(hr)||!out||!*out)return hr;auto *original=*out;
  auto it=registry_->nativeProxies.find(original);if(it!=registry_->nativeProxies.end()){hr=it->second->QueryInterface(IID_IDirect3DVertexBuffer8,reinterpret_cast<void**>(out));original->Release();return hr;}
  *out=new BufferProxy<IDirect3DVertexBuffer8,D3DVERTEXBUFFER_DESC>(original,this,registry_,true);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8 *buffer,UINT base)override{
  auto *original=Unwrap(buffer);HRESULT hr=native_->SetIndices(original,base);if(SUCCEEDED(hr)){boundIndex_=original?registry_->index[original].lock():nullptr;boundBase_=base;if(fixedState_.Recording())nativeBindingsReady_=false;}return hr;
 }
 HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8 **out,UINT *base)override{
  HRESULT hr=native_->GetIndices(out,base);if(FAILED(hr)||!out||!*out)return hr;auto *original=*out;
  auto it=registry_->nativeProxies.find(original);if(it!=registry_->nativeProxies.end()){hr=it->second->QueryInterface(IID_IDirect3DIndexBuffer8,reinterpret_cast<void**>(out));original->Release();return hr;}
  *out=new BufferProxy<IDirect3DIndexBuffer8,D3DINDEXBUFFER_DESC>(original,this,registry_,false);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE CreateTexture(UINT width,UINT height,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture8 **out)override{
  if(!out)return E_POINTER;*out=nullptr;IDirect3DTexture8 *native=nullptr;
  HRESULT hr=native_->CreateTexture(width,height,levels,usage,format,pool,&native);if(FAILED(hr))return hr;
  *out=new TextureProxy(native,this,registry_);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT size,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DCubeTexture8 **out)override{
  if(!out)return E_POINTER;*out=nullptr;IDirect3DCubeTexture8 *texture=nullptr;HRESULT hr=native_->CreateCubeTexture(size,levels,usage,format,pool,&texture);
  if(SUCCEEDED(hr))*out=new CubeTextureProxy(texture,this,registry_);return hr;
 }
 HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT width,UINT height,UINT depth,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DVolumeTexture8 **out)override{
  if(!out)return E_POINTER;*out=nullptr;IDirect3DVolumeTexture8 *texture=nullptr;HRESULT hr=native_->CreateVolumeTexture(width,height,depth,levels,usage,format,pool,&texture);
  if(SUCCEEDED(hr))*out=new VolumeTextureProxy(texture,this,registry_);return hr;
 }
 HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,IDirect3DBaseTexture8 *texture)override{
  if(stage>=8)return D3DERR_INVALIDCALL;auto *original=Unwrap(texture);
  HRESULT hr=native_->SetTexture(stage,original);if(SUCCEEDED(hr)){boundTextures_[stage]=TextureInfo(original);if(fixedState_.Recording())nativeBindingsReady_=nativeTexturesReady_=false;}return hr;
 }
 HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,IDirect3DBaseTexture8 **out)override{
  HRESULT hr=native_->GetTexture(stage,out);if(FAILED(hr)||!out||!*out)return hr;auto *original=*out;
  auto it=registry_->nativeProxies.find(original);if(it!=registry_->nativeProxies.end()){hr=it->second->QueryInterface(IID_IDirect3DBaseTexture8,reinterpret_cast<void**>(out));original->Release();return hr;}
  switch(original->GetType()){
   case D3DRTYPE_TEXTURE:*out=new TextureProxy(static_cast<IDirect3DTexture8*>(original),this,registry_);break;
   case D3DRTYPE_CUBETEXTURE:*out=new CubeTextureProxy(static_cast<IDirect3DCubeTexture8*>(original),this,registry_);break;
   case D3DRTYPE_VOLUMETEXTURE:*out=new VolumeTextureProxy(static_cast<IDirect3DVolumeTexture8*>(original),this,registry_);break;
   default:original->Release();*out=nullptr;return E_NOTIMPL;
  }return S_OK;
 }
 HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8 *source,IDirect3DBaseTexture8 *dest)override{
  auto *original=Unwrap(dest);HRESULT hr=native_->UpdateTexture(Unwrap(source),original);
  if(SUCCEEDED(hr))renderer_.TextureChanged(TextureInfo(original));return hr;
 }
 HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8 *source,const RECT *rects,UINT count,IDirect3DSurface8 *dest,const POINT *points)override{
  source=Unwrap(source);dest=Unwrap(dest);
  HRESULT hr=renderer_.SynchronizeSurface(source);if(FAILED(hr))return hr;
  // perf87: signature of the bytes this copy writes, taken before the copy (CopyRectsRepeat.h).
  const int mode=copyRectsRepeatMode.load(std::memory_order_relaxed);
  CopyRectsSignature signature;const bool eligible=mode&&CopySignature(source,rects,count,dest,points,signature);
  hr=native_->CopyRects(source,rects,count,dest,points);if(SUCCEEDED(hr)){ComPtr<IDirect3DBaseTexture8> texture;
   if(SUCCEEDED(dest->GetContainer(IID_IDirect3DBaseTexture8,reinterpret_cast<void**>(texture.GetAddressOf())))){
    auto data=TextureInfo(texture.Get());auto &stats=renderer_.CopyStats();++stats.copies;
    const bool repeat=eligible&&copyRepeats_.Repeat(data,data->revision,dest,signature);
    if(repeat)++stats.repeats;
    if(!repeat||mode!=1){ // a repeat in mode 1 leaves the texture bytes, revision and D3D11 copy unchanged
     const auto before=data->revision;renderer_.TextureChanged(data);
     if(eligible){++stats.tracked;
      if(mode==2){data->copyVerified=true;if(repeat){data->baseRevision=before;data->expectedRevision=data->revision;}}
      copyRepeats_.Record(data,data->revision,dest,std::move(signature));
     }else copyRepeats_.Forget(dest);
    }
   }
   hr=renderer_.SurfaceChanged(dest); }return hr;
 }
 // Uncompressed formats whose rows CopyRects copies verbatim; 0 = not tracked.
 static UINT CopyPixelBytes(D3DFORMAT format){
  switch(format){
   case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:return 4;
   case D3DFMT_R5G6B5:case D3DFMT_X1R5G5B5:case D3DFMT_A1R5G5B5:case D3DFMT_A4R4G4B4:case D3DFMT_X4R4G4B4:case D3DFMT_A8R3G3B2:case D3DFMT_A8L8:return 2;
   case D3DFMT_A8:case D3DFMT_L8:case D3DFMT_R3G3B2:return 1;
   default:return 0;
  }
 }
 // Only system-memory sources into non-target texture surfaces the renderer
 // never writes; rectangles must lie inside the source. Anything else is untracked.
 bool CopySignature(IDirect3DSurface8 *source,const RECT *rects,UINT count,IDirect3DSurface8 *dest,const POINT *points,CopyRectsSignature &out){
  if(!source||!dest||source==dest||(rects&&!count)||renderer_.TracksSurface(source)||renderer_.TracksSurface(dest))return false;
  D3DSURFACE_DESC from{},to{};if(FAILED(source->GetDesc(&from))||FAILED(dest->GetDesc(&to)))return false;
  if(from.Pool!=D3DPOOL_SYSTEMMEM||from.Format!=to.Format||(to.Usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return false;
  const UINT pixel=CopyPixelBytes(from.Format);if(!pixel)return false;
  const RECT whole{0,0,static_cast<LONG>(from.Width),static_cast<LONG>(from.Height)};
  const UINT n=rects?count:1;std::size_t total=0;
  out.geometry.assign({static_cast<std::int32_t>(from.Format),static_cast<std::int32_t>(pixel),static_cast<std::int32_t>(n)});
  for(UINT i=0;i<n;++i){const RECT &r=rects?rects[i]:whole;
   if(r.left<0||r.top<0||r.left>=r.right||r.top>=r.bottom||r.right>static_cast<LONG>(from.Width)||r.bottom>static_cast<LONG>(from.Height))return false;
   const POINT p=points?points[i]:POINT{r.left,r.top};
   for(const LONG v:{r.left,r.top,r.right,r.bottom,p.x,p.y})out.geometry.push_back(static_cast<std::int32_t>(v));
   total+=std::size_t(r.right-r.left)*std::size_t(r.bottom-r.top)*pixel;
   if(total>CopyRectsRepeats<TextureData>::MaxBytes)return false;
  }
  D3DLOCKED_RECT lock{};if(FAILED(source->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return false;
  out.bytes.resize(total);auto *to8=out.bytes.data();
  for(UINT i=0;i<n;++i){const RECT &r=rects?rects[i]:whole;const std::size_t row=std::size_t(r.right-r.left)*pixel;
   for(LONG y=r.top;y<r.bottom;++y){std::memcpy(to8,static_cast<const std::uint8_t*>(lock.pBits)+std::size_t(y)*lock.Pitch+std::size_t(r.left)*pixel,row);to8+=row;}
  }
  source->UnlockRect();return true;
 }
 HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,UINT first,UINT primitives)override{return BufferedDraw(type,first,PrimitiveVertexCount(type,primitives),0,primitives,false);}
 HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,UINT minIndex,UINT vertices,UINT firstIndex,UINT primitives)override{return BufferedDraw(type,minIndex,vertices,firstIndex,primitives,true);}
 HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,UINT primitives,const void *vertices,UINT stride)override{
  UINT count=PrimitiveVertexCount(type,primitives);DWORD fvf=0;native_->GetVertexShader(&fvf);
  DrawPacket packet;packet.fvf=fvf;packet.primitive=type;std::string error;
  HRESULT hr=DecodeVertices(vertices,std::size_t(count)*stride,stride,fvf,0,count,packet.vertices,error);
  if(FAILED(hr))return Report(hr,error);return Submit(std::move(packet));
 }
 HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,UINT first,UINT vertices,UINT primitives,const void *indices,D3DFORMAT format,const void *data,UINT stride)override{
  DrawPacket packet;native_->GetVertexShader(&packet.fvf);packet.primitive=type;std::string error;
  if(std::uint64_t(first)+vertices>UINT_MAX)return E_INVALIDARG;
  UINT count=PrimitiveVertexCount(type,primitives);
  HRESULT hr=DecodeIndices(indices,std::size_t(count)*(format==D3DFMT_INDEX16?2:4),format,0,count,0,first+vertices,packet.indices);
  if(FAILED(hr))return hr;
  hr=DecodeReferencedVertices(data,std::size_t(first+std::uint64_t(vertices))*stride,stride,packet.fvf,first,vertices,packet.indices,packet.vertices,error);
  if(FAILED(hr))return Report(hr,error);return Submit(std::move(packet));
 }
 HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *decl,const DWORD *function,DWORD *handle,DWORD usage)override{
  HRESULT hr=native_->CreateVertexShader(decl,function,handle,usage);if(FAILED(hr))return hr;
  DWORD bytes=0;if(FAILED(hr=native_->GetVertexShaderDeclaration(*handle,nullptr,&bytes)))return hr;
  std::vector<DWORD> tokens((bytes+3)/4);if(FAILED(hr=native_->GetVertexShaderDeclaration(*handle,tokens.data(),&bytes)))return hr;
  declarations_[*handle]=std::move(tokens);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD handle)override{const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;declarations_.erase(handle);renderer_.ForgetShaders();HRESULT hr=native_->DeleteVertexShader(handle);if(SUCCEEDED(hr))fixedState_.Invalidate();return hr;}
 HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD handle)override{const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;renderer_.ForgetShaders();HRESULT hr=native_->DeletePixelShader(handle);if(SUCCEEDED(hr))fixedState_.Invalidate();return hr;}
 HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*,IDirect3DSwapChain8**)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT,const float*,const D3DRECTPATCH_INFO*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT,const float*,const D3DTRIPATCH_INFO*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE ProcessVertices(UINT,UINT,UINT,IDirect3DVertexBuffer8*,DWORD)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8 *surface)override{return renderer_.ReadFrontBuffer(Unwrap(surface));}
 HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT width,UINT height,D3DFORMAT format,IDirect3DSurface8 **out)override{return WrapSurface(native_->CreateImageSurface(width,height,format,out),out);}
 HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT width,UINT height,D3DFORMAT format,D3DMULTISAMPLE_TYPE sample,BOOL lockable,IDirect3DSurface8 **out)override{return WrapSurface(native_->CreateRenderTarget(width,height,format,sample,lockable,out),out);}
 HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT width,UINT height,D3DFORMAT format,D3DMULTISAMPLE_TYPE sample,IDirect3DSurface8 **out)override{return WrapSurface(native_->CreateDepthStencilSurface(width,height,format,sample,out),out);}
 HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8 **out)override{return WrapSurface(native_->GetRenderTarget(out),out);}
 HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8 **out)override{return WrapSurface(native_->GetDepthStencilSurface(out),out);}
 HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index,D3DBACKBUFFER_TYPE type,IDirect3DSurface8 **out)override{return WrapSurface(native_->GetBackBuffer(index,type,out),out);}
 void STDMETHODCALLTYPE SetGammaRamp(DWORD,const D3DGAMMARAMP *ramp)override{if(ramp){gamma_=*ramp;HRESULT hr=renderer_.Renderer().SetGamma(ramp->red,ramp->green,ramp->blue);if(FAILED(hr))Report(hr,"gamma lookup update failed");}}
 void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP *ramp)override{if(ramp)*ramp=gamma_;}
 HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x,UINT y,IDirect3DSurface8 *surface)override{return native_->SetCursorProperties(x,y,Unwrap(surface));}
 HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD *token)override{const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;HRESULT hr=native_->EndStateBlock(token);stateMirror_.EndBlock(hr);fixedState_.EndBlock(hr);if(SUCCEEDED(hr)){nativeBindingsReady_=nativeTexturesReady_=false;RetainBlock(*token);}return hr;}
 HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,DWORD *token)override{const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;HRESULT hr=native_->CreateStateBlock(type,token);if(SUCCEEDED(hr))RetainBlock(*token);return hr;}
 HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD token)override{const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;HRESULT hr=native_->CaptureStateBlock(token);if(SUCCEEDED(hr))RetainBlock(token);return hr;}
 HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD token)override{const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;HRESULT hr=native_->DeleteStateBlock(token);if(SUCCEEDED(hr))blocks_.erase(token);return hr;}
 HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD token)override{
  const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;
  HRESULT hr=native_->ApplyStateBlock(token);if(FAILED(hr))return hr;
  stateMirror_.Invalidate();fixedState_.Invalidate();nativeBindingsReady_=nativeTexturesReady_=false;
  for(UINT i=0;i<16;++i){ComPtr<IDirect3DVertexBuffer8> vb;UINT stride=0;native_->GetStreamSource(i,vb.GetAddressOf(),&stride);boundVertices_[i]=vb?registry_->vertex[vb.Get()].lock():nullptr;}
  ComPtr<IDirect3DIndexBuffer8> ib;UINT base=0;native_->GetIndices(ib.GetAddressOf(),&base);boundIndex_=ib?registry_->index[ib.Get()].lock():nullptr;
  for(UINT i=0;i<8;++i){ComPtr<IDirect3DBaseTexture8> texture;native_->GetTexture(i,texture.GetAddressOf());boundTextures_[i]=TextureInfo(texture.Get());}return hr;
 }
 #include "DeviceForwarders.inl"
private:
 HRESULT SyncFixedWrites(){return deferredFixed_.Sync(*native_.Get());}
 bool BeginFixedWrites(){
  return nativeSubmitEnabled_&&nativeSortedEnabled_&&deferFixedEnabled_&&!renderer_.Offscreen()&&!fixedState_.Recording()&&
   fixedState_.Capture(*native_.Get())&&deferredFixed_.Begin(true,fixedState_);
 }
 bool CaptureNativeTextures(){
  if(nativeTexturesReady_)return true;++nativeGeometryStats_.textureRefresh;
  std::array<std::shared_ptr<TextureData>,8> textures;
  for(UINT stage=0;stage<8;++stage){ComPtr<IDirect3DBaseTexture8> texture;
   if(FAILED(native_->GetTexture(stage,texture.GetAddressOf())))return false;textures[stage]=TextureInfo(texture.Get());
  }
  boundTextures_=std::move(textures);nativeTexturesReady_=true;return true;
 }
 bool CaptureNativeBindings(){
  ++nativeSubmitStats_.bindingRefresh;
  ComPtr<IDirect3DVertexBuffer8> vb;ComPtr<IDirect3DIndexBuffer8> ib;UINT stride=0,base=0;
  if(FAILED(native_->GetStreamSource(0,vb.GetAddressOf(),&stride))||FAILED(native_->GetIndices(ib.GetAddressOf(),&base))||!CaptureNativeTextures())return false;
  boundVertices_[0]=vb?registry_->vertex[vb.Get()].lock():nullptr;boundIndex_=ib?registry_->index[ib.Get()].lock():nullptr;
  boundStrides_[0]=stride;boundBase_=base;nativeBindingsReady_=true;return true;
 }
 bool TryNativeGeometry(const NativeIndexedDraw &draw,long &result){
  const auto *view=currentNativeGeometry;
  if(!nativeGeometryEnabled_||!view){++nativeGeometryStats_.noSource;return false;}
  ++nativeGeometryStats_.attempts;
  if(!view->Matches(draw,fixedState_.Value().vertexShader,GetNativeMeshSource().resource)){
   ++nativeGeometryStats_.mismatch;return false;
  }
  if(!CaptureNativeTextures()){++nativeGeometryStats_.prepareFallback;return false;}
  const auto key=view->Key();auto geometry=geometryCache_.Find(key);
  if(geometry){++nativeGeometryStats_.hits;++nativeSubmitStats_.cacheHits;renderer_.GeometryCacheResult(true,view->range,draw.primitives*3);}
  else {
   geometry=std::make_shared<DecodedGeometry>();
   const auto decodedBefore=nativeVertexStats_.decodedBytes;
   const bool share=nativeSharedVerticesEnabled_&&NativeSharedFits(*view,geometryCache_.Limit());
   if(nativeSharedVerticesEnabled_&&!share)++nativeVertexStats_.budgetBypass;
   HRESULT prepared=share?BuildNativeSharedGeometry(*view,nativeVertexPool_,*geometry,nativeVertexStats_):BuildNativeGeometry(*view,*geometry);
   // A nonfinite vertex outside this material's range must not reject an
   // otherwise valid original submesh. Preserve the checked local path.
   if(FAILED(prepared)&&share){++nativeVertexStats_.localFallback;prepared=BuildNativeGeometry(*view,*geometry);}
   if(FAILED(prepared)){++nativeGeometryStats_.prepareFallback;return false;}
   geometryCache_.Insert(key,geometry,view->recipe,view->recipe,&view->recipe->revision,&view->recipe->revision);
   ++nativeGeometryStats_.misses;++nativeSubmitStats_.cacheMisses;nativeGeometryStats_.decodedBytes+=geometry->sharedVertices?nativeVertexStats_.decodedBytes-decodedBefore:geometry->fixedVertices.size()*sizeof(FixedVertex);
   renderer_.GeometryCacheResult(false,view->range,draw.primitives*3);
  }
  const bool stagePacket=stagedScenePackets.load(std::memory_order_relaxed);std::optional<DrawPacket> local;
  StagedPacketGuard staging{renderer_,stagePacket};DrawPacket &packet=stagePacket?renderer_.StagePacket():local.emplace();
  packet.fvf=view->fvf;packet.primitive=packet.sourcePrimitive=D3DPT_TRIANGLELIST;
  if(geometry->sharedVertices)++nativeVertexStats_.draws;
  packet.geometry=std::move(geometry);packet.sourceIdentity=key;packet.sourceVertex=packet.sourceIndex=view->recipe;
  ++nativeGeometryStats_.accepted;++nativeSubmitStats_.accepted;
  result=Submit(std::move(packet),true);if(FAILED(result)){++nativeGeometryStats_.errors;++nativeSubmitStats_.errors;}
  return true;
 }
 bool TryNativeIndexed(const NativeIndexedDraw &draw,long &result){
  ++nativeSubmitStats_.attempts;
  const auto source=GetNativeMeshSource();
  if(!NativeSubmitContext(source,nativeSubmitEnabled_,renderer_.Offscreen(),fixedState_.Recording())){
   if(source.sorted)++nativeSortedAttempts_;
   if(!draw.geometryOnly&&NativeSortedSubmitContext(source,nativeSubmitEnabled_&&nativeSortedEnabled_,renderer_.Offscreen(),fixedState_.Recording())&&
      fixedState_.Capture(*native_.Get())&&NativeSubmitLayout(fixedState_.Value(),declarations_.find(fixedState_.Value().vertexShader)!=declarations_.end(),draw)&&
      (nativeBindingsReady_||CaptureNativeBindings())&&boundVertices_[0]&&boundIndex_&&boundStrides_[0]&&CaptureNativeTextures()){
    ++nativeSubmitStats_.accepted;++nativeSortedAccepted_;
    // Successful-unlock revisions can share immutable captured snapshots.
    // They do not enter the static mesh cache and are never reordered.
    result=BufferedDraw(static_cast<D3DPRIMITIVETYPE>(draw.primitive),draw.firstVertex,draw.vertices,draw.firstIndex,draw.primitives,true,true);
    if(FAILED(result))++nativeSubmitStats_.errors;
    return true; // A failed handled draw must not be repeated.
   }
   ++nativeSubmitStats_.contextFallback;return false;
  }
  if(!fixedState_.Ready())++nativeSubmitStats_.fixedRefresh;
  if(!fixedState_.Capture(*native_.Get())){++nativeSubmitStats_.stateFallback;return false;}
  const auto &fixed=fixedState_.Value();
  if(!NativeSubmitLayout(fixed,declarations_.find(fixed.vertexShader)!=declarations_.end(),draw)){
   ++nativeSubmitStats_.layoutFallback;return false;
  }
  stateMirror_.Capture([&](UINT state,DWORD *value){return native_->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state),value);},
   [&](UINT stage,UINT state,DWORD *value){return native_->GetTextureStageState(stage,static_cast<D3DTEXTURESTAGESTATETYPE>(state),value);});
  if(!NativeSubmitState(stateMirror_.render)){++nativeSubmitStats_.stateFallback;return false;}
  if(TryNativeGeometry(draw,result))return true;
  if(draw.geometryOnly)return false; // WW3D must bind/materialize this draw before compatibility fallback.
  if(!nativeBindingsReady_&&!CaptureNativeBindings()){++nativeSubmitStats_.bindingFallback;return false;}
  if(!NativeSubmitBindings(bool(boundVertices_[0]),bool(boundIndex_),boundStrides_[0],
       boundVertices_[0]?boundVertices_[0]->usage:0,boundIndex_?boundIndex_->usage:0)){
   ++nativeSubmitStats_.bindingFallback;return false;
  }
  ++nativeSubmitStats_.accepted;
  result=BufferedDraw(static_cast<D3DPRIMITIVETYPE>(draw.primitive),draw.firstVertex,draw.vertices,draw.firstIndex,draw.primitives,true,true);
  if(FAILED(result))++nativeSubmitStats_.errors;
  // Submission errors are handled here. Returning false would draw a second time.
  return true;
 }
 void RecordNativeSubmit(bool force=false){
  if(!force)++nativeSubmitSceneEnds_;
  if(!nativeSubmitSceneEnds_)return;
  if(!force&&nativeSubmitSceneEnds_<120&&std::chrono::steady_clock::now()-lastNativeStatsWrite_<std::chrono::seconds(1))return;
  lastNativeStatsWrite_=std::chrono::steady_clock::now();
  wchar_t profile[32768]{};DWORD length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",profile,32768);
  if(length&&length<32768){
   const auto path=std::filesystem::path(profile)/L"renderer-sorted-submit-v58.csv";
   std::error_code ec;const bool header=!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)==0;
   GenZHLoad::AppendLog out(path,std::ios::app);
   if(out){if(header)out<<"pid,utc_epoch_ms,scene_ends,sorted_attempts,sorted_mirrored_accepted,fixed_decode_vertices,fixed_decode_bytes\n";
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    out<<GetCurrentProcessId()<<','<<epoch<<','<<nativeSubmitSceneEnds_<<','<<nativeSortedAttempts_<<','<<nativeSortedAccepted_<<','<<fixedDecodeVertices_<<','<<fixedDecodeVertices_*sizeof(FixedVertex)<<'\n';}
  }
  if(length&&length<32768){
   const auto path=std::filesystem::path(profile)/L"renderer-submit-v24.csv";
   std::error_code ec;const bool header=!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)==0;
   GenZHLoad::AppendLog out(path,std::ios::app);
   if(out){
    if(header)out<<"pid,utc_epoch_ms,scene_ends,attempts,accepted,context_fallback,state_fallback,binding_fallback,layout_fallback,fixed_refresh,binding_refresh,geometry_hits,geometry_misses,errors\n";
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto &s=nativeSubmitStats_;
    out<<GetCurrentProcessId()<<','<<epoch<<','<<nativeSubmitSceneEnds_<<','<<s.attempts<<','<<s.accepted<<','<<s.contextFallback<<','<<s.stateFallback<<','<<s.bindingFallback<<','<<s.layoutFallback<<','<<s.fixedRefresh<<','<<s.bindingRefresh<<','<<s.cacheHits<<','<<s.cacheMisses<<','<<s.errors<<'\n';
   }
  }
  if(length&&length<32768){
   const auto path=std::filesystem::path(profile)/L"renderer-geometry-v25.csv";
   std::error_code ec;const bool header=!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)==0;
   GenZHLoad::AppendLog out(path,std::ios::app);
   if(out){
    if(header)out<<"pid,utc_epoch_ms,scene_ends,attempts,accepted,no_source,mismatch,prepare_fallback,geometry_hits,geometry_misses,decoded_vertex_bytes,texture_refresh,errors\n";
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto &s=nativeGeometryStats_;
    out<<GetCurrentProcessId()<<','<<epoch<<','<<nativeSubmitSceneEnds_<<','<<s.attempts<<','<<s.accepted<<','<<s.noSource<<','<<s.mismatch<<','<<s.prepareFallback<<','<<s.hits<<','<<s.misses<<','<<s.decodedBytes<<','<<s.textureRefresh<<','<<s.errors<<'\n';
   }
  }
  if(length&&length<32768){
   const auto path=std::filesystem::path(profile)/L"renderer-legacy-v26.csv";
   std::error_code ec;const bool header=!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)==0;
   GenZHLoad::AppendLog out(path,std::ios::app);
   if(out){
    if(header)out<<"pid,utc_epoch_ms,scene_ends,reserved_buffers_total,reserved_bytes_total,materialized_buffers_total,materialized_bytes_total,packed_bytes_total,failed_flushes_total,live_deferred_buffers,live_deferred_bytes\n";
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto &s=deferredLegacyStats;
    out<<GetCurrentProcessId()<<','<<epoch<<','<<nativeSubmitSceneEnds_<<','<<s.reservedBuffers<<','<<s.reservedBytes<<','<<s.materializedBuffers<<','<<s.materializedBytes<<','<<s.packedBytes<<','<<s.failedFlushes<<','<<s.liveDeferredBuffers<<','<<s.liveDeferredBytes<<'\n';
   }
  }
  if(length&&length<32768){
   const auto path=std::filesystem::path(profile)/L"renderer-vertices-v27.csv";
   std::error_code ec;const bool header=!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)==0;
   GenZHLoad::AppendLog out(path,std::ios::app);
   if(out){
    if(header)out<<"pid,utc_epoch_ms,scene_ends,pool_hits,pool_misses,decoded_vertex_bytes,selected_submesh_bytes,shared_draws,build_errors,budget_bypass,local_fallback,pool_entries,cache_owned_bytes\n";
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto &s=nativeVertexStats_;
    out<<GetCurrentProcessId()<<','<<epoch<<','<<nativeSubmitSceneEnds_<<','<<s.hits<<','<<s.misses<<','<<s.decodedBytes<<','<<s.submeshBytes<<','<<s.draws<<','<<s.errors<<','<<s.budgetBypass<<','<<s.localFallback<<','<<nativeVertexPool_.Size()<<','<<geometryCache_.Bytes()<<'\n';
   }
  }
  if(length&&length<32768){
   const auto path=std::filesystem::path(profile)/L"renderer-sorted-geometry-v59.csv";
   std::error_code ec;const bool header=!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)==0;
   GenZHLoad::AppendLog out(path,std::ios::app);const auto &v=sortedBufferPool_.vertices;const auto &i=sortedBufferPool_.indices;
   if(out){if(header)out<<"pid,utc_epoch_ms,scene_ends,shared_draws,vertex_hits,vertex_misses,index_hits,index_misses,vertex_decoded_bytes,index_decoded_bytes,budget_bypass,build_errors,cache_owned_bytes,cache_entries\n";
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    out<<GetCurrentProcessId()<<','<<epoch<<','<<nativeSubmitSceneEnds_<<','<<sortedBufferPool_.draws<<','<<v.hits<<','<<v.misses<<','<<i.hits<<','<<i.misses<<','<<v.decodedBytes<<','<<i.decodedBytes<<','<<(v.bypassed+i.bypassed)<<','<<sortedBufferPool_.errors<<','<<sortedBufferPool_.Bytes()<<','<<sortedBufferPool_.Size()<<'\n';}
   GenZHLoad::Record("two_uv_sorted_v77","draws="+std::to_string(sortedBufferPool_.twoUVDraws)+" decoded_bytes_saved="+std::to_string(sortedBufferPool_.twoUVDecodedBytesSaved)+" errors="+std::to_string(sortedBufferPool_.errors),0,0,false);
   GenZHLoad::Record("snapshot_recycle_v87","vertex_misses="+std::to_string(v.misses)+" vertex_recycled="+std::to_string(v.recycled)+" index_misses="+std::to_string(i.misses)+
    " index_recycled="+std::to_string(i.recycled)+" recycle="+(capturedSnapshotRecycle.load(std::memory_order_relaxed)?"on":"off"),0,0,false);
  }
  const auto &d=deferredFixed_.counts;
  GenZHLoad::Record("deferred_fixed_v74","scopes="+std::to_string(d.scopes)+" deferred="+std::to_string(d.deferred)+
   " native_writes="+std::to_string(d.nativeWrites)+" synchronizations="+std::to_string(d.synchronizations)+" failures="+std::to_string(d.failures),0);
  sortedBufferPool_.ClearStats();
  nativeSubmitStats_={};nativeGeometryStats_={};nativeVertexStats_={};nativeSubmitSceneEnds_=0;
  nativeSortedAttempts_=nativeSortedAccepted_=fixedDecodeVertices_=0;
 }
 HRESULT WrapSurface(HRESULT hr,IDirect3DSurface8 **out){
  if(FAILED(hr)||!out||!*out)return hr;auto *original=*out;auto it=registry_->nativeProxies.find(original);
  if(it!=registry_->nativeProxies.end()){hr=it->second->QueryInterface(IID_IDirect3DSurface8,reinterpret_cast<void**>(out));original->Release();return hr;}
  ComPtr<IDirect3DBaseTexture8> texture;std::shared_ptr<TextureData> data;
  if(SUCCEEDED(original->GetContainer(IID_IDirect3DBaseTexture8,reinterpret_cast<void**>(texture.GetAddressOf()))))data=TextureInfo(texture.Get());
  *out=new SurfaceProxy(original,this,registry_,std::move(data));return S_OK;
 }
 struct BlockResources{std::vector<std::shared_ptr<BufferData>> buffers;std::vector<std::shared_ptr<TextureData>> textures;};
 void RetainBlock(DWORD token){auto &block=blocks_[token];block={};
  for(const auto &entry:registry_->vertex)if(auto data=entry.second.lock())block.buffers.push_back(std::move(data));
  for(const auto &entry:registry_->index)if(auto data=entry.second.lock())block.buffers.push_back(std::move(data));
  for(const auto &entry:registry_->textures)if(auto data=entry.second.lock())block.textures.push_back(std::move(data));
 }
 ~DeviceProxy(){try{RecordNativeSubmit(true);}catch(...){}nativeMeshDispatch.Disconnect(static_cast<IDirect3DDevice8*>(this),this);for(UINT i=0;i<8;++i)native_->SetTexture(i,nullptr);for(UINT i=0;i<16;++i)native_->SetStreamSource(i,nullptr,0);native_->SetIndices(nullptr,0);}
 template<class T>T *Unwrap(T *pointer){if(!pointer)return nullptr;auto found=registry_->proxies.find(pointer);return found==registry_->proxies.end()?pointer:static_cast<T*>(found->second);}
 std::shared_ptr<TextureData> TextureInfo(IDirect3DBaseTexture8 *native){
  if(!native)return {};auto &weak=registry_->textures[native];auto data=weak.lock();
  if(!data){data=std::make_shared<TextureData>();data->native=native;weak=data;}return data;
 }
 HRESULT Report(HRESULT hr,const std::string &error){D3D11Diagnostic(("D3D11 draw rejected: "+error).c_str());return hr;}
 HRESULT Submit(DrawPacket &&packet,bool direct=false){
  if(FAILED(deferredFixed_.Status()))return deferredFixed_.Status();
  if(!direct){const auto sync=SyncFixedWrites();if(FAILED(sync))return sync;}
  stateMirror_.Capture([&](UINT state,DWORD *value){return native_->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state),value);},
   [&](UINT stage,UINT state,DWORD *value){return native_->GetTextureStageState(stage,static_cast<D3DTEXTURESTAGESTATETYPE>(state),value);});
  packet.stateSnapshot=stateMirror_.Snapshot();packet.stateRevision=stateMirror_.revision;
  HRESULT hr=renderer_.Snapshot(native_.Get(),packet,true,direct?&fixedState_.Value():nullptr);if(FAILED(hr))return hr;
  if(direct)packet.textures=boundTextures_;else for(UINT stage=0;stage<8;++stage){ComPtr<IDirect3DBaseTexture8> texture;native_->GetTexture(stage,texture.GetAddressOf());packet.textures[stage]=TextureInfo(texture.Get());}
  return renderer_.Draw(std::move(packet));
 }
 void CaptureStateMirror(){
  stateMirror_.Capture([&](UINT state,DWORD *value){return native_->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state),value);},
   [&](UINT stage,UINT state,DWORD *value){return native_->GetTextureStageState(stage,static_cast<D3DTEXTURESTAGESTATETYPE>(state),value);});
 }
 // Live private data read by CaptureTextures/CaptureTerrain for textures[0].
 void ProbeSortedRun(SortedRunProbe &probe){
  probe=SortedRunProbe{};
  if(!boundTextures_[0]||!boundTextures_[0]->native)return;
  auto *texture=boundTextures_[0]->native.Get();
  DWORD bytes=0;texture->GetPrivateData(D3D11TerrainAtlasGUID,nullptr,&bytes);probe.atlasBytes=bytes;
  DWORD size=static_cast<DWORD>(probe.name.size());const HRESULT hr=texture->GetPrivateData(D3D11TextureNameGUID,probe.name.data(),&size);
  if(hr==D3DERR_MOREDATA){probe.usable=false;return;} // longer names keep the full path
  probe.nameValid=SUCCEEDED(hr)&&size&&size<=probe.name.size()&&probe.name[size-1]==0;probe.nameSize=probe.nameValid?size:0;
 }
 bool MatchesSortedRun(const SortedRunTemplate &t,const DrawPacket &packet,const NativeMeshSource &source,
                       const std::array<std::uint64_t,8> &context,const SortedRunProbe &probe)const{
  if(!SameMeshSource(t.source,source)||t.fvf!=packet.fvf||t.primitive!=packet.primitive||t.context!=context||!SameProbe(t.probe,probe))return false;
  for(UINT i=0;i<8;++i){const auto *texture=boundTextures_[i].get();
   if(t.textures[i]!=texture||t.revisions[i]!=(texture?texture->revision:0))return false;}
  return std::memcmp(&t.fixed,&fixedState_.Value(),sizeof(t.fixed))==0&&t.render==stateMirror_.render&&t.stages==stateMirror_.stages;
 }
 // Full capture into a scratch packet (never queued) and byte comparison.
 void VerifySortedRun(const DrawPacket &templated){
  DrawPacket check;check.primitive=templated.primitive;check.sourcePrimitive=templated.sourcePrimitive;check.fvf=templated.fvf;
  check.geometry=templated.geometry;check.sourceIdentity=templated.sourceIdentity;check.sourceVertex=templated.sourceVertex;check.sourceIndex=templated.sourceIndex;
  check.stateSnapshot=stateMirror_.Snapshot();check.stateRevision=stateMirror_.revision;
  if(FAILED(renderer_.Snapshot(native_.Get(),check,true,&fixedState_.Value())))return;
  check.textures=boundTextures_;if(FAILED(renderer_.CaptureTexturesForCheck(check)))return;
  ++sortedRunTemplateCounts.verified;
  if(!SameCapturedPacket(check,templated)&&sortedRunTemplateCounts.verifyMismatches++<8)
   GenZHLoad::Record("sorted_run_template_mismatch_v84","mesh="+std::to_string(check.nativeSource.mesh)+" pass="+std::to_string(check.nativeSource.pass),0);
 }
 HRESULT SubmitSortedRun(DrawPacket &&packet){
  if(!sortedRunTemplatesEnabled.load(std::memory_order_relaxed)||!fixedState_.Ready()){++sortedRunTemplateCounts.bypass;return Submit(std::move(packet),true);}
  if(FAILED(deferredFixed_.Status()))return deferredFixed_.Status();
  CaptureStateMirror();
  const auto source=GetNativeMeshSource();const auto context=renderer_.CaptureContext();
  SortedRunProbe probe;ProbeSortedRun(probe);
  if(probe.usable)for(const auto &entry:sortedTemplates_)if(MatchesSortedRun(*entry,packet,source,context,probe)){
   ++sortedRunTemplateCounts.hits;
   ApplySortedRunTemplate(entry->packet,packet);
   if(sortedRunTemplateVerify.load(std::memory_order_relaxed))VerifySortedRun(packet);
   return renderer_.Draw(std::move(packet),true);
  }
  ++sortedRunTemplateCounts.misses;
  const auto before=renderer_.ScenePacketCount();
  HRESULT hr=Submit(std::move(packet),true);
  if(SUCCEEDED(hr)&&probe.usable&&renderer_.ScenePacketCount()==before+1&&sortedTemplates_.size()<64){
   const auto &queued=renderer_.LastScenePacket();
   if(queued.geometry&&!queued.terrain&&!queued.outputOverlay&&queued.indices.empty()&&queued.vertices.empty()&&queued.fixedVertices.empty()){
    auto entry=std::make_unique<SortedRunTemplate>();
    entry->source=source;entry->fvf=queued.fvf;entry->primitive=queued.primitive;entry->context=context;entry->probe=probe;
    for(UINT i=0;i<8;++i){entry->textures[i]=boundTextures_[i].get();entry->revisions[i]=boundTextures_[i]?boundTextures_[i]->revision:0;}
    entry->fixed=fixedState_.Value();entry->render=stateMirror_.render;entry->stages=stateMirror_.stages;
    ApplySortedRunTemplate(queued,entry->packet);
    sortedTemplates_.push_back(std::move(entry));++sortedRunTemplateCounts.stores;
   }
  }
  return hr;
 }
 void RecordSortedRunTemplates(){
  const double now=GenZHLoad::Seconds();if(now<sortedTemplateReport_)return;sortedTemplateReport_=now+1;
  const auto &c=sortedRunTemplateCounts;
  GenZHLoad::Record("sorted_run_templates_v84","enabled="+std::to_string(sortedRunTemplatesEnabled.load())+" verify="+std::to_string(sortedRunTemplateVerify.load())+
   " hits="+std::to_string(c.hits)+" misses="+std::to_string(c.misses)+" stores="+std::to_string(c.stores)+" bypass="+std::to_string(c.bypass)+
   " verified="+std::to_string(c.verified)+" verify_mismatches="+std::to_string(c.verifyMismatches),0);
 }
 std::vector<std::unique_ptr<SortedRunTemplate>> sortedTemplates_;double sortedTemplateReport_=0;
 HRESULT BufferedDraw(D3DPRIMITIVETYPE type,UINT firstVertex,UINT vertices,UINT firstIndex,UINT primitives,bool indexed,bool direct=false){
  UINT count=PrimitiveVertexCount(type,primitives);if(!count)return D3DERR_INVALIDCALL;
  ComPtr<IDirect3DVertexBuffer8> vb;UINT stride=0;HRESULT hr=S_OK;std::shared_ptr<BufferData> vertexData,indexData;UINT base=0;
  if(direct){vertexData=boundVertices_[0];indexData=boundIndex_;stride=boundStrides_[0];base=boundBase_;}
  else {
   hr=native_->GetStreamSource(0,vb.GetAddressOf(),&stride);if(FAILED(hr)||!vb)return D3DERR_INVALIDCALL;
   vertexData=registry_->vertex[vb.Get()].lock();
   if(indexed){ComPtr<IDirect3DIndexBuffer8> ib;native_->GetIndices(ib.GetAddressOf(),&base);indexData=registry_->index[ib.Get()].lock();}
  }
  if(!vertexData)return Report(E_FAIL,"vertex buffer has no captured CPU contents");
  if(indexed&&!indexData)return Report(E_FAIL,"index buffer has no captured CPU contents");
  // perf88: build the packet in the scene queue's next slot (no move of the
  // multi-kilobyte packet); the guard destroys it wherever Draw did not commit it.
  const bool stagePacket=stagedScenePackets.load(std::memory_order_relaxed);std::optional<DrawPacket> local;
  StagedPacketGuard staging{renderer_,stagePacket};DrawPacket &packet=stagePacket?renderer_.StagePacket():local.emplace();
  packet.primitive=packet.sourcePrimitive=type;
  if(direct)packet.fvf=fixedState_.Value().vertexShader;else native_->GetVertexShader(&packet.fvf);std::string error;

  if(std::uint64_t(base)+firstVertex>UINT_MAX)return E_INVALIDARG;
  UINT start=base+firstVertex;
  auto declaration=declarations_.find(packet.fvf);
  // Only fixed-layout, static indexed triangle lists have an immutable decode.
  // Skinning/declaration streams, UP draws and dynamic buffers keep their checked path.
  const bool cacheable=indexed&&type==D3DPT_TRIANGLELIST&&declaration==declarations_.end()&&stride&&
   !(vertexData->usage&D3DUSAGE_DYNAMIC)&&!(indexData->usage&D3DUSAGE_DYNAMIC);
  // Capture provenance even for dynamic draws. Decoded packet data is still
  // immutable; later native unlocks do not rewrite an already submitted draw.
  GeometryKey key={reinterpret_cast<std::uintptr_t>(vertexData.get()),vertexData->revision,
       reinterpret_cast<std::uintptr_t>(indexData.get()),indexData?indexData->revision:0,
       packet.fvf,stride,start,vertices,firstIndex,count,base,indexData?static_cast<std::uint64_t>(indexData->format):0};
  packet.sourceIdentity=key;packet.sourceVertex=vertexData;packet.sourceIndex=indexData;
  if(cacheable){
   if(auto cached=geometryCache_.Find(key)){
    packet.geometry=std::move(cached);renderer_.GeometryCacheResult(true,vertices,count);if(direct)++nativeSubmitStats_.cacheHits;return Submit(std::move(packet),direct);
   }
  }
  if(indexed){
   hr=DecodeIndices(indexData->bytes.data(),indexData->bytes.size(),indexData->format,firstIndex,count,base,static_cast<UINT>(vertexData->bytes.size()/stride),packet.indices);
   if(FAILED(hr))return hr;
  }
  const auto sortedSource=GetNativeMeshSource();
  if(direct&&indexed&&!cacheable&&sharedSortedGeometryEnabled_&&declaration==declarations_.end()&&
     type==D3DPT_TRIANGLELIST&&NativeSortedSubmitContext(sortedSource,nativeSortedEnabled_,renderer_.Offscreen(),fixedState_.Recording())){
   SortedBufferView view;view.compactTwoUV=twoUVSortedEnabled_;view.vertexData=vertexData->bytes.data();view.indexData=indexData->bytes.data();
   view.vertexBytes=vertexData->bytes.size();view.indexBytes=indexData->bytes.size();
   view.vertexOwner=vertexData;view.indexOwner=indexData;view.vertexRevision=vertexData->revision;view.indexRevision=indexData->revision;
   view.stride=stride;view.fvf=packet.fvf;view.firstVertex=start;view.vertexCount=vertices;
   if(sortedSource.sortedVertexCount){
    const auto used=std::uint64_t(base)+sortedSource.sortedVertexCount;
    if(used>=std::uint64_t(start)+vertices&&used<=vertexData->bytes.size()/stride)
     view.vertexBytes=static_cast<std::size_t>(used)*stride;
   }
   view.firstIndex=firstIndex;view.indexCount=count;view.base=base;view.indexFormat=indexData->format;
   auto geometry=std::make_shared<DecodedGeometry>();
   const auto shared=BuildSortedBufferGeometry(view,std::move(packet.indices),sortedBufferPool_,*geometry,error);
   if(shared==S_OK){packet.geometry=std::move(geometry);packet.indices.clear();return SubmitSortedRun(std::move(packet));}
   if(FAILED(shared)){++sortedBufferPool_.errors;return Report(shared,"shared sorted buffer: "+error);}
   // A cache budget bypass retains the exact sparse compatibility path.
  }
  if(declaration!=declarations_.end()){
   packet.vertexShader=packet.fvf;packet.fvf=0;std::array<VertexStreamSpan,16> spans{};
   std::array<std::shared_ptr<BufferData>,16> keep;
   for(UINT stream=0;stream<16;++stream){ComPtr<IDirect3DVertexBuffer8> buffer;UINT streamStride=0;
    if(SUCCEEDED(native_->GetStreamSource(stream,buffer.GetAddressOf(),&streamStride))&&buffer){keep[stream]=registry_->vertex[buffer.Get()].lock();if(keep[stream])spans[stream]={keep[stream]->bytes.data(),keep[stream]->bytes.size(),streamStride};}}
   hr=indexed?DecodeReferencedDeclaration(declaration->second,spans,start,vertices,packet.indices,packet.vertices,error):
      DecodeDeclaration(declaration->second,spans,start,vertices,packet.vertices,error);
  }else if(indexed&&!cacheable){
   hr=DecodeReferencedFixedVertices(vertexData->bytes.data(),vertexData->bytes.size(),stride,packet.fvf,start,vertices,packet.indices,packet.fixedVertices,error);
   if(SUCCEEDED(hr))fixedDecodeVertices_+=packet.fixedVertices.size();
  }else hr=indexed?DecodeReferencedVertices(vertexData->bytes.data(),vertexData->bytes.size(),stride,packet.fvf,start,vertices,packet.indices,packet.vertices,error):
     DecodeVertices(vertexData->bytes.data(),vertexData->bytes.size(),stride,packet.fvf,start,vertices,packet.vertices,error);
  if(FAILED(hr))return Report(hr,error);
  if(indexed&&vertices>4096&&packet.VertexCount()<vertices/2){
   static unsigned gatherAuditCount=0;
   if(gatherAuditCount++<64)GenZHLoad::Record("compatibility_indexed_gather",
      std::to_string(vertices)+" -> "+std::to_string(packet.VertexCount())+" referenced vertices",0,packet.VertexCount()*(packet.CompactVertices()?sizeof(FixedVertex):sizeof(LegacyVertex)));
  }
  if(cacheable){
   packet.geometry=std::make_shared<DecodedGeometry>();
   packet.geometry->vertices=std::move(packet.vertices);packet.geometry->indices=std::move(packet.indices);
   packet.geometry->Compact();geometryCache_.Insert(key,packet.geometry,vertexData,indexData,&vertexData->revision,&indexData->revision);
   renderer_.GeometryCacheResult(false,vertices,count);if(direct)++nativeSubmitStats_.cacheMisses;
  }
  return Submit(std::move(packet),direct);
 }
 GeometryCache geometryCache_;SortedBufferGeometryPool sortedBufferPool_;bool sharedSortedGeometryEnabled_=true,twoUVSortedEnabled_=true;
 std::uint64_t nativeSortedAttempts_=0,nativeSortedAccepted_=0,fixedDecodeVertices_=0;
 NativeVertexPool nativeVertexPool_;NativeVertexStats nativeVertexStats_;bool nativeSharedVerticesEnabled_=true;
 GeometryMemoryMonitor geometryMemory_;
 NativeStateMirror stateMirror_;
 NativeFixedStateMirror fixedState_;
 DeferredFixedWrites deferredFixed_;bool deferFixedEnabled_=true;
 bool nativeBindingsReady_=false,nativeTexturesReady_=false,nativeSubmitEnabled_=true,nativeGeometryEnabled_=true,nativeSortedEnabled_=true;
 std::array<UINT,16> boundStrides_{};UINT boundBase_=0;
 struct NativeSubmitStats{std::uint64_t attempts=0,accepted=0,contextFallback=0,stateFallback=0,bindingFallback=0,layoutFallback=0,fixedRefresh=0,bindingRefresh=0,cacheHits=0,cacheMisses=0,errors=0;} nativeSubmitStats_;
 std::uint64_t nativeSubmitSceneEnds_=0;
 std::chrono::steady_clock::time_point lastNativeStatsWrite_=std::chrono::steady_clock::now();
 struct NativeGeometryStats{std::uint64_t attempts=0,accepted=0,noSource=0,mismatch=0,prepareFallback=0,hits=0,misses=0,decodedBytes=0,textureRefresh=0,errors=0;} nativeGeometryStats_;
 std::atomic<ULONG> refs_{1};ComPtr<IDirect3DDevice8> native_;std::shared_ptr<Registry> registry_;
 std::unordered_map<DWORD,std::vector<DWORD>> declarations_;
 std::unordered_map<DWORD,BlockResources> blocks_;
 std::array<std::shared_ptr<BufferData>,16> boundVertices_{};std::shared_ptr<BufferData> boundIndex_;
 std::array<std::shared_ptr<TextureData>,8> boundTextures_{};
 CopyRectsRepeats<TextureData> copyRepeats_;
 D3DGAMMARAMP gamma_{};
 CompatibilityRenderer renderer_;HWND window_=nullptr;UINT width_=0,height_=0;
};
}

extern "C" void *GenZH_UnwrapBridgeResource(void *resource,int kind,bool changed) {
 if(!resource)return nullptr;
 using namespace GenZH::Modern;
 if(kind==D3DRTYPE_SURFACE) { if(auto *p=dynamic_cast<SurfaceProxy*>(static_cast<IDirect3DSurface8*>(resource)))return p->ResourceNative(changed); }
 if(kind==D3DRTYPE_VOLUME) { if(auto *p=dynamic_cast<VolumeProxy*>(static_cast<IDirect3DVolume8*>(resource)))return p->ResourceNative(changed); }
 if(kind==D3DRTYPE_TEXTURE) { if(auto *p=dynamic_cast<TextureProxy*>(static_cast<IDirect3DTexture8*>(resource)))return p->ResourceNative(changed); }
 if(kind==D3DRTYPE_CUBETEXTURE) { if(auto *p=dynamic_cast<CubeTextureProxy*>(static_cast<IDirect3DCubeTexture8*>(resource)))return p->ResourceNative(changed); }
 if(kind==D3DRTYPE_VOLUMETEXTURE) { if(auto *p=dynamic_cast<VolumeTextureProxy*>(static_cast<IDirect3DVolumeTexture8*>(resource)))return p->ResourceNative(changed); }
 return resource;
}

// Once a bridge device is connected it serves the rest of the process. Texture
// Peek calls this per apply; the environment query allocated on every call.
static std::atomic<bool> d3d11DeviceConnected{false};
bool IsD3D11RendererRequested(){
 if(d3d11DeviceConnected.load(std::memory_order_relaxed))return true;
 char choice[32]{};return GetEnvironmentVariableA("GENZH_RENDERER",choice,sizeof(choice))==5&&std::strcmp(choice,"d3d11")==0;
}
void ConnectD3D11Device(IDirect3DDevice8 **device,void *window){
 if(!IsD3D11RendererRequested())return;
 if(!device||!*device)throw std::runtime_error("D3D11 connection has no asset device");
 auto *connected=new GenZH::Modern::DeviceProxy(*device);HRESULT hr=connected->Initialize(static_cast<HWND>(window));
 if(FAILED(hr)){std::string error=connected->Error();connected->Release();throw std::runtime_error("D3D11 connection failed: "+error);}
 (*device)->Release();*device=connected;d3d11DeviceConnected.store(true,std::memory_order_relaxed);D3D11Diagnostic("D3D11 device connection initialized");
}
