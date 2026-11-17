// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <vector>
namespace GenZH::Modern {
struct ImageRGBA {
    std::uint32_t width=0,height=0;
    // Top-left origin, RGBA bytes; independent of graphics API and pointer size.
    std::vector<std::uint8_t> pixels;
};
bool DecodeTGA(std::istream &input,ImageRGBA &result,std::string &error);
bool LoadTGA(const std::filesystem::path &path,ImageRGBA &result,std::string &error);
bool ResizeRGBA(const ImageRGBA &source,std::uint32_t width,std::uint32_t height,ImageRGBA &result);
}
