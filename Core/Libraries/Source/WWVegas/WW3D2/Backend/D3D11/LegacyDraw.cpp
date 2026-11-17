// SPDX-License-Identifier: GPL-3.0-or-later
#include "LegacyDraw.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace GenZH::Modern {
namespace {
float ReadFloat(const std::uint8_t *data) { float value;std::memcpy(&value,data,4);return value; }
// std::isfinite for binary32 (exponent not all ones) without the out-of-line CRT classification call.
bool FiniteBits(float value) { std::uint32_t bits;std::memcpy(&bits,&value,4);return (bits&0x7f800000u)!=0x7f800000u; }
void Color(const std::uint8_t *data,float *rgba) {
    std::uint32_t value;std::memcpy(&value,data,4);
    rgba[0]=static_cast<float>((value>>16)&255)/255;
    rgba[1]=static_cast<float>((value>>8)&255)/255;
    rgba[2]=static_cast<float>(value&255)/255;
    rgba[3]=static_cast<float>((value>>24)&255)/255;
}
std::string Trim(std::string value) {
    auto first=value.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};
    return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
std::string Operand(std::string input,bool destination=false) {
    input=Trim(input);if(!destination&&input.rfind("1-",0)==0)return "(1-"+Operand(input.substr(2))+ ")";
    bool negative=!input.empty()&&input[0]=='-';
    if(negative)input.erase(0,1);
    auto dot=input.find('.');std::string suffix=dot==std::string::npos?"":input.substr(dot);
    std::string root=dot==std::string::npos?input:input.substr(0,dot);
    auto modifier=root.find('_');std::string mode=modifier==std::string::npos?"":root.substr(modifier);
    root=root.substr(0,modifier);
    if(!std::regex_match(root,std::regex("[rtvc][0-9]+"))) throw std::runtime_error("unsupported operand: "+input);
    if(root[0]=='c')root="PSConstants["+root.substr(1)+"]";
    std::string expression=root;
    if(!destination) {
        if(mode=="_bias")expression="("+expression+"-0.5)";
        else if(mode=="_bx2")expression="("+expression+"*2-1)";
        else if(mode=="_x2")expression="("+expression+"*2)";
        else if(mode=="_d2")expression="("+expression+"*0.5)";
        else if(mode=="_abs")expression="abs("+expression+")";
        else if(mode=="_comp")expression="(1-"+expression+")";
        else if(!mode.empty())throw std::runtime_error("unsupported source modifier: "+mode);
        if(negative)expression="(-"+expression+")";
        expression+=suffix;
        // Arithmetic PS1 operands expand short swizzles to a float4.
        if(!suffix.empty()) {
            std::string swizzle=suffix.substr(1);
            if(swizzle.size()<4)expression+=std::string(4-swizzle.size(),swizzle.back());
        }
    } else expression+=suffix;
    return expression;
}
}

UINT PrimitiveVertexCount(D3DPRIMITIVETYPE type,UINT primitives) {
    std::uint64_t count=0;
    if(!primitives)return 0;
    switch(type) {
    case D3DPT_POINTLIST:count=primitives;break;
    case D3DPT_LINELIST:count=std::uint64_t(primitives)*2;break;
    case D3DPT_LINESTRIP:count=std::uint64_t(primitives)+1;break;
    case D3DPT_TRIANGLELIST:count=std::uint64_t(primitives)*3;break;
    case D3DPT_TRIANGLESTRIP:case D3DPT_TRIANGLEFAN:count=std::uint64_t(primitives)+2;break;
    default:return 0;
    }
    return count>std::numeric_limits<UINT>::max()?0:static_cast<UINT>(count);
}

template<class Vertex>static HRESULT DecodeVerticesImpl(const void *data,std::size_t bytes,UINT stride,DWORD fvf,UINT first,UINT count,
                       std::vector<Vertex> &result,std::string &error,const std::vector<std::uint32_t> *referenced=nullptr) {
    result.clear();error.clear();
    if(!data || !stride || !count || std::uint64_t(first+std::uint64_t(count))*stride>bytes) {
        error="vertex range exceeds captured buffer";return E_INVALIDARG;
    }
    UINT position=fvf&D3DFVF_POSITION_MASK;
    if(position!=D3DFVF_XYZ && position!=D3DFVF_XYZRHW) {
        error="blended/programmatic vertex layout needs a declaration adapter";return E_NOTIMPL;
    }
    UINT positionBytes=position==D3DFVF_XYZRHW?16:12;
    UINT normalOffset=positionBytes,diffuseOffset=normalOffset+((fvf&D3DFVF_NORMAL)?12:0);
    UINT specularOffset=diffuseOffset+((fvf&D3DFVF_DIFFUSE)?4:0);
    UINT uvOffset=specularOffset+((fvf&D3DFVF_SPECULAR)?4:0);
    UINT uvCount=(fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
    if(uvCount>std::size(Vertex{}.uv))return E_INVALIDARG;
    std::array<UINT,8> uvOffsets{},uvSizes{};
    for(UINT i=0;i<uvCount;++i) {
        UINT code=(fvf>>(16+2*i))&3;
        uvOffsets[i]=uvOffset;uvSizes[i]=code==0?2:code==1?3:code==2?4:1;
        uvOffset+=uvSizes[i]*4;
    }
    if(uvOffset>stride){error="FVF fields exceed vertex stride";return E_INVALIDARG;}
    const auto outputCount=referenced?referenced->size():count;
    result.resize(outputCount);
    auto source=static_cast<const std::uint8_t*>(data);
    for(std::size_t i=0;i<outputCount;++i) {
        const auto sourceIndex=referenced?(*referenced)[i]:first+i;
        const auto *p=source+std::size_t(sourceIndex)*stride;auto &vertex=result[i];
        for(UINT j=0;j<positionBytes/4;++j)vertex.position[j]=ReadFloat(p+j*4);
        if(fvf&D3DFVF_NORMAL)for(UINT j=0;j<3;++j)vertex.normal[j]=ReadFloat(p+normalOffset+j*4);
        if(fvf&D3DFVF_DIFFUSE)Color(p+diffuseOffset,vertex.diffuse);
        if(fvf&D3DFVF_SPECULAR)Color(p+specularOffset,vertex.specular);
        for(UINT channel=0;channel<uvCount;++channel)
            for(UINT j=0;j<uvSizes[channel];++j)vertex.uv[channel][j]=ReadFloat(p+uvOffsets[channel]+j*4);
        for(float value:vertex.position)if(!FiniteBits(value)){error="nonfinite position";return E_INVALIDARG;}
    }
    return S_OK;
}

HRESULT DecodeIndices(const void *data,std::size_t bytes,D3DFORMAT format,UINT first,UINT count,
                      UINT base,UINT vertexCount,std::vector<std::uint32_t> &result) {
    result.clear();UINT stride=format==D3DFMT_INDEX16?2:format==D3DFMT_INDEX32?4:0;
    if(!data || !stride || std::uint64_t(first+std::uint64_t(count))*stride>bytes)return E_INVALIDARG;
    const auto *source=static_cast<const std::uint8_t*>(data)+std::size_t(first)*stride;
    result.reserve(count);
    for(UINT i=0;i<count;++i) {
        std::uint32_t value=0;std::memcpy(&value,source+std::size_t(i)*stride,stride);
        std::uint64_t actual=std::uint64_t(value)+base;
        if(actual>=vertexCount)return E_INVALIDARG;
        result.push_back(static_cast<std::uint32_t>(actual));
    }
    return S_OK;
}

std::string TranslatePixelShader(const std::string &assembly,std::string &error) {
    error.clear();std::ostringstream output;
    output<<"float4 LegacyPixel(float4 v0,float4 v1,float4 coords[8]) {\n";
    for(int i=0;i<6;++i)output<<"float4 r"<<i<<"=0;\n";
    for(int i=0;i<8;++i)output<<"float4 t"<<i<<"=coords["<<i<<"];\n";
    output<<"float2 matrixPad=0;\n";
    try {
        std::istringstream lines(assembly);std::string line;
        while(std::getline(lines,line)) {
            auto comment=line.find("//");line=Trim(line.substr(0,std::min(comment,line.find(';'))));
            if(line.empty() || line[0]==';' || line.rfind("ps",0)==0)continue;
            if(line[0]=='+')line=Trim(line.substr(1));
            auto space=line.find_first_of(" \t");std::string op=line.substr(0,space);
            if(op=="nop" || op=="phase")continue;
            std::vector<std::string> operands;
            std::istringstream arguments(space==std::string::npos?"":line.substr(space+1));std::string arg;
            while(std::getline(arguments,arg,','))operands.push_back(Trim(arg));
            if(operands.empty())throw std::runtime_error("missing operands: "+op);
            auto get=[&](std::size_t i){if(i>=operands.size())throw std::runtime_error("missing operand: "+op);return Operand(operands[i]);};
            std::string dest=Operand(operands[0],true),expression;
            if(op=="texld" || op=="texcrd" || op=="tex" || op=="texcoord" || op=="texkill" || op=="texbem" || op=="texbeml" || op=="texreg2ar" || op=="texreg2gb") {
                int stage=std::stoi(operands[0].substr(1));if(stage<0 || stage>=8)throw std::runtime_error("texture stage out of bounds");
                if(op=="texkill"){output<<"clip("<<dest<<".xyz);\n";continue;}
                if(op=="texld")expression="Sample"+std::to_string(stage)+"("+get(1)+")";
                else if(op=="texcrd")expression=get(1);
                else if(op=="texcoord")expression="coords["+std::to_string(stage)+"]";
                else {
                    std::string coord="coords["+std::to_string(stage)+"]";
                    if(op=="texreg2ar")coord="float4(("+get(1)+").a,("+get(1)+").r,0,1)";
                    if(op=="texreg2gb")coord="float4(("+get(1)+").gb,0,1)";
                    if(op=="texbem" || op=="texbeml")coord="float4("+coord+".xy+mul(float2x2(Bump["+std::to_string(stage)+"]), ("+get(1)+").xy),0,1)";
                    expression="Sample"+std::to_string(stage)+"("+coord+")";
                    if(op=="texbeml")expression+="*(("+get(1)+").z*BumpLuma["+std::to_string(stage)+"].x+BumpLuma["+std::to_string(stage)+"].y)";
                }
            } else if(op=="def") {
                throw std::runtime_error("inline def constants require shader-local constant binding");
            } else {
                bool saturate=false;float scale=1;
                auto suffix=op.find('_');std::string modifiers=suffix==std::string::npos?"":op.substr(suffix);
                op=op.substr(0,suffix);
                if(modifiers.find("_sat")!=std::string::npos)saturate=true;
                if(modifiers.find("_x2")!=std::string::npos)scale=2;
                if(modifiers.find("_x4")!=std::string::npos)scale=4;
                if(modifiers.find("_x8")!=std::string::npos)scale=8;
                if(modifiers.find("_d2")!=std::string::npos)scale=.5f;
                if(modifiers.find("_d4")!=std::string::npos)scale=.25f;
                if(modifiers.find("_d8")!=std::string::npos)scale=.125f;
                if(op=="mov")expression=get(1);
                else if(op=="add")expression=get(1)+"+"+get(2);
                else if(op=="sub")expression=get(1)+"-"+get(2);
                else if(op=="mul")expression=get(1)+"*"+get(2);
                else if(op=="mad")expression=get(1)+"*"+get(2)+"+"+get(3);
                else if(op=="lrp")expression="lerp("+get(3)+","+get(2)+","+get(1)+")";
                else if(op=="dp3")expression="dot(("+get(1)+").xyz,("+get(2)+").xyz).xxxx";
                else if(op=="dp4")expression="dot("+get(1)+","+get(2)+").xxxx";
                else if(op=="min" || op=="max")expression=op+"("+get(1)+","+get(2)+")";
                else if(op=="cmp")expression="float4(("+get(1)+").x>=0?("+get(2)+").x:("+get(3)+").x,("+get(1)+").y>=0?("+get(2)+").y:("+get(3)+").y,("+get(1)+").z>=0?("+get(2)+").z:("+get(3)+").z,("+get(1)+").w>=0?("+get(2)+").w:("+get(3)+").w)";
                else throw std::runtime_error("unsupported pixel instruction: "+op);
                expression="("+expression+")*"+std::to_string(scale);
                if(saturate)expression="saturate("+expression+")";
            }
            auto mask=dest.find('.');
            if(mask!=std::string::npos)expression="("+expression+")"+dest.substr(mask);
            output<<dest<<"="<<expression<<";\n";
        }
        output<<"return r0;\n}\n";return output.str();
    }catch(const std::exception &exception){error=exception.what();return {};}
}
HRESULT DecodeBumpTexture(const void *data,std::size_t bytes,UINT pitch,UINT width,UINT height,D3DFORMAT format,std::vector<float> &rgba){
 UINT size=format==D3DFMT_V8U8||format==D3DFMT_L6V5U5?2:format==D3DFMT_X8L8V8U8||format==D3DFMT_Q8W8V8U8||format==D3DFMT_V16U16?4:0;
 if(!size||!data||!width||!height||std::uint64_t(width)*size>pitch||std::uint64_t(pitch)*height>bytes)return E_INVALIDARG;
 rgba.assign(std::size_t(width)*height*4,0);
 auto snorm=[](UINT bits,UINT count){int value=static_cast<int>(bits);if(bits&(1u<<(count-1)))value-=1<<count;return std::max(-1.0f,value/static_cast<float>((1u<<(count-1))-1));};
 for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){std::uint32_t packed=0;std::memcpy(&packed,static_cast<const std::uint8_t*>(data)+std::size_t(y)*pitch+x*size,size);
  auto *pixel=rgba.data()+(std::size_t(y)*width+x)*4;pixel[3]=1;
  if(format==D3DFMT_L6V5U5){pixel[0]=snorm(packed&31,5);pixel[1]=snorm((packed>>5)&31,5);pixel[2]=((packed>>10)&63)/63.0f;}
  else if(format==D3DFMT_V16U16){pixel[0]=snorm(packed&65535,16);pixel[1]=snorm(packed>>16,16);}
  else{pixel[0]=snorm(packed&255,8);pixel[1]=snorm((packed>>8)&255,8);
   if(format==D3DFMT_X8L8V8U8)pixel[2]=((packed>>16)&255)/255.0f;
   if(format==D3DFMT_Q8W8V8U8){pixel[2]=snorm((packed>>16)&255,8);pixel[3]=snorm(packed>>24,8);}}
 }return S_OK;
}

static HRESULT DecodeDeclarationImpl(const std::vector<DWORD> &tokens,const std::array<VertexStreamSpan,16> &streams,
 UINT first,UINT count,std::vector<LegacyVertex> &vertices,std::string &error,const std::vector<std::uint32_t> *referenced=nullptr){
 const auto outputCount=referenced?referenced->size():count;vertices.assign(outputCount,LegacyVertex{});UINT stream=0;std::array<UINT,16> offsets{};
 bool ended=false;
 for(std::size_t i=0;i<tokens.size();++i){DWORD token=tokens[i];if(token==D3DVSD_END()){ended=true;break;}
  UINT type=(token>>29)&7;
  if(type==D3DVSD_TOKEN_NOP)continue;
  if(type==D3DVSD_TOKEN_STREAM){stream=token&15;if(token&D3DVSD_STREAMTESSMASK)return E_NOTIMPL;continue;}
  if(type==D3DVSD_TOKEN_CONSTMEM){i+=((token>>25)&15)*4;if(i>=tokens.size())return E_INVALIDARG;continue;}
  if(type!=D3DVSD_TOKEN_STREAMDATA){error="unsupported vertex declaration token";return E_NOTIMPL;}
  if(token&D3DVSD_DATALOADTYPEMASK){offsets[stream]+=((token>>16)&15)*4;continue;}
  UINT reg=token&31,format=(token>>16)&15;
  UINT size=format<=3?(format+1)*4:format==4||format==5||format==6?4:format==7?8:0;
  if(reg>=16||!size)return E_INVALIDARG;
  const auto &span=streams[stream];UINT offset=offsets[stream];
  if(!span.data||offset+size>span.stride||std::uint64_t(first+std::uint64_t(count))*span.stride>span.bytes){error="declared vertex stream is out of bounds";return E_INVALIDARG;}
  for(std::size_t v=0;v<outputCount;++v){const auto sourceIndex=referenced?(*referenced)[v]:first+v;auto *source=static_cast<const std::uint8_t*>(span.data)+std::size_t(sourceIndex)*span.stride+offset;
   float *dest=vertices[v].registers[reg];dest[3]=1;
   if(format<=3)for(UINT component=0;component<=format;++component)dest[component]=ReadFloat(source+4*component);
   else if(format==D3DVSDT_D3DCOLOR)Color(source,dest);
   else if(format==D3DVSDT_UBYTE4)for(UINT component=0;component<4;++component)dest[component]=source[component];
   else for(UINT component=0;component<(format==D3DVSDT_SHORT2?2u:4u);++component){short value;std::memcpy(&value,source+component*2,2);dest[component]=value;}
  }
  offsets[stream]+=size;
 }
 if(!ended){error="unterminated vertex declaration";return E_INVALIDARG;}return S_OK;
}

namespace {
// perf89: when the declared vertex range is small relative to the index count,
// the ascending unique list and each index's position in it come from a
// range-sized table (one pass each) instead of sort/unique and binary search.
// Both give the same unique list and the same remapped indices.
struct ReferencedTable {std::vector<std::uint32_t> slot;UINT first=0;bool used=false;};
bool ReferencedPlan(UINT first,UINT count,const std::vector<std::uint32_t> &indices,std::vector<std::uint32_t> &unique,std::string &error,ReferencedTable *table=nullptr){
 if(table)table->used=false;
 if(!count||indices.empty()||std::uint64_t(first)+count>std::uint64_t(UINT_MAX)+1){error="invalid indexed vertex range";return false;}
 for(auto index:indices)if(index<first||std::uint64_t(index)-first>=count){error="index outside declared vertex range";return false;}
 if(table&&std::uint64_t(count)<=std::uint64_t(indices.size())*2+256){
  table->used=true;table->first=first;table->slot.assign(count,UINT32_MAX);
  for(auto index:indices)table->slot[index-first]=0;
  unique.clear();
  for(UINT v=0;v<count;++v)if(table->slot[v]!=UINT32_MAX){table->slot[v]=static_cast<std::uint32_t>(unique.size());unique.push_back(first+v);}
  return true;
 }
 unique=indices;std::sort(unique.begin(),unique.end());unique.erase(std::unique(unique.begin(),unique.end()),unique.end());return true;
}
void RemapReferenced(std::vector<std::uint32_t> &indices,const std::vector<std::uint32_t> &unique,const ReferencedTable *table=nullptr){
 if(table&&table->used){for(auto &index:indices)index=table->slot[index-table->first];return;}
 for(auto &index:indices)index=static_cast<std::uint32_t>(std::lower_bound(unique.begin(),unique.end(),index)-unique.begin());
}
}
HRESULT DecodeVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,UINT first,UINT count,std::vector<LegacyVertex> &vertices,std::string &error){
 return DecodeVerticesImpl(data,bytes,stride,fvf,first,count,vertices,error);
}
HRESULT DecodeDeclaration(const std::vector<DWORD> &tokens,const std::array<VertexStreamSpan,16> &streams,UINT first,UINT count,std::vector<LegacyVertex> &vertices,std::string &error){
 return DecodeDeclarationImpl(tokens,streams,first,count,vertices,error);
}
HRESULT DecodeReferencedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,UINT first,UINT count,std::vector<std::uint32_t> &indices,std::vector<LegacyVertex> &vertices,std::string &error){
 thread_local ReferencedTable table;
 std::vector<std::uint32_t> unique;if(!ReferencedPlan(first,count,indices,unique,error,&table))return E_INVALIDARG;
 const auto hr=DecodeVerticesImpl(data,bytes,stride,fvf,first,count,vertices,error,&unique);if(SUCCEEDED(hr))RemapReferenced(indices,unique,&table);return hr;
}
HRESULT DecodeTwoUVFixedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,
 UINT count,std::vector<TwoUVFixedVertex> &result,std::string &error){
 if(!TwoUVFixedLayout(fvf,stride)){result.clear();error="unsupported two-UV sorted layout";return E_INVALIDARG;}
 // perf87: one write per vertex instead of value-initializing the array and
 // decoding over it. Same fields, offsets (XYZ 0, normal 12, diffuse 24, UV0 28,
 // UV1 36 of 44) and checks as DecodeVerticesImpl; unwritten lanes keep the
 // TwoUVFixedVertex defaults. On rejection the array matches the generic path.
 result.clear();error.clear();
 if(!data||!count||std::uint64_t(count)*stride>bytes){error="vertex range exceeds captured buffer";return E_INVALIDARG;}
 result.reserve(count);
 const auto *source=static_cast<const std::uint8_t*>(data);
 for(UINT i=0;i<count;++i){
  const auto *p=source+std::size_t(i)*44;TwoUVFixedVertex vertex;
  for(UINT j=0;j<3;++j){vertex.position[j]=ReadFloat(p+j*4);vertex.normal[j]=ReadFloat(p+12+j*4);}
  Color(p+24,vertex.diffuse);
  for(UINT j=0;j<2;++j){vertex.uv[0][j]=ReadFloat(p+28+j*4);vertex.uv[1][j]=ReadFloat(p+36+j*4);}
  result.push_back(vertex);
  for(float value:vertex.position)if(!FiniteBits(value)){result.resize(count);error="nonfinite position";return E_INVALIDARG;}
 }
 return S_OK;
}
HRESULT DecodeFixedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,UINT first,UINT count,std::vector<FixedVertex> &vertices,std::string &error){
 return DecodeVerticesImpl(data,bytes,stride,fvf,first,count,vertices,error);
}
HRESULT DecodeReferencedFixedVertices(const void *data,std::size_t bytes,UINT stride,DWORD fvf,UINT first,UINT count,std::vector<std::uint32_t> &indices,std::vector<FixedVertex> &vertices,std::string &error){
 thread_local ReferencedTable table;
 std::vector<std::uint32_t> unique;if(!ReferencedPlan(first,count,indices,unique,error,&table))return E_INVALIDARG;
 const auto hr=DecodeVerticesImpl(data,bytes,stride,fvf,first,count,vertices,error,&unique);if(SUCCEEDED(hr))RemapReferenced(indices,unique,&table);return hr;
}
HRESULT DecodeReferencedDeclaration(const std::vector<DWORD> &tokens,const std::array<VertexStreamSpan,16> &streams,UINT first,UINT count,std::vector<std::uint32_t> &indices,std::vector<LegacyVertex> &vertices,std::string &error){
 thread_local ReferencedTable table;
 std::vector<std::uint32_t> unique;if(!ReferencedPlan(first,count,indices,unique,error,&table))return E_INVALIDARG;
 const auto hr=DecodeDeclarationImpl(tokens,streams,first,count,vertices,error,&unique);if(SUCCEEDED(hr))RemapReferenced(indices,unique,&table);return hr;
}

std::string TranslateVertexShader(const std::string &assembly,std::string &error){
 error.clear();std::ostringstream out;
 out<<"LegacyOutput LegacyVS(LegacyInput input){LegacyOutput output=(LegacyOutput)0;\n";
 for(int i=0;i<16;++i)out<<"float4 v"<<i<<"=input.reg"<<i<<";\n";
 for(int i=0;i<12;++i)out<<"float4 r"<<i<<"=0;\n";
 out<<"float4 a0=0,oPos=0,oD0=1,oD1=0,oFog=0,oPts=1;\n";
 for(int i=0;i<8;++i)out<<"float4 oT"<<i<<"=0;\n";
 auto operand=[](std::string input,bool dest=false){
  input=Trim(input);bool minus=!input.empty()&&input[0]=='-';if(minus)input.erase(0,1);
  // Relative constants in VS1 use the address register's x component.
  input=std::regex_replace(input,std::regex(R"(c\[a0\.x\s*\+\s*([0-9]+)\])"),"VSConstants[clamp((int)a0.x+$1,0,95)]");
  input=std::regex_replace(input,std::regex(R"(c\[a0\.x\])"),"VSConstants[clamp((int)a0.x,0,95)]");
  input=std::regex_replace(input,std::regex(R"(\bc([0-9]+))"),"VSConstants[$1]");
  auto dot=input.rfind('.');if(dot!=std::string::npos){std::string mask=input.substr(dot+1);
   if(mask.find_first_not_of("xyzwrgba")==std::string::npos&&!dest&&mask.size()<4)input+=std::string(4-mask.size(),mask.back());}
  return minus?"(-"+input+")":input;
 };
 try{
  std::istringstream lines(assembly);std::string line;
  while(std::getline(lines,line)){line=Trim(line.substr(0,line.find("//")));if(line.empty()||line[0]==';'||line.rfind("vs",0)==0)continue;
   auto space=line.find_first_of(" \t");std::string op=line.substr(0,space);if(op=="nop")continue;
   std::vector<std::string> args;std::istringstream items(line.substr(space+1));std::string arg;while(std::getline(items,arg,','))args.push_back(Trim(arg));
   auto get=[&](std::size_t n){if(n>=args.size())throw std::runtime_error("missing VS operand");return operand(args[n]);};
   std::string dest=operand(args.at(0),true),expression;
   if(op=="mov")expression=get(1);
   else if(op=="add"||op=="sub"||op=="mul")expression=get(1)+(op=="add"?"+":op=="sub"?"-":"*")+get(2);
   else if(op=="mad")expression=get(1)+"*"+get(2)+"+"+get(3);
   else if(op=="dp3"||op=="dp4")expression="dot(("+get(1)+")."+(op=="dp3"?"xyz":"xyzw")+",("+get(2)+")."+(op=="dp3"?"xyz":"xyzw")+").xxxx";
   else if(op=="rcp")expression="(1/("+get(1)+").x).xxxx";
   else if(op=="rsq")expression="rsqrt(abs(("+get(1)+").x)).xxxx";
   else if(op=="min"||op=="max")expression=op+"("+get(1)+","+get(2)+")";
   else if(op=="frc")expression="frac("+get(1)+")";
   else if(op=="m4x4"||op=="m4x3"||op=="m3x4"||op=="m3x3"||op=="m3x2"){
    int base=std::stoi(args.at(2).substr(1));int rows=op.back()-'0';std::string dimensions=op[1]=='4'?"xyzw":"xyz";
    expression="float4(";
    for(int row=0;row<4;++row){if(row)expression+=",";expression+=row<rows?"dot(("+get(1)+")."+dimensions+",VSConstants["+std::to_string(base+row)+"]."+dimensions+")":"0";}expression+=")";
   }else throw std::runtime_error("unsupported vertex instruction: "+op);
   auto mask=dest.find('.');if(mask!=std::string::npos)expression="("+expression+")"+dest.substr(mask);
   out<<dest<<"="<<expression<<";\n";
  }
  // taa101: the camera's sub-pixel jitter (zero unless bound for the primary camera).
  out<<"output.position=oPos;output.position.xy+=TemporalVertexJitter.xy*oPos.w;output.diffuse=oD0;output.specular=oD1;output.fog=oFog.x;output.pointSize=oPts.x;\n";
  for(int i=0;i<8;++i)out<<"output.uv"<<i<<"=oT"<<i<<";\n";
  out<<"return output;}\n";return out.str();
 }catch(const std::exception &e){error=e.what();return {};}
}

}
