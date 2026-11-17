// SPDX-License-Identifier: GPL-3.0-or-later
// D3DX8 signatures over AMD64 resources. No 32-bit utility DLL is loaded.
#include <d3dx8.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <wrl/client.h>
extern "C" void *GenZH_UnwrapBridgeResource(void *,int,bool);
extern "C" void *GenZH_Resource9Surface(void *);
extern "C" void *GenZH_Resource9Volume(void *);
extern "C" void *GenZH_Resource9Texture(void *,int);
using Microsoft::WRL::ComPtr;
namespace {
template<class Function> Function Utility(const char *name) {
    static HMODULE module=LoadLibraryExW(L"d3dx9_43.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<Function>(GetProcAddress(module,name)) : nullptr;
}
void *Surface(IDirect3DSurface8 *surface) { return GenZH_Resource9Surface(GenZH_UnwrapBridgeResource(surface,D3DRTYPE_SURFACE,false)); }
void *Volume(IDirect3DVolume8 *volume) { return GenZH_Resource9Volume(GenZH_UnwrapBridgeResource(volume,D3DRTYPE_VOLUME,false)); }
void Changed(void *resource,int type,HRESULT status) { if(SUCCEEDED(status))GenZH_UnwrapBridgeResource(resource,type,true); }
}
HRESULT WINAPI D3DXGetErrorStringA(HRESULT code,LPSTR text,UINT length) {
    if(!text||!length)return E_INVALIDARG;
    std::snprintf(text,length,"Graphics status 0x%08lx",static_cast<unsigned long>(code));return S_OK;
}
UINT WINAPI D3DXGetFVFVertexSize(DWORD fvf) {
    using F=UINT(WINAPI*)(DWORD);auto fn=Utility<F>("D3DXGetFVFVertexSize");return fn?fn(fvf):0;
}
D3DXMATRIX *WINAPI D3DXMatrixRotationZ(D3DXMATRIX *out,FLOAT angle) {
    if(!out)return nullptr;
    *out=D3DXMATRIX(std::cos(angle),std::sin(angle),0,0,-std::sin(angle),std::cos(angle),0,0,0,0,1,0,0,0,0,1);return out;
}
D3DXMATRIX *WINAPI D3DXMatrixMultiply(D3DXMATRIX *out,const D3DXMATRIX *a,const D3DXMATRIX *b) {
    using F=D3DXMATRIX*(WINAPI*)(D3DXMATRIX*,const D3DXMATRIX*,const D3DXMATRIX*);
    auto fn=Utility<F>("D3DXMatrixMultiply");return fn?fn(out,a,b):nullptr;
}
D3DXMATRIX *WINAPI D3DXMatrixInverse(D3DXMATRIX *out,FLOAT *det,const D3DXMATRIX *m) {
    using F=D3DXMATRIX*(WINAPI*)(D3DXMATRIX*,FLOAT*,const D3DXMATRIX*);
    auto fn=Utility<F>("D3DXMatrixInverse");return fn?fn(out,det,m):nullptr;
}
D3DXMATRIX *WINAPI D3DXMatrixTranspose(D3DXMATRIX *out,const D3DXMATRIX *m) {
    using F=D3DXMATRIX*(WINAPI*)(D3DXMATRIX*,const D3DXMATRIX*);
    auto fn=Utility<F>("D3DXMatrixTranspose");return fn?fn(out,m):nullptr;
}
D3DXMATRIX *WINAPI D3DXMatrixScaling(D3DXMATRIX *out,FLOAT x,FLOAT y,FLOAT z) {
    if(!out)return nullptr;D3DXMatrixIdentity(out);out->_11=x;out->_22=y;out->_33=z;return out;
}
D3DXMATRIX *WINAPI D3DXMatrixTranslation(D3DXMATRIX *out,FLOAT x,FLOAT y,FLOAT z) {
    if(!out)return nullptr;D3DXMatrixIdentity(out);out->_41=x;out->_42=y;out->_43=z;return out;
}
D3DXVECTOR4 *WINAPI D3DXVec4Transform(D3DXVECTOR4 *out,const D3DXVECTOR4 *v,const D3DXMATRIX *m) {
    using F=D3DXVECTOR4*(WINAPI*)(D3DXVECTOR4*,const D3DXVECTOR4*,const D3DXMATRIX*);
    auto fn=Utility<F>("D3DXVec4Transform");return fn?fn(out,v,m):nullptr;
}
HRESULT WINAPI D3DXAssembleShader(LPCVOID source,UINT length,DWORD flags,LPD3DXBUFFER *constants,
    LPD3DXBUFFER *compiled,LPD3DXBUFFER *errors) {
    if(constants)*constants=nullptr;
    using F=HRESULT(WINAPI*)(LPCSTR,UINT,const void*,void*,DWORD,LPD3DXBUFFER*,LPD3DXBUFFER*);
    auto fn=Utility<F>("D3DXAssembleShader");
    // D3D8 supplies its input declaration separately. D3D9 validation would
    // reject those original vs_1_1 inputs before the declaration is applied.
    return fn?fn(static_cast<LPCSTR>(source),length,nullptr,nullptr,flags|2u,compiled,errors):E_FAIL;
}
D3DXVECTOR4 *WINAPI D3DXVec3Transform(D3DXVECTOR4 *out,const D3DXVECTOR3 *v,const D3DXMATRIX *m) {
    if(!out||!v||!m)return nullptr;
    const D3DXVECTOR3 p=*v;
    *out=D3DXVECTOR4(p.x*m->_11+p.y*m->_21+p.z*m->_31+m->_41,
        p.x*m->_12+p.y*m->_22+p.z*m->_32+m->_42,
        p.x*m->_13+p.y*m->_23+p.z*m->_33+m->_43,
        p.x*m->_14+p.y*m->_24+p.z*m->_34+m->_44);return out;
}
HRESULT WINAPI D3DXCreateTexture(IDirect3DDevice8 *d,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture8 **out) {
    if(!d||!out)return E_INVALIDARG;if(format==D3DFMT_UNKNOWN)format=D3DFMT_A8R8G8B8;
    return d->CreateTexture(w,h,levels==D3DX_DEFAULT?0:levels,usage,format,pool,out);
}
HRESULT WINAPI D3DXCreateCubeTexture(IDirect3DDevice8 *d,UINT size,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DCubeTexture8 **out) {
    if(!d||!out)return E_INVALIDARG;return d->CreateCubeTexture(size,levels==D3DX_DEFAULT?0:levels,usage,format,pool,out);
}
HRESULT WINAPI D3DXCreateVolumeTexture(IDirect3DDevice8 *d,UINT w,UINT h,UINT depth,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DVolumeTexture8 **out) {
    if(!d||!out)return E_INVALIDARG;return d->CreateVolumeTexture(w,h,depth,levels==D3DX_DEFAULT?0:levels,usage,format,pool,out);
}
HRESULT WINAPI D3DXLoadSurfaceFromSurface(IDirect3DSurface8 *dst,const PALETTEENTRY *dp,const RECT *dr,
    IDirect3DSurface8 *src,const PALETTEENTRY *sp,const RECT *sr,DWORD filter,D3DCOLOR key) {
    using F=HRESULT(WINAPI*)(void*,const PALETTEENTRY*,const RECT*,void*,const PALETTEENTRY*,const RECT*,DWORD,D3DCOLOR);
    auto fn=Utility<F>("D3DXLoadSurfaceFromSurface");if(!fn||!dst||!src)return E_INVALIDARG;
    HRESULT hr=fn(Surface(dst),dp,dr,Surface(src),sp,sr,filter,key);Changed(dst,D3DRTYPE_SURFACE,hr);return hr;
}
HRESULT WINAPI D3DXLoadSurfaceFromMemory(IDirect3DSurface8 *dst,const PALETTEENTRY *dp,const RECT *dr,
    const void *src,D3DFORMAT format,UINT pitch,const PALETTEENTRY *sp,const RECT *sr,DWORD filter,D3DCOLOR key) {
    using F=HRESULT(WINAPI*)(void*,const PALETTEENTRY*,const RECT*,const void*,D3DFORMAT,UINT,const PALETTEENTRY*,const RECT*,DWORD,D3DCOLOR);
    auto fn=Utility<F>("D3DXLoadSurfaceFromMemory");if(!fn||!dst||!src)return E_INVALIDARG;
    HRESULT hr=fn(Surface(dst),dp,dr,src,format,pitch,sp,sr,filter,key);Changed(dst,D3DRTYPE_SURFACE,hr);return hr;
}
HRESULT WINAPI D3DXLoadVolumeFromVolume(IDirect3DVolume8 *dst,const PALETTEENTRY *dp,const D3DBOX *dr,
    IDirect3DVolume8 *src,const PALETTEENTRY *sp,const D3DBOX *sr,DWORD filter,D3DCOLOR key) {
    using F=HRESULT(WINAPI*)(void*,const PALETTEENTRY*,const D3DBOX*,void*,const PALETTEENTRY*,const D3DBOX*,DWORD,D3DCOLOR);
    auto fn=Utility<F>("D3DXLoadVolumeFromVolume");if(!fn||!dst||!src)return E_INVALIDARG;
    HRESULT hr=fn(Volume(dst),dp,dr,Volume(src),sp,sr,filter,key);Changed(dst,D3DRTYPE_VOLUME,hr);return hr;
}
HRESULT WINAPI D3DXFilterTexture(IDirect3DBaseTexture8 *texture,const PALETTEENTRY *palette,UINT first,DWORD filter) {
    using F=HRESULT(WINAPI*)(void*,const PALETTEENTRY*,UINT,DWORD);
    auto fn=Utility<F>("D3DXFilterTexture");if(!fn||!texture)return E_INVALIDARG;
    const int kind=texture->GetType();
    HRESULT hr=fn(GenZH_Resource9Texture(GenZH_UnwrapBridgeResource(texture,kind,false),kind),palette,first,filter);
    Changed(texture,kind,hr);return hr;
}
HRESULT WINAPI D3DXCreateTextureFromFileExA(IDirect3DDevice8 *device,LPCSTR file,UINT w,UINT h,UINT levels,
    DWORD usage,D3DFORMAT format,D3DPOOL pool,DWORD filter,DWORD mipFilter,D3DCOLOR key,D3DXIMAGE_INFO *info,PALETTEENTRY *palette,IDirect3DTexture8 **out) {
    if(!device||!file||!out)return E_INVALIDARG;*out=nullptr;
    using GetInfo=HRESULT(WINAPI*)(LPCSTR,D3DXIMAGE_INFO*);
    using Load=HRESULT(WINAPI*)(void*,const PALETTEENTRY*,const RECT*,LPCSTR,const RECT*,DWORD,D3DCOLOR,D3DXIMAGE_INFO*);
    auto get=Utility<GetInfo>("D3DXGetImageInfoFromFileA");auto load=Utility<Load>("D3DXLoadSurfaceFromFileA");if(!get||!load)return E_FAIL;
    D3DXIMAGE_INFO image{};HRESULT hr=get(file,&image);if(FAILED(hr))return hr;
    if(!w||w==D3DX_DEFAULT)w=image.Width;if(!h||h==D3DX_DEFAULT)h=image.Height;
    if(format==D3DFMT_UNKNOWN)format=image.Format;
    hr=D3DXCreateTexture(device,w,h,levels,usage,format,pool,out);if(FAILED(hr))return hr;
    ComPtr<IDirect3DSurface8> surface;hr=(*out)->GetSurfaceLevel(0,surface.GetAddressOf());
    if(SUCCEEDED(hr))hr=load(Surface(surface.Get()),palette,nullptr,file,nullptr,filter,key,nullptr);
    if(SUCCEEDED(hr))hr=D3DXFilterTexture(*out,palette,0,mipFilter);
    if(FAILED(hr)){(*out)->Release();*out=nullptr;}else if(info)*info=image;
    return hr;
}
