// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <cstring>
#include "ByteEquality.h"
namespace GenZH::Modern {
// Cache the contents of an exclusively owned buffer, not context bindings.
// Reset whenever the buffer is recreated. A failed Map must not commit data.
template<std::size_t N> class ConstantUploadCache {
    std::array<unsigned char,N> bytes_{};bool valid_=false;
public:
    void Reset(){valid_=false;}
    HRESULT Upload(ID3D11DeviceContext *context,ID3D11Buffer *buffer,const void *data,
                   std::uint64_t &uploads,std::uint64_t &skips){
        if(valid_&&BytesEqual(bytes_.data(),data,N)){++skips;return S_OK;}
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr=context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        if(FAILED(hr))return hr;
        std::memcpy(mapped.pData,data,N);context->Unmap(buffer,0);
        std::memcpy(bytes_.data(),data,N);valid_=true;++uploads;return S_OK;
    }
};
}
