// SPDX-License-Identifier: GPL-3.0-or-later
// taa101: GPU check of the TAA jitter in translated game vertex programs. A vs_1_1
// program (dp4 oPos from c0-c3, as trees.vso/wave.vso build their position) draws a
// triangle whose edge lies at clip x = 0 with w = 1 and w = 3 (perspective). With the
// zero jitter buffer bound at b8 the edge covers exactly half the 64-pixel target; a
// +0.75 pixel jitter moves it by exactly that many pixels (one more column whose
// centre lies within), independent of w; the program reads b8 (reflection).
#include "LegacyShaderSource.h"
#include "ProgramSource.h"
#include "ShaderBytecodeCache.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace GenZH::Modern;using Microsoft::WRL::ComPtr;
static unsigned checks=0;
static void Check(bool ok,const std::string &what){++checks;if(!ok)throw std::runtime_error(what);}
static void HR(HRESULT hr,const std::string &what){Check(SUCCEEDED(hr),what);}
int main(){try{
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0,got{};
 HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&level,1,D3D11_SDK_VERSION,device.GetAddressOf(),&got,context.GetAddressOf()),"hardware device");
 std::array<D3DRESOURCETYPE,8> types{};std::string error;
 const std::string assembly="vs_1_1\ndp4 oPos.x, v0, c0\ndp4 oPos.y, v0, c1\ndp4 oPos.z, v0, c2\ndp4 oPos.w, v0, c3\nmov oD0, c4\n";
 std::string source=BuildProgramSource(LegacyShaderSource,true,false,assembly,{},types,error);Check(!source.empty(),"translated program: "+error);
 Check(source.find("output.position.xy+=TemporalVertexJitter.xy*oPos.w")!=std::string::npos,"translation applies the vertex jitter");
 source+="\nfloat4 PSJitterCoverage(LegacyOutput input):SV_TARGET{return 1;}\n";
 ComPtr<ID3DBlob> vsCode,psCode;bool hit=false;HR(CompileCachedShader(source,"LegacyVS","vs_5_0",vsCode,error,hit),"LegacyVS: "+error);HR(CompileCachedShader(source,"PSJitterCoverage","ps_5_0",psCode,error,hit),"PS: "+error);
 {ComPtr<ID3D11ShaderReflection> reflection;HR(D3DReflect(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),IID_PPV_ARGS(reflection.GetAddressOf())),"reflection");
  D3D11_SHADER_INPUT_BIND_DESC bind{};HR(reflection->GetResourceBindingDescByName("TemporalVertexData",&bind),"vertex program binds TemporalVertexData");Check(bind.BindPoint==8,"TemporalVertexData at b8");}
 ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;HR(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,vs.GetAddressOf()),"VS");HR(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,ps.GetAddressOf()),"PS");
 // The programmable input carries the fixed elements too (POSITION..TEXCOORD7), then v0..v15.
 std::vector<D3D11_INPUT_ELEMENT_DESC> elements{{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
  {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0}};
 for(UINT i=0;i<8;++i)elements.push_back({"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64+i*16,D3D11_INPUT_PER_VERTEX_DATA,0});
 for(UINT i=0;i<16;++i)elements.push_back({"BLENDWEIGHT",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,192+i*16,D3D11_INPUT_PER_VERTEX_DATA,0});
 ComPtr<ID3D11InputLayout> layout;HR(device->CreateInputLayout(elements.data(),static_cast<UINT>(elements.size()),vsCode->GetBufferPointer(),vsCode->GetBufferSize(),layout.GetAddressOf()),"input layout");
 D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=64;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;HR(device->CreateTexture2D(&td,nullptr,target.GetAddressOf()),"target");HR(device->CreateRenderTargetView(target.Get(),nullptr,rtv.GetAddressOf()),"rtv");
 td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&td,nullptr,staging.GetAddressOf()),"staging");
 auto buffer=[&](UINT bytes,const void *data){D3D11_BUFFER_DESC bd{};bd.ByteWidth=bytes;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA init{data,0,0};ComPtr<ID3D11Buffer> b;HR(device->CreateBuffer(&bd,&init,b.GetAddressOf()),"constant buffer");return b;};
 float vsConstants[96][4]{};vsConstants[0][0]=vsConstants[1][1]=vsConstants[2][2]=vsConstants[3][3]=1;vsConstants[4][0]=vsConstants[4][1]=vsConstants[4][2]=vsConstants[4][3]=1;
 const auto programConstants=buffer(sizeof(vsConstants),vsConstants);std::vector<unsigned char> legacy(4096,0);const auto legacyConstants=buffer(4096,legacy.data());
 auto columns=[&](float jitterPixels,float w){
  const float jitter[4]{2*jitterPixels/64,0,0,0};const auto jitterBuffer=buffer(16,jitter);
  // Two triangles cover the left half: clip x in [-3w,0], y in [-3w,3w].
  struct V{float reg[28][4];};std::vector<V> tris;const float quad[6][2]{{-3,-3},{0,-3},{0,3},{-3,-3},{0,3},{-3,3}};
  for(const auto &q:quad){V p{};p.reg[12][0]=q[0]*w;p.reg[12][1]=q[1]*w;p.reg[12][2]=.5f*w;p.reg[12][3]=w;tris.push_back(p);}
  D3D11_BUFFER_DESC bd{};bd.ByteWidth=static_cast<UINT>(tris.size()*sizeof(V));bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init{tris.data(),0,0};ComPtr<ID3D11Buffer> vb;HR(device->CreateBuffer(&bd,&init,vb.GetAddressOf()),"vertices");
  const float clear[4]{};context->ClearRenderTargetView(rtv.Get(),clear);ID3D11RenderTargetView *rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
  D3D11_VIEWPORT viewport{0,0,64,64,0,1};context->RSSetViewports(1,&viewport);
  D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;HR(device->CreateRasterizerState(&rd,raster.GetAddressOf()),"raster");context->RSSetState(raster.Get());
  UINT stride=sizeof(V),offset=0;ID3D11Buffer *raw=vb.Get();context->IASetVertexBuffers(0,1,&raw,&stride,&offset);context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11Buffer *b0=legacyConstants.Get(),*b1=programConstants.Get(),*b8=jitterBuffer.Get();context->VSSetConstantBuffers(0,1,&b0);context->VSSetConstantBuffers(1,1,&b1);context->VSSetConstantBuffers(8,1,&b8);
  context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->Draw(6,0);
  context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"readback");
  unsigned covered=0;const auto *row=static_cast<const unsigned char*>(map.pData)+32*map.RowPitch;for(unsigned x=0;x<64;++x)covered+=row[x*4]>128?1u:0u;context->Unmap(staging.Get(),0);return covered;};
 const unsigned plain1=columns(0,1),plain3=columns(0,3),moved1=columns(.75f,1),moved3=columns(.75f,3),back1=columns(-.75f,1);
 std::cerr<<"columns zero "<<plain1<<"/"<<plain3<<" plus "<<moved1<<"/"<<moved3<<" minus "<<back1<<"\n";
 Check(plain1==32&&plain3==32,"zero jitter: the edge covers exactly half the target");
 Check(moved1==33&&moved3==33,"+0.75 px jitter moves the edge one column, independent of w");
 Check(back1==31,"-0.75 px jitter moves it the other way");
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"columns_zero\":"<<plain1<<",\"columns_plus\":"<<moved1<<",\"columns_minus\":"<<back1<<"}"<<std::endl;return 0;
}catch(const std::exception &e){std::cout<<"{\"passed\":false,\"checks\":"<<checks<<",\"error\":\""<<e.what()<<"\"}"<<std::endl;return 1;}}
