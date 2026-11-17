// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

// Native index values, entry counts and offsets are 32-bit end to end.
using W3DIndex = std::uint32_t;
using W3DIndexCount = std::uint32_t;
inline std::uint32_t W3DCheckedElementBytes(std::uint32_t count, std::uint32_t stride) {
    if (!count || !stride || count > std::numeric_limits<std::uint32_t>::max() / stride)
        throw std::out_of_range("W3D buffer byte size exceeds the graphics API range");
    return count * stride;
}
inline std::uint32_t W3DIndexByteSize(W3DIndexCount count) {
    constexpr std::uint32_t elementSize = sizeof(W3DIndex);
    if (!count || count > std::numeric_limits<std::uint32_t>::max() / elementSize)
        throw std::out_of_range("W3D index buffer size is invalid");
    return count * elementSize;
}
inline bool W3DIndexRangeFits(W3DIndexCount capacity, W3DIndexCount first, W3DIndexCount count) {
    return first <= capacity && count <= capacity - first;
}
