// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <vector>
#include <cstdint>
#include "LoadTelemetry.h"
#include "NativeShaderCacheDigest.h"
namespace GenZH::Modern {
// D3DCompiler's optimizer can exhaust a 1 MiB caller stack on the combined
// legacy/PBR/terrain/atmosphere program. Reserve a bounded worker stack instead
// of reducing shader features or raising the stack of every game thread.
inline HRESULT CompileShaderOnWorker(const std::string &source,const char *entry,const char *profile,UINT flags,
    ID3DBlob **code,ID3DBlob **errors){
    struct Request {const std::string *source;const char *entry,*profile;UINT flags;ID3DBlob **code,**errors;HRESULT result=E_FAIL;};
    Request request{&source,entry,profile,flags,code,errors};
    HANDLE worker=CreateThread(nullptr,8u*1024*1024,[](LPVOID arg)->DWORD{
        auto &r=*static_cast<Request*>(arg);
        r.result=D3DCompile(r.source->data(),r.source->size(),"LegacyScene.hlsl",nullptr,nullptr,r.entry,r.profile,r.flags,0,r.code,r.errors);return 0;
    },&request,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!worker)return HRESULT_FROM_WIN32(GetLastError());
    const DWORD wait=WaitForSingleObject(worker,INFINITE);const DWORD error=wait==WAIT_FAILED?GetLastError():ERROR_SUCCESS;CloseHandle(worker);
    return wait==WAIT_OBJECT_0?request.result:HRESULT_FROM_WIN32(error);
}
inline std::uint64_t ShaderHash(const void *bytes,std::size_t size){
    auto *p=static_cast<const unsigned char*>(bytes);std::uint64_t hash=14695981039346656037ull;
    for(std::size_t i=0;i<size;++i){hash^=p[i];hash*=1099511628211ull;}return hash;
}
inline HRESULT CompileCachedShader(const std::string &source,const char *entry,const char *profile,
    Microsoft::WRL::ComPtr<ID3DBlob> &code,std::string &error,bool &hit){
    constexpr UINT flags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3;
    GenZHLoad::Scope timing("shader_prepare",std::string(entry)+" "+profile);
    const std::string key=std::string("v16 strict O3\n")+entry+'\n'+profile+'\n'+source;
    wchar_t directory[32768]{};const DWORD length=GetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",directory,32768);
    std::filesystem::path path;
    if(length&&length<32768){std::ostringstream name;name<<std::hex<<ShaderHash(key.data(),key.size())<<".shader";path=std::filesystem::path(directory)/L"shader-cache"/name.str();}
    struct Header{std::uint32_t magic,keyBytes,codeBytes,reserved;std::uint64_t codeHash;};
    static_assert(sizeof(Header)==24);
    constexpr std::uint32_t magic=0x5336315a;hit=false;
    std::array<std::uint8_t,32> digest{};const bool compact=ShaderKeyDigest(key.data(),key.size(),digest);
    const std::uint32_t tokenBytes=compact?32:static_cast<std::uint32_t>(key.size());
    const void *token=compact?static_cast<const void*>(digest.data()):static_cast<const void*>(key.data());
    if(!path.empty()){
        std::ifstream file(path,std::ios::binary);Header h{};
        if(file.read(reinterpret_cast<char*>(&h),sizeof(h))&&h.magic==magic&&
           ((h.reserved==0&&h.keyBytes==key.size()&&h.keyBytes<2*1024*1024)||(h.reserved==1&&compact&&h.keyBytes==32))&&h.codeBytes>=4&&h.codeBytes<4*1024*1024){
            std::string saved(h.keyBytes,'\0');file.read(saved.data(),h.keyBytes);
            const bool keyMatches=h.reserved==0?saved==key:std::memcmp(saved.data(),digest.data(),32)==0;
            // Full SHA-256 validates the complete source/entry/profile/flags,
            // not only the shorter filename hash. Old full keys remain supported.
            if(file&&keyMatches&&SUCCEEDED(D3DCreateBlob(h.codeBytes,code.ReleaseAndGetAddressOf()))){
                file.read(static_cast<char*>(code->GetBufferPointer()),h.codeBytes);
                const bool complete=file&&file.peek()==std::char_traits<char>::eof();
                if(complete&&!std::memcmp(code->GetBufferPointer(),"DXBC",4)&&ShaderHash(code->GetBufferPointer(),code->GetBufferSize())==h.codeHash){hit=true;timing.hit=true;timing.bytes=h.codeBytes;return S_OK;}
                code.Reset();
            }
        }
    }
    if(!path.empty()){
        // Persist the complete compile request before a blocking miss. Atomic,
        // per-key files avoid append races between offline worker threads.
        std::error_code ignored;auto requests=path.parent_path().parent_path()/L"shader-requests";
        std::filesystem::create_directories(requests,ignored);auto request=requests/path.filename();request.replace_extension(L".request");
        auto temporary=request;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetCurrentThreadId())+L".tmp";
        bool saved=false;{std::ofstream out(temporary,std::ios::binary);if(out){out<<entry<<'\n'<<profile<<'\n';out.write(source.data(),source.size());out.flush();saved=static_cast<bool>(out);}}
        if(!saved||!MoveFileExW(temporary.c_str(),request.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))std::filesystem::remove(temporary,ignored);
    }
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr=CompileShaderOnWorker(source,entry,profile,flags,code.ReleaseAndGetAddressOf(),errors.GetAddressOf());
    if(FAILED(hr)){error=errors?static_cast<const char*>(errors->GetBufferPointer()):"shader compile failed";return hr;}
    timing.bytes=code->GetBufferSize();
    if(!path.empty()){
        std::error_code ignored;std::filesystem::create_directories(path.parent_path(),ignored);
        Header h{magic,tokenBytes,static_cast<std::uint32_t>(code->GetBufferSize()),compact?1u:0u,ShaderHash(code->GetBufferPointer(),code->GetBufferSize())};
        auto temporary=path;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetCurrentThreadId())+L".tmp";
        bool saved=false;
        {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        if(file){file.write(reinterpret_cast<const char*>(&h),sizeof(h));file.write(static_cast<const char*>(token),h.keyBytes);file.write(static_cast<const char*>(code->GetBufferPointer()),h.codeBytes);file.flush();saved=static_cast<bool>(file);}}
        // Readers observe a complete file, even while an offline worker prepares it.
        if(!saved||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))std::filesystem::remove(temporary,ignored);
    }
    return S_OK;
}
}
