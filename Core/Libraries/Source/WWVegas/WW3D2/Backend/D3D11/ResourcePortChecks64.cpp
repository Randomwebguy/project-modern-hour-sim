// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeviceBridge.h"
#include "NativeSceneIdentity.h"
#include <d3dx8.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static unsigned checks;
static void Check(bool ok,const char *text){++checks;if(!ok)throw std::runtime_error(text);}
static void HR(HRESULT hr,const char *text){if(FAILED(hr)){std::cerr<<text<<" HRESULT="<<std::hex<<hr<<"\n";Check(false,text);}}
int main(int argc,char **argv){try{
    Check(sizeof(void*)==8,"AMD64 process required");
    Check(argc==2,"output folder required");std::filesystem::path folder(argv[1]);std::filesystem::create_directories(folder);
    SetEnvironmentVariableA("GENZH_RENDERER","d3d11");SetEnvironmentVariableA("GENZH_USER_DATA_DIR",folder.string().c_str());
    // Never shown; the game and input systems are not involved.
    HWND window=CreateWindowExA(0,"STATIC","Resource migration file checks",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window!=nullptr,"hidden test target creation");
    ComPtr<IDirect3D8> d3d;d3d.Attach(Direct3DCreate8(D3D_SDK_VERSION));Check(bool(d3d),"AMD64 resource factory");
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_UNKNOWN;
    pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.Windowed=TRUE;
    ComPtr<IDirect3DDevice8> device;HR(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,device.GetAddressOf()),"AMD64 resource device");
    auto *connected=device.Detach();ConnectD3D11Device(&connected,window);device.Attach(connected);
    ComPtr<IDirect3DTexture8> texture;HR(device->CreateTexture(4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.GetAddressOf()),"texture");
    ComPtr<IDirect3DSurface8> surface;HR(texture->GetSurfaceLevel(0,surface.GetAddressOf()),"texture surface");
    const DWORD pixels[16]={0xff234567,0xff123456,0xff456789,0xffabcdef};RECT r{0,0,4,4};
    HR(D3DXLoadSurfaceFromMemory(surface.Get(),nullptr,nullptr,pixels,D3DFMT_A8R8G8B8,16,nullptr,&r,D3DX_FILTER_NONE,0),"AMD64 memory-to-surface copy");
    D3DLOCKED_RECT locked{};HR(surface->LockRect(&locked,nullptr,D3DLOCK_READONLY),"surface readback");
    for(unsigned y=0;y<4;++y)Check(std::memcmp(static_cast<unsigned char*>(locked.pBits)+y*locked.Pitch,pixels+y*4,16)==0,"surface bytes preserved");surface->UnlockRect();
    ComPtr<IDirect3DCubeTexture8> cube;HR(device->CreateCubeTexture(4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,cube.GetAddressOf()),"cube resource");
    ComPtr<IDirect3DSurface8> cubeFace;HR(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X,0,cubeFace.GetAddressOf()),"cube surface");
    HR(D3DXLoadSurfaceFromSurface(cubeFace.Get(),nullptr,nullptr,surface.Get(),nullptr,nullptr,D3DX_FILTER_NONE,0),"cube surface interop");
    HR(cubeFace->LockRect(&locked,nullptr,D3DLOCK_READONLY),"cube readback");Check(std::memcmp(locked.pBits,pixels,16)==0,"cube bytes preserved");cubeFace->UnlockRect();
    ComPtr<IDirect3DVolumeTexture8> volumeA,volumeB;for(auto *p:{std::addressof(volumeA),std::addressof(volumeB)})HR(device->CreateVolumeTexture(4,4,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,p->GetAddressOf()),"volume resource");
    D3DLOCKED_BOX box{};HR(volumeA->LockBox(0,&box,nullptr,0),"volume write");
    for(unsigned z=0;z<2;++z)for(unsigned y=0;y<4;++y)std::memcpy(static_cast<unsigned char*>(box.pBits)+z*box.SlicePitch+y*box.RowPitch,pixels+y*4,16);volumeA->UnlockBox(0);
    ComPtr<IDirect3DVolume8> va,vb;HR(volumeA->GetVolumeLevel(0,va.GetAddressOf()),"source volume");HR(volumeB->GetVolumeLevel(0,vb.GetAddressOf()),"dest volume");
    HR(D3DXLoadVolumeFromVolume(vb.Get(),nullptr,nullptr,va.Get(),nullptr,nullptr,D3DX_FILTER_NONE,0),"volume interop");
    HR(volumeB->LockBox(0,&box,nullptr,D3DLOCK_READONLY),"volume readback");Check(std::memcmp(box.pBits,pixels,16)==0,"volume bytes preserved");volumeB->UnlockBox(0);
    const char shader[]="vs_1_1\nmov oPos, v0\nmov oD0, v1\n";
    ComPtr<ID3DXBuffer> code,assemblyError;
    const HRESULT assemblyHR=D3DXAssembleShader(shader,sizeof(shader)-1,0,nullptr,code.GetAddressOf(),assemblyError.GetAddressOf());
    if(FAILED(assemblyHR)&&assemblyError)std::cerr<<static_cast<const char*>(assemblyError->GetBufferPointer());
    HR(assemblyHR,"original shader assembly");
    const DWORD decl[]={D3DVSD_STREAM(0),D3DVSD_REG(0,D3DVSDT_FLOAT4),D3DVSD_REG(1,D3DVSDT_D3DCOLOR),D3DVSD_END()};
    DWORD shaderID=0;HR(device->CreateVertexShader(decl,static_cast<const DWORD*>(code->GetBufferPointer()),&shaderID,0),"stable vertex shader ID");
    DWORD size=0;HR(device->GetVertexShaderDeclaration(shaderID,nullptr,&size),"declaration size");Check(size==sizeof(decl),"declaration length unchanged");
    std::vector<DWORD> declaration(size/4);HR(device->GetVertexShaderDeclaration(shaderID,declaration.data(),&size),"declaration bytes");Check(std::memcmp(declaration.data(),decl,size)==0,"declaration byte equality");
    HR(device->GetVertexShaderFunction(shaderID,nullptr,&size),"shader function size");Check(size==code->GetBufferSize(),"original shader size preserved");
    std::vector<DWORD> function(size/4);HR(device->GetVertexShaderFunction(shaderID,function.data(),&size),"shader function bytes");Check(std::memcmp(function.data(),code->GetBufferPointer(),size)==0,"original shader byte equality");
    HR(device->SetVertexShader(shaderID),"bind vertex shader");DWORD block=0;HR(device->CreateStateBlock(D3DSBT_ALL,&block),"stable state block ID");
    HR(device->SetVertexShader(D3DFVF_XYZRHW|D3DFVF_DIFFUSE),"bind FVF");HR(device->ApplyStateBlock(block),"restore shader state");DWORD restored=0;HR(device->GetVertexShader(&restored),"restored shader ID");Check(restored==shaderID,"state block shader ID restored");
    HR(device->DeleteStateBlock(block),"delete state block");HR(device->DeleteVertexShader(shaderID),"delete vertex shader");
    struct Vertex{float x,y,z,w;DWORD color;};std::vector<Vertex> vertices(70005);
    vertices[70002]={8,8,.5f,1,0xffff8040};vertices[70003]={56,8,.5f,1,0xffff8040};vertices[70004]={8,56,.5f,1,0xffff8040};
    ComPtr<IDirect3DVertexBuffer8> vertexBuffer;ComPtr<IDirect3DIndexBuffer8> indexBuffer;
    const DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE;
    HR(device->CreateVertexBuffer(static_cast<UINT>(vertices.size()*sizeof(Vertex)),0,fvf,D3DPOOL_MANAGED,vertexBuffer.GetAddressOf()),"wide vertex buffer");
    BYTE *data=nullptr;HR(vertexBuffer->Lock(0,0,&data,0),"wide vertex lock");std::memcpy(data,vertices.data(),vertices.size()*sizeof(Vertex));vertexBuffer->Unlock();
    HR(device->CreateIndexBuffer(12,0,D3DFMT_INDEX32,D3DPOOL_MANAGED,indexBuffer.GetAddressOf()),"native R32 index buffer");
    HR(indexBuffer->Lock(0,0,&data,0),"wide index lock");const DWORD indices[]={70002,70003,70004};std::memcpy(data,indices,12);indexBuffer->Unlock();
    D3DINDEXBUFFER_DESC desc{};HR(indexBuffer->GetDesc(&desc),"index buffer desc");Check(desc.Format==D3DFMT_INDEX32&&desc.Size==12,"R32 resource format");
    HR(device->SetVertexShader(fvf),"wide draw FVF");HR(device->SetStreamSource(0,vertexBuffer.Get(),sizeof(Vertex)),"wide vertex bind");HR(device->SetIndices(indexBuffer.Get(),0),"wide index bind");
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_LIGHTING,FALSE);
    D3DVIEWPORT8 viewport{0,0,64,64,0,1};HR(device->SetViewport(&viewport),"test viewport");
    device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    device->SetRenderState(D3DRS_FOGENABLE,FALSE);device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    HR(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"clear");HR(device->BeginScene(),"begin scene");
    HR(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,70002,3,0,1),"wide indexed DX11 draw");HR(device->EndScene(),"end scene");
    ComPtr<IDirect3DSurface8> image;HR(device->CreateImageSurface(64,64,D3DFMT_A8R8G8B8,image.GetAddressOf()),"pixel readback image");HR(device->GetFrontBuffer(image.Get()),"DX11 output readback");
    HR(image->LockRect(&locked,nullptr,D3DLOCK_READONLY),"DX11 pixel lock");unsigned colored=0;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){DWORD p;std::memcpy(&p,static_cast<unsigned char*>(locked.pBits)+y*locked.Pitch+x*4,4);if(p&0x00ffffff)++colored;}image->UnlockRect();
    std::cerr<<"Colored pixels: "<<colored<<"\n";
    Check(colored>500,"high-index triangle produces actual DX11 pixels");
    HR(device->Present(nullptr,nullptr,nullptr,nullptr),"DX11 present hidden target");
    HR(device->BeginScene(),"second fast hidden scene");HR(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,70002,3,0,1),"second R32 draw");HR(device->EndScene(),"second scene end");HR(device->Present(nullptr,nullptr,nullptr,nullptr),"second hidden present");
    // Exercise the actual production paged queue and sorted concatenation on
    // the GPU. Two overlapping differently colored half-alpha triangles must
    // produce exactly the same pixels as the unmerged reference, in that order.
    HR(device->SetRenderState(D3DRS_STENCILENABLE,TRUE),"ordered world stencil");
    device->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_ALWAYS);device->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);device->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    const DWORD localIndices[]={0,1,2};std::vector<DWORD> reference;
    for(unsigned mode=0;mode<2;++mode){
      HR(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"ordered blend clear");HR(device->BeginScene(),"ordered blend begin");
      GenZH::Modern::NativeMeshSource source;source.root=1;source.mesh=2;source.resource=3;source.sorted=mode!=0;
      GenZH::Modern::ScopedNativeMeshSource scoped(source);
      for(DWORD color:{0x80ff4000u,0x800020ffu}){
       const Vertex local[]={{8,8,.5f,1,color},{56,8,.5f,1,color},{8,56,.5f,1,color}};
       HR(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,3,1,localIndices,D3DFMT_INDEX32,local,sizeof(Vertex)),"ordered half-alpha triangle");
      }
      HR(device->EndScene(),"ordered blend end");HR(device->GetFrontBuffer(image.Get()),"ordered blend readback");
      HR(image->LockRect(&locked,nullptr,D3DLOCK_READONLY),"ordered blend pixels");std::vector<DWORD> pixelsNow(64*64);
      for(unsigned y=0;y<64;++y)std::memcpy(pixelsNow.data()+y*64,static_cast<unsigned char*>(locked.pBits)+y*locked.Pitch,64*4);
      image->UnlockRect();if(!mode){reference=std::move(pixelsNow);Check((reference[16*64+16]&0x00ffffff)!=0,"blend fixture has actual nonblack coverage");}
      else Check(reference==pixelsNow,"merged transparent primitive order is pixel exact on actual GPU");
      HR(device->Present(nullptr,nullptr,nullptr,nullptr),"ordered blend present");
    }
    std::ofstream(folder/"resource-port-result.json")<<"{\"passed\":true,\"pointer_bits\":64,\"indices\":[70002,70003,70004],\"colored_pixels\":"<<colored
        <<",\"texture_cube_volume_copies\":true,\"shader_declaration_and_function_byte_exact\":true,\"state_block_ids\":true,\"sorted_batch_pixel_exact\":true,\"checks\":"<<checks<<",\"game_started\":false,\"visible_window\":false}\n";
    // Resources own the hidden target until teardown; never destroy it early.
    std::cout<<"PASS AMD64 resource + DX11 connection; "<<checks<<" checks\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<"\n";return 1;}}

