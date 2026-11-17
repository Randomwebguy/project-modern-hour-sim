// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include "LoadTelemetry.h"
namespace GenZH::Modern {
struct PreparedDDS {
 PreparedDDS()=default;PreparedDDS(const PreparedDDS&)=delete;PreparedDDS&operator=(const PreparedDDS&)=delete;
 PreparedDDS(PreparedDDS&&)=default;PreparedDDS&operator=(PreparedDDS&&)=default;
 UINT width=0,height=0,mips=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
 std::vector<unsigned char> data;std::vector<D3D11_SUBRESOURCE_DATA> levels;
};
// Deliberately narrow: bounded 2D, single-array, complete DX10 BC mip chains.
inline bool DecodePreparedDDS(std::vector<unsigned char> bytes,PreparedDDS &out,std::string &error){
 out={};auto fail=[&](const char *why){error=why;return false;};
 if(bytes.size()<148)return fail("truncated DDS header");
 auto u32=[&](std::size_t at){UINT value=0;std::memcpy(&value,bytes.data()+at,4);return value;};
 if(u32(0)!=0x20534444||u32(4)!=124||u32(76)!=32||u32(84)!=0x30315844)return fail("DDS DX10 header required");
 UINT width=u32(16),height=u32(12),mips=u32(28),format=u32(128);
 if(!width||!height||width>8192||height>8192||u32(132)!=3||u32(140)!=1||(u32(136)&4)||u32(24)>1||(u32(112)&0xFE00))return fail("unsupported DDS dimensions/array/cube");
 UINT expected=1;for(UINT n=std::max(width,height);n>1;n>>=1)++expected;
 if(mips!=expected)return fail("incomplete or oversized DDS mip chain");
 UINT block=16;if(format==DXGI_FORMAT_BC4_UNORM)block=8;
 else if(format!=DXGI_FORMAT_BC7_UNORM&&format!=DXGI_FORMAT_BC7_UNORM_SRGB)return fail("unsupported prepared DDS format");
 // D3D11 requires a block-aligned top level. Sub-4x4 lower mips remain valid.
 // Reject an unusable prepared cache so callers load the exact authored TGA.
 if((width&3u)||(height&3u))return fail("DDS BC top level must be a multiple of four; use authored RGBA source");
 std::size_t size=148;UINT w=width,h=height;
 for(UINT mip=0;mip<mips;++mip){size+=static_cast<std::size_t>(std::max(1u,(w+3)/4))*std::max(1u,(h+3)/4)*block;w=std::max(1u,w/2);h=std::max(1u,h/2);}
 if(size!=bytes.size())return fail("DDS payload size mismatch");
 out.width=width;out.height=height;out.mips=mips;
 // Compatibility shaders do their own IEC sRGB decode; avoid double decoding.
 out.format=static_cast<DXGI_FORMAT>(format==DXGI_FORMAT_BC7_UNORM_SRGB?DXGI_FORMAT_BC7_UNORM:format);
 out.data=std::move(bytes);out.levels.resize(mips);size=148;w=width;h=height;
 for(UINT mip=0;mip<mips;++mip){UINT pitch=std::max(1u,(w+3)/4)*block,rows=std::max(1u,(h+3)/4);
  out.levels[mip]={out.data.data()+size,pitch,pitch*rows};size+=static_cast<std::size_t>(pitch)*rows;w=std::max(1u,w/2);h=std::max(1u,h/2);}
 error.clear();return true;
}
inline bool IsPreparedCurrent(const std::filesystem::path &source,const std::filesystem::path &dds){
 WIN32_FILE_ATTRIBUTE_DATA attr{};if(!GetFileAttributesExW(source.c_str(),GetFileExInfoStandard,&attr))return false;
 ULARGE_INTEGER size{},time{};size.LowPart=attr.nFileSizeLow;size.HighPart=attr.nFileSizeHigh;time.LowPart=attr.ftLastWriteTime.dwLowDateTime;time.HighPart=attr.ftLastWriteTime.dwHighDateTime;
 std::ifstream stamp(std::filesystem::path(dds).replace_extension(L".source"),std::ios::binary);std::uint64_t saved[3]{};
 std::error_code ignored;const auto ddsSize=std::filesystem::file_size(dds,ignored);
 return !ignored&&stamp.read(reinterpret_cast<char*>(saved),sizeof(saved))&&stamp.peek()==EOF&&saved[0]==size.QuadPart&&saved[1]==time.QuadPart&&saved[2]==ddsSize;
}
inline HRESULT LoadPreparedTexture(ID3D11Device *device,const std::filesystem::path &source,Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &view){
 const auto dds=source.parent_path()/L"runtime-dds"/std::filesystem::path(source.filename()).replace_extension(L".dds");
 if(!IsPreparedCurrent(source,dds))return S_FALSE;
 GenZHLoad::Scope timing("texture_dds",source.filename().string());
 std::ifstream file(dds,std::ios::binary|std::ios::ate);if(!file)return S_FALSE;
 const auto length=file.tellg();if(length<148||length>96*1024*1024)return S_FALSE;
 std::vector<unsigned char> bytes(static_cast<std::size_t>(length));file.seekg(0);
 if(!file.read(reinterpret_cast<char*>(bytes.data()),length))return S_FALSE;
 timing.bytes=bytes.size();PreparedDDS image;std::string error;
 if(!DecodePreparedDDS(std::move(bytes),image,error))return S_FALSE;
 D3D11_TEXTURE2D_DESC desc{};desc.Width=image.width;desc.Height=image.height;desc.MipLevels=image.mips;
 desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=image.format;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;HRESULT hr=device->CreateTexture2D(&desc,image.levels.data(),texture.GetAddressOf());
 if(FAILED(hr))return hr;hr=device->CreateShaderResourceView(texture.Get(),nullptr,view.ReleaseAndGetAddressOf());timing.hit=SUCCEEDED(hr);return hr;
}
}
