// SPDX-License-Identifier: GPL-3.0-or-later
#include "CompatibilityRenderer.h"
#include <d3d11shader.h>
#include "LegacyShaderSource.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <climits>
#include <stdexcept>
using namespace GenZH::Modern;
using Microsoft::WRL::ComPtr;
static void Require(bool condition,const char *label){if(!condition)throw std::runtime_error(label);}
static void Replace(std::string &source,const char *marker,const std::string &replacement){source.replace(source.find(marker),std::strlen(marker),replacement);}
static void Compile(const std::string &vertex,const std::string &pixel,const std::string &out="",const std::string &dimension="2D"){
 std::string source=(vertex.find("input.reg")!=std::string::npos?std::string("#define LEGACY_PROGRAMMABLE_INPUT 1\n"):std::string())+LegacyShaderSource,textures;
 for(int stage=0;stage<8;++stage){std::string n=std::to_string(stage);textures+="Texture"+dimension+" Texture"+n+":register(t"+n+");SamplerState Sampler"+n+":register(s"+n+");float4 Sample"+n+"(float4 c){return Texture"+n+".Sample(Sampler"+n+",c."+(dimension=="2D"?"xy":"xyz")+");}\n";}
 textures+="float4 SampleStage(int i,float4 c){";for(int i=0;i<8;++i)textures+="if(i=="+std::to_string(i)+")return Sample"+std::to_string(i)+"(c);";textures+="return 1;}";
 Replace(source,"/*TEXTURES*/",textures);Replace(source,"/*VERTEX_PROGRAM*/",vertex);Replace(source,"/*PIXEL_PROGRAM*/",pixel);
 if(!out.empty()){std::filesystem::create_directories(out);std::ofstream(out+"/LegacyFixed.hlsl")<<source;}
 std::vector<const char*> entries={"LegacyVS","LegacyPS","LegacyPointGS","LegacyShadowVS","LegacyShadowPS"};
 if(!out.empty()){entries.push_back("TerrainHS");entries.push_back("TerrainDS");entries.push_back("TerrainGrassGS");}
 for(auto entry:entries){ComPtr<ID3DBlob> code,errors;
  std::string name=entry;
  const char *profile=name=="TerrainHS"?"hs_5_0":name=="TerrainDS"?"ds_5_0":name=="LegacyPointGS"||name=="TerrainGrassGS"?"gs_5_0":name=="LegacyVS"||name=="LegacyShadowVS"?"vs_5_0":"ps_5_0";
  HRESULT hr=D3DCompile(source.data(),source.size(),"BridgeFileChecks",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.GetAddressOf(),errors.GetAddressOf());
  if(FAILED(hr))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"shader compiler failed");
  ComPtr<ID3D11ShaderReflection> reflection;Require(SUCCEEDED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(reflection.GetAddressOf()))),"shader reflection");
  D3D11_SHADER_BUFFER_DESC constants{};auto *cb=reflection->GetConstantBufferByName("LegacyData");
  if(SUCCEEDED(cb->GetDesc(&constants))){Require(constants.Size==sizeof(LegacyConstants),"legacy constant buffer layout mismatch");
   const std::pair<const char*,UINT> fields[]={{"World",offsetof(LegacyConstants,world)},{"TextureMatrices",offsetof(LegacyConstants,textureMatrices)},{"PSConstants",offsetof(LegacyConstants,pixelConstants)},{"BridgeFlags",offsetof(LegacyConstants,bridgeFlags)},{"PointScale",offsetof(LegacyConstants,pointScale)}};
   for(const auto &field:fields){D3D11_SHADER_VARIABLE_DESC desc{};Require(SUCCEEDED(cb->GetVariableByName(field.first)->GetDesc(&desc))&&desc.StartOffset==field.second,"legacy field offset mismatch");}}
  if(!out.empty()){std::ofstream file(out+"/"+entry+".cso",std::ios::binary);file.write(static_cast<const char*>(code->GetBufferPointer()),static_cast<std::streamsize>(code->GetBufferSize()));}
 }
}
int main(int argc,char **argv){try{
 std::string error;std::vector<LegacyVertex> vertices;
 struct Packed{float position[4];DWORD diffuse;float uv[2];};Packed packed{{20,30,.5f,2},0x80402010,{.25f,.75f}};
 Require(SUCCEEDED(DecodeVertices(&packed,sizeof(packed),sizeof(packed),D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1,0,1,vertices,error)),"RHW decode");
 Require(vertices[0].position[3]==2&&vertices[0].diffuse[0]==64.0f/255&&vertices[0].uv[0][1]==.75f,"packed color and UV");
 Require(FAILED(DecodeVertices(&packed,sizeof(packed)-1,sizeof(packed),D3DFVF_XYZRHW,0,1,vertices,error)),"truncated vertex accepted");
 std::uint8_t bump[]{128,127,0,192};std::vector<float> bumpValues;
 Require(SUCCEEDED(DecodeBumpTexture(bump,sizeof(bump),4,2,1,D3DFMT_V8U8,bumpValues)),"signed bump decode");
 Require(bumpValues[0]==-1&&bumpValues[1]==1&&bumpValues[4]==0&&bumpValues[5]==-64.0f/127,"water bump vector signs lost");
 Require(FAILED(DecodeBumpTexture(bump,sizeof(bump)-1,4,2,1,D3DFMT_V8U8,bumpValues)),"truncated bump texture accepted");
 std::vector<DWORD> decl{D3DVSD_STREAM(0),D3DVSD_REG(0,D3DVSDT_FLOAT4),D3DVSD_REG(1,D3DVSDT_D3DCOLOR),D3DVSD_REG(2,D3DVSDT_FLOAT2),D3DVSD_END()};
 std::array<VertexStreamSpan,16> spans{};spans[0]={&packed,sizeof(packed),sizeof(packed)};
 Require(SUCCEEDED(DecodeDeclaration(decl,spans,0,1,vertices,error)),"programmable declaration decode");
 Require(vertices[0].registers[0][3]==2&&vertices[0].registers[1][0]==64.0f/255&&vertices[0].registers[2][1]==.75f,"VS inputs");
 std::uint16_t input[]{0,1,2};std::vector<std::uint32_t> indices;
 Require(SUCCEEDED(DecodeIndices(input,sizeof(input),D3DFMT_INDEX16,0,3,7,10,indices))&&indices[2]==9,"base vertex handling");
 Require(FAILED(DecodeIndices(input,sizeof(input),D3DFMT_INDEX16,0,3,7,9,indices)),"invalid index accepted");
 Require(PrimitiveVertexCount(D3DPT_TRIANGLELIST,UINT_MAX)==0,"primitive count overflow");
 const std::string fixedVS="LegacyOutput LegacyVS(LegacyInput input){return FixedVS(input);}";
 const std::string fixedPS="float4 LegacyPixel(float4 d,float4 s,float4 c[8]){return FixedPixel(d,s,c);}";
 Compile(fixedVS,fixedPS,argc>=3?argv[2]:"");Compile(fixedVS,fixedPS,"","Cube");Compile(fixedVS,fixedPS,"","3D");unsigned checked=3;
 if(argc>=2)for(const auto &entry:std::filesystem::directory_iterator(argv[1])){
  auto path=entry.path();if(path.stem().extension()==".pso"||path.stem().extension()==".vso")continue;
  if(path.extension()!=".pso"&&path.extension()!=".vso"&&path.extension()!=".asm")continue;
  std::ifstream file(path,std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
  std::string text;
  if(path.extension()==".asm")text.assign(bytes.begin(),bytes.end());
  else {ComPtr<ID3DBlob> assembly;Require(SUCCEEDED(D3DDisassemble(bytes.data(),bytes.size(),0,nullptr,assembly.GetAddressOf())),"legacy disassembly");text=static_cast<const char*>(assembly->GetBufferPointer());}
  std::string translated=path.extension()==".vso"?TranslateVertexShader(text,error):TranslatePixelShader(text,error);
  if(translated.empty())throw std::runtime_error(path.filename().string()+": "+error);
  Compile(path.extension()==".vso"?translated:fixedVS,path.extension()==".pso"?translated:fixedPS);++checked;
 }
 std::cout<<"PASS: bounds, packed vertices, declarations, rebased indices; "<<checked<<" shader variants (VS/PS/GS and shadow VS/PS). No device or window created.\n";return 0;
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
