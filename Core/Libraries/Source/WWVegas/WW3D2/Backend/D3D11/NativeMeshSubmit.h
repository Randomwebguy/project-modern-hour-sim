// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace GenZH::Modern {
struct NativeIndexedDraw {
    std::uint32_t primitive=0,firstVertex=0,vertices=0,firstIndex=0,primitives=0;
    bool geometryOnly=false;
};
// WW3D has one active device. Older/unregistered devices keep their COM path.
// No foreign COM object RTTI cast, extra reference cycle or ownership transfer.
class NativeMeshDispatch {
public:
    using Submit=bool(*)(void*,const NativeIndexedDraw&,long&);
    using Begin=bool(*)(void*);using End=void(*)(void*);
private:
    void *device_=nullptr,*owner_=nullptr;Submit submit_=nullptr;bool geometry_=false;Begin begin_=nullptr;End end_=nullptr;
public:
    bool lazy_=false;
    bool LazyEnabled()const{return GeometryEnabled()&&lazy_;}
    bool GeometryEnabled()const{return device_&&submit_&&geometry_;}
    void Connect(void *device,void *owner,Submit submit,bool geometry=false,bool lazy=false,Begin begin=nullptr,End end=nullptr){device_=device;owner_=owner;submit_=submit;geometry_=geometry;lazy_=lazy;begin_=begin;end_=end;}
    void Disconnect(void *device,void *owner){if(device_==device&&owner_==owner){device_=owner_=nullptr;submit_=nullptr;begin_=nullptr;end_=nullptr;}}
    bool BeginScope(void *device,void *&owner,End &end)const{
        if(device!=device_||!device||!begin_||!end_||!begin_(owner_))return false;owner=owner_;end=end_;return true;
    }
    bool Try(void *device,const NativeIndexedDraw &draw,long &result)const{return device&&device==device_&&submit_&&submit_(owner_,draw,result);}
};
inline NativeMeshDispatch nativeMeshDispatch;
class ScopedNativeFixedWrites {
    void *owner_=nullptr;NativeMeshDispatch::End end_=nullptr;
public:
    explicit ScopedNativeFixedWrites(void *device){nativeMeshDispatch.BeginScope(device,owner_,end_);}
    ~ScopedNativeFixedWrites(){if(end_)end_(owner_);}
    ScopedNativeFixedWrites(const ScopedNativeFixedWrites&)=delete;
    ScopedNativeFixedWrites &operator=(const ScopedNativeFixedWrites&)=delete;
};
}
