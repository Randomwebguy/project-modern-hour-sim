// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <memory>
namespace GenZH::Modern {
// perf112: the render and texture-stage state of a draw. Packets captured at the same native state revision share one
// immutable snapshot (NativeStateMirror::Snapshot), so a draw no longer copies 2 KB of state. A packet that needs a
// different value clones the snapshot first (DrawPacket::MutableStates).
struct StateSnapshot {
    std::array<DWORD,256> render{};
    std::array<std::array<DWORD,32>,8> stages{};
};
}
