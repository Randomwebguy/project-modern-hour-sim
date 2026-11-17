// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <emmintrin.h>
#include <atomic>
#include <cstddef>
#include <cstring>
namespace GenZH::Modern {
// perf90: byte-range equality, exactly memcmp(a,b,n)==0, with SSE2 compares of
// 64 bytes per step. The renderer's per-packet equality tests compare mostly
// equal kilobyte blocks (packet constants, shadow stamps), where this is about
// twice as fast as memcmp and three times std::array ==. GENZH_SIMD_EQUAL=off
// uses memcmp.
inline std::atomic<bool> simdByteEquality{true};
inline bool BytesEqual(const void *a,const void *b,std::size_t n){
    if(!simdByteEquality.load(std::memory_order_relaxed))return !std::memcmp(a,b,n);
    const auto *x=static_cast<const unsigned char*>(a),*y=static_cast<const unsigned char*>(b);
    std::size_t i=0;
    for(;i+64<=n;i+=64){
        const __m128i e0=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x+i)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(y+i)));
        const __m128i e1=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x+i+16)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(y+i+16)));
        const __m128i e2=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x+i+32)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(y+i+32)));
        const __m128i e3=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x+i+48)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(y+i+48)));
        if(_mm_movemask_epi8(_mm_and_si128(_mm_and_si128(e0,e1),_mm_and_si128(e2,e3)))!=0xFFFF)return false;
    }
    for(;i+16<=n;i+=16)
        if(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x+i)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(y+i))))!=0xFFFF)return false;
    for(;i<n;++i)if(x[i]!=y[i])return false;
    return true;
}
}
