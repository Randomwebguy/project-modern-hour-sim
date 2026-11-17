// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <limits>
#include <cstdint>
namespace GenZH::Modern {
// A native CNG provider per caller thread; source/entry/profile/flags use SHA-256.
// Disk filenames remain unchanged. A provider failure retains the full-key format.
class ShaderDigestProvider {
 BCRYPT_ALG_HANDLE algorithm_=nullptr;DWORD objectBytes_=0;
public:
 ShaderDigestProvider(){
  if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0){algorithm_=nullptr;return;}
  DWORD returned=0;
  if(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes_),sizeof(objectBytes_),&returned,0)<0||returned!=sizeof(objectBytes_)||objectBytes_>1024){BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;}
 }
 ~ShaderDigestProvider(){if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
 ShaderDigestProvider(const ShaderDigestProvider&)=delete;ShaderDigestProvider& operator=(const ShaderDigestProvider&)=delete;
 bool Hash(const void *bytes,std::size_t count,std::array<std::uint8_t,32> &digest){
  if(!algorithm_||count>(std::numeric_limits<ULONG>::max)()||(!bytes&&count))return false;
  std::array<UCHAR,1024> object{};BCRYPT_HASH_HANDLE hash=nullptr;
  if(BCryptCreateHash(algorithm_,&hash,object.data(),objectBytes_,nullptr,0,0)<0)return false;
  const auto result=BCryptHashData(hash,const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)),static_cast<ULONG>(count),0);
  const bool ok=result>=0&&BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
  BCryptDestroyHash(hash);return ok;
 }
};
inline bool ShaderKeyDigest(const void *bytes,std::size_t count,std::array<std::uint8_t,32> &digest){thread_local ShaderDigestProvider provider;return provider.Hash(bytes,count,digest);}
}
