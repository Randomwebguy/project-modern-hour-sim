// SPDX-License-Identifier: GPL-3.0-or-later
// photo107: offline GPU checks of the new paths that do not need the game: every changed shader
// entry compiles, and the screen-space indirect light (GI) tints a floor next to a bright occluder
// while a far floor stays neutral (the same scene with the GI pass skipped is the reference).
#include <d3d11.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "ShaderBytecodeCache.h"
#include "ShaderSource.h"
#include "TemporalAAShaderSource.h"
#include "BloomShaderSource.h"
#include "AtmosphereShaderSource.h"
#include "GTAOGPU.h"
#include "GTAOShaderSource.h"
#include "LegacyShaderSource.h"
#include <limits>
#include "ColourGrade.h"

using Microsoft::WRL::ComPtr;
using namespace GenZH::Modern;
namespace {
unsigned checks = 0;
void Check(bool condition, const std::string &label) { ++checks; if (!condition) throw std::runtime_error(label); }
void HR(HRESULT hr, const std::string &label) {
    if (FAILED(hr)) { char code[16]; std::snprintf(code, sizeof(code), "%08lx", static_cast<unsigned long>(hr)); throw std::runtime_error(label + " 0x" + code); }
}
// Compiles one entry of a source text (the same cache the renderer uses).
ComPtr<ID3DBlob> Compile(const std::string &source, const char *entry, const char *profile) {
    ComPtr<ID3DBlob> code; std::string error; bool hit = false;
    const HRESULT hr = CompileCachedShader(source, entry, profile, code, error, hit);
    if (FAILED(hr)) throw std::runtime_error(std::string(entry) + ": " + error);
    return code;
}
// The atmosphere probes (WeatherSky.hlsl, Atmosphere.hlsl) on the CPU, for the IBL check at the end of main.
struct ProbeVector { float x, y, z; };
ProbeVector operator+(ProbeVector a, ProbeVector b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
ProbeVector operator*(ProbeVector a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float ProbeDot(ProbeVector a, ProbeVector b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
ProbeVector ProbeCross(ProbeVector a, ProbeVector b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
ProbeVector ProbeNormalise(ProbeVector a) { return a * (1.0f / std::sqrt(ProbeDot(a, a))); }
// WeatherCubeDirection: the face convention the probes read and write through (the standard D3D cube layout).
ProbeVector ProbeCubeDirection(UINT face, float u, float v) {
    switch (face) {
    case 0: return ProbeNormalise({1.0f, -v, -u});
    case 1: return ProbeNormalise({-1.0f, -v, u});
    case 2: return ProbeNormalise({u, 1.0f, v});
    case 3: return ProbeNormalise({u, -1.0f, -v});
    case 4: return ProbeNormalise({u, -v, 1.0f});
    default: return ProbeNormalise({-u, -v, -1.0f});
    }
}
void ProbeBasis(ProbeVector n, ProbeVector &tangent, ProbeVector &bitangent) {
    tangent = ProbeNormalise(ProbeCross(std::abs(n.z) < .999f ? ProbeVector{0.0f, 0.0f, 1.0f} : ProbeVector{1.0f, 0.0f, 0.0f}, n));
    bitangent = ProbeCross(n, tangent);
}
// ProbeRadicalInverse: the bit reversal the probe kernels use for their sample sequence.
float ProbeRadicalInverse(UINT bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
    bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
    bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
    return float(bits) * 2.3283064365386963e-10f;
}
// The synthetic sky: linear in the direction and positive, so bilinear cube sampling and the filters are exact to texel error.
float ProbeSky(ProbeVector d, UINT channel) {
    return channel == 0 ? .3f + .25f * d.y : channel == 1 ? .4f + .2f * d.x : .2f + .1f * d.z;
}

// The legacy source as the renderer compiles it: the texture slots and SampleStage stubs, and the fixed-function default programs
// (ProgramSource.h BuildProgramSource fills the same markers). Probes that never reach the legacy pixel path use this.
std::string PreparedLegacySource() {
    std::string textures;
    for (UINT i = 0; i < 8; ++i) {
        const std::string slot = std::to_string(i);
        textures += "Texture2D Texture" + slot + ":register(t" + slot + ");SamplerState Sampler" + slot + ":register(s" + slot + ");\n";
        textures += "float4 Sample" + slot + "(float4 coord){return 1;}\n";
    }
    textures += "float4 SampleStage(int index,float4 coord){return coord;}\n";
    std::string source = GenZH::Modern::LegacyShaderSource;
    const auto replaceMarker = [&source](const char *marker, const std::string &value) { source.replace(source.find(marker), std::strlen(marker), value); };
    replaceMarker("/*TEXTURES*/", textures);
    replaceMarker("/*PIXEL_PROGRAM*/", "float4 LegacyPixel(float4 diffuse,float4 specular,float4 coords[8]){return FixedPixel(diffuse,specular,coords);}");
    replaceMarker("/*VERTEX_PROGRAM*/", "LegacyOutput LegacyVS(LegacyInput input){return FixedVS(input);}");
    return source;
}

// Float16 texture helpers: the GI inputs are read as floats (R32 for depth, RGBA16F for the rest).
ComPtr<ID3D11Texture2D> Texture(ID3D11Device *device, UINT width, UINT height, DXGI_FORMAT format, UINT bind, const void *data, UINT rowBytes,
                                ComPtr<ID3D11ShaderResourceView> *view, ComPtr<ID3D11RenderTargetView> *target = nullptr) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, rowBytes, 0};
    ComPtr<ID3D11Texture2D> texture;
    HR(device->CreateTexture2D(&desc, data ? &init : nullptr, texture.GetAddressOf()), "texture");
    if (view) HR(device->CreateShaderResourceView(texture.Get(), nullptr, view->GetAddressOf()), "texture view");
    if (target) HR(device->CreateRenderTargetView(texture.Get(), nullptr, target->GetAddressOf()), "texture target");
    return texture;
}
struct GiConstants { float viewProjection[16], inverseViewProjection[16], eye[4], depthRange[4], frame[4]; };
struct Vec3 { float x, y, z; };
}

int main() {
    try {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf()), "device");

        // 1. Every changed entry point compiles (the renderer compiles them at run time).
        const std::string scene(ShaderSource, sizeof(ShaderSource) - 1);
        const std::string taa(TemporalAAShaderSource), bloom(BloomShaderSource);
        Compile(scene, "VSPresent", "vs_5_0");
        Compile(scene, "PSTonemap", "ps_5_0");
        const auto giPixel = Compile(scene, "PSGlobalIllumination", "ps_5_0");
        const auto composePixel = Compile(scene, "PSGlobalCompose", "ps_5_0");
        Compile(taa, "VSTemporal", "vs_5_0");
        Compile(taa, "PSTemporalResolve", "ps_5_0");
        Compile(taa, "PSTemporalSharpen", "ps_5_0");
        Compile(bloom, "VSBloom", "vs_5_0");
        Compile(bloom, "PSBloomPrefilter", "ps_5_0");
        Compile(bloom, "PSBloomDown", "ps_5_0");
        Compile(bloom, "PSBloomUp", "ps_5_0");
        Check(giPixel && composePixel, "scene entries compile");

        // Vertex stage of the full-screen passes (VSPresent from the scene text).
        ComPtr<ID3D11VertexShader> presentVS;
        {
            auto code = Compile(scene, "VSPresent", "vs_5_0");
            HR(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, presentVS.GetAddressOf()), "present vs");
        }
        ComPtr<ID3D11PixelShader> giPS, composePS;
        HR(device->CreatePixelShader(giPixel->GetBufferPointer(), giPixel->GetBufferSize(), nullptr, giPS.GetAddressOf()), "gi ps");
        HR(device->CreatePixelShader(composePixel->GetBufferPointer(), composePixel->GetBufferSize(), nullptr, composePS.GetAddressOf()), "compose ps");

        // 2. The indirect light. Camera: identity view, left-handed perspective (60 degrees, 1..100).
        const UINT W = 256, H = 256, GW = 128, GH = 128;
        const auto view = DirectX::XMMatrixIdentity();
        const auto projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(60.0f), 1.0f, 1.0f, 100.0f);
        const auto viewProjection = DirectX::XMMatrixMultiply(view, projection);
        const auto inverseViewProjection = DirectX::XMMatrixInverse(nullptr, viewProjection);
        const float floorZ = 12.0f;                 // a floor facing the camera (normal -z)
        const Vec3 sphereCentre{0.0f, 0.0f, 6.0f};  // a bright occluder in front of it
        const float sphereRadius = 1.2f;

        std::vector<float> depth(W * H), normal(W * H * 4), albedo(W * H * 4), lit(W * H * 4);
        std::vector<Vec3> floorPoint(W * H);
        std::vector<char> onFloor(W * H, 0);
        DirectX::XMFLOAT4X4 vpStore, ivpStore;
        DirectX::XMStoreFloat4x4(&vpStore, viewProjection);
        DirectX::XMStoreFloat4x4(&ivpStore, inverseViewProjection);
        auto unproject = [&](float ndcX, float ndcY, float ndcZ) {
            DirectX::XMVECTOR p = DirectX::XMVector4Transform(DirectX::XMVectorSet(ndcX, ndcY, ndcZ, 1), inverseViewProjection);
            p = DirectX::XMVectorScale(p, 1.0f / DirectX::XMVectorGetW(p));
            return Vec3{DirectX::XMVectorGetX(p), DirectX::XMVectorGetY(p), DirectX::XMVectorGetZ(p)};
        };
        for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) {
            const float ndcX = (x + .5f) / W * 2 - 1, ndcY = 1 - (y + .5f) / H * 2;
            const Vec3 nearPoint = unproject(ndcX, ndcY, 0), farPoint = unproject(ndcX, ndcY, 1);
            const Vec3 dir{farPoint.x - nearPoint.x, farPoint.y - nearPoint.y, farPoint.z - nearPoint.z};
            const size_t i = size_t(y) * W + x;
            // Sphere first, then the floor.
            const Vec3 oc{nearPoint.x - sphereCentre.x, nearPoint.y - sphereCentre.y, nearPoint.z - sphereCentre.z};
            const float a = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
            const float b = 2 * (oc.x * dir.x + oc.y * dir.y + oc.z * dir.z);
            const float c = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - sphereRadius * sphereRadius;
            const float disc = b * b - 4 * a * c;
            const float tFloor = (floorZ - nearPoint.z) / dir.z;
            if (disc >= 0 && (-b - std::sqrt(disc)) / (2 * a) < tFloor && (-b - std::sqrt(disc)) / (2 * a) > 0) {
                const float t = (-b - std::sqrt(disc)) / (2 * a);
                const Vec3 s{nearPoint.x + dir.x * t, nearPoint.y + dir.y * t, nearPoint.z + dir.z * t};
                DirectX::XMVECTOR clip = DirectX::XMVector4Transform(DirectX::XMVectorSet(s.x, s.y, s.z, 1), viewProjection);
                depth[i] = DirectX::XMVectorGetZ(clip) / DirectX::XMVectorGetW(clip);
                const Vec3 n{(s.x - sphereCentre.x) / sphereRadius, (s.y - sphereCentre.y) / sphereRadius, (s.z - sphereCentre.z) / sphereRadius};
                normal[i * 4] = n.x; normal[i * 4 + 1] = n.y; normal[i * 4 + 2] = n.z; normal[i * 4 + 3] = 1;
                albedo[i * 4] = albedo[i * 4 + 1] = albedo[i * 4 + 2] = .8f; albedo[i * 4 + 3] = 1;
                lit[i * 4] = 4.0f; lit[i * 4 + 1] = .2f; lit[i * 4 + 2] = .2f; lit[i * 4 + 3] = 1;  // the emitter is red
            } else {
                const float t = tFloor;
                const Vec3 f{nearPoint.x + dir.x * t, nearPoint.y + dir.y * t, nearPoint.z + dir.z * t};
                floorPoint[i] = f; onFloor[i] = 1;
                DirectX::XMVECTOR clip = DirectX::XMVector4Transform(DirectX::XMVectorSet(f.x, f.y, f.z, 1), viewProjection);
                depth[i] = DirectX::XMVectorGetZ(clip) / DirectX::XMVectorGetW(clip);
                normal[i * 4] = 0; normal[i * 4 + 1] = 0; normal[i * 4 + 2] = -1; normal[i * 4 + 3] = 1;
                albedo[i * 4] = albedo[i * 4 + 1] = albedo[i * 4 + 2] = .8f; albedo[i * 4 + 3] = 1;
                lit[i * 4] = lit[i * 4 + 1] = lit[i * 4 + 2] = .5f; lit[i * 4 + 3] = 1;
            }
        }
        // Half-float copies of the colour inputs (RGBA16F), depth as R32.
        auto toHalf = [](const std::vector<float> &floats) {
            std::vector<DirectX::PackedVector::HALF> halves(floats.size());
            for (size_t i = 0; i < floats.size(); ++i) halves[i] = DirectX::PackedVector::XMConvertFloatToHalf(floats[i]);
            return halves;
        };
        const auto normalHalf = toHalf(normal), albedoHalf = toHalf(albedo), litHalf = toHalf(lit);

        ComPtr<ID3D11ShaderResourceView> depthView, normalView, albedoView, litView, giView;
        ComPtr<ID3D11RenderTargetView> giTarget, composedTarget;
        Texture(device.Get(), W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth.data(), W * 4, &depthView);
        Texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, normalHalf.data(), W * 8, &normalView);
        Texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, albedoHalf.data(), W * 8, &albedoView);
        auto litTexture = Texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, litHalf.data(), W * 8, &litView, &composedTarget);
        // The indirect light target (half resolution, as the renderer uses) and the composed result.
        ComPtr<ID3D11Texture2D> giTexture = Texture(device.Get(), GW, GH, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, nullptr, 0, &giView, &giTarget);

        // Constant buffer (b13): the same layout as the renderer's GlobalIlluminationConstants.
        GiConstants k{};
        DirectX::XMFLOAT4X4 vpCopy;DirectX::XMStoreFloat4x4(&vpCopy, viewProjection);
        std::memcpy(k.viewProjection, &vpCopy, 64);
        std::memcpy(k.inverseViewProjection, &ivpStore, 64);
        k.eye[3] = 1;
        k.depthRange[0] = 0; k.depthRange[1] = 1; k.depthRange[2] = .55f; k.depthRange[3] = 6.0f;  // strength, radius (world units)
        k.frame[1] = float(W); k.frame[2] = float(H);
        D3D11_BUFFER_DESC constantDesc{}; constantDesc.ByteWidth = sizeof(GiConstants); constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA constantData{&k, 0, 0};
        ComPtr<ID3D11Buffer> constants;
        HR(device->CreateBuffer(&constantDesc, &constantData, constants.GetAddressOf()), "constants");

        // Samplers and blend states as the renderer sets them.
        D3D11_SAMPLER_DESC sampler{}; sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> linear; HR(device->CreateSamplerState(&sampler, linear.GetAddressOf()), "sampler");
        D3D11_BLEND_DESC additive{}; auto &target = additive.RenderTarget[0];
        target.BlendEnable = TRUE; target.SrcBlend = D3D11_BLEND_ONE; target.DestBlend = D3D11_BLEND_ONE; target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO; target.DestBlendAlpha = D3D11_BLEND_ONE; target.BlendOpAlpha = D3D11_BLEND_OP_ADD; target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> additiveState; HR(device->CreateBlendState(&additive, additiveState.GetAddressOf()), "blend");
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rasterState; HR(device->CreateRasterizerState(&raster, rasterState.GetAddressOf()), "raster");

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(presentVS.Get(), nullptr, 0);
        context->RSSetState(rasterState.Get());
        ID3D11SamplerState *samplers[1] = {linear.Get()};
        context->PSSetSamplers(0, 1, samplers);
        ID3D11Buffer *constantBuffers[1] = {constants.Get()};
        context->PSSetConstantBuffers(13, 1, constantBuffers);

        // Pass 1: the gather at half resolution.
        D3D11_VIEWPORT half{0, 0, float(GW), float(GH), 0, 1};
        context->RSSetViewports(1, &half);
        ID3D11RenderTargetView *rtv = giTarget.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->PSSetShader(giPS.Get(), nullptr, 0);
        ID3D11ShaderResourceView *inputs[5] = {depthView.Get(), normalView.Get(), albedoView.Get(), nullptr, litView.Get()};
        context->PSSetShaderResources(24, 3, inputs);
        context->PSSetShaderResources(28, 1, &inputs[4]);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *none[5] = {};
        context->PSSetShaderResources(24, 5, none);

        // Pass 2: the indirect light is added to the lit colour (a copy of it, as the target).
        D3D11_VIEWPORT full{0, 0, float(W), float(H), 0, 1};
        context->RSSetViewports(1, &full);
        ComPtr<ID3D11Texture2D> composed;
        {
            D3D11_TEXTURE2D_DESC desc{}; litTexture->GetDesc(&desc); desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            HR(device->CreateTexture2D(&desc, nullptr, composed.GetAddressOf()), "composed");
            context->CopyResource(composed.Get(), litTexture.Get());
        }
        ComPtr<ID3D11RenderTargetView> composedRtv; HR(device->CreateRenderTargetView(composed.Get(), nullptr, composedRtv.GetAddressOf()), "composed rtv");
        rtv = composedRtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->OMSetBlendState(additiveState.Get(), nullptr, 0xffffffff);
        context->PSSetShader(composePS.Get(), nullptr, 0);
        ID3D11ShaderResourceView *radiance = giView.Get();
        context->PSSetShaderResources(27, 1, &radiance);
        context->Draw(3, 0);
        context->PSSetShaderResources(27, 1, none);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

        // Read the composed image back.
        D3D11_TEXTURE2D_DESC readDesc{}; composed->GetDesc(&readDesc); readDesc.Usage = D3D11_USAGE_STAGING; readDesc.BindFlags = 0; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; HR(device->CreateTexture2D(&readDesc, nullptr, staging.GetAddressOf()), "staging");
        context->CopyResource(staging.Get(), composed.Get());
        D3D11_MAPPED_SUBRESOURCE map{}; HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "map");
        std::vector<float> result(W * H * 3);
        for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) {
            const auto *row = reinterpret_cast<const DirectX::PackedVector::HALF *>(static_cast<const unsigned char *>(map.pData) + y * map.RowPitch);
            for (int c = 0; c < 3; ++c) result[(size_t(y) * W + x) * 3 + c] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + c]);
        }
        context->Unmap(staging.Get(), 0);

        // Floor pixels near the emitter (within 3 units of the sphere, in the plane) against far ones (over 9 units).
        double nearRedBias = 0, farRedBias = 0; size_t nearCount = 0, farCount = 0; bool finite = true;
        for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            for (int c = 0; c < 3; ++c) finite = finite && std::isfinite(result[i * 3 + c]) && result[i * 3 + c] >= 0;
            if (!onFloor[i]) continue;
            const float dx = floorPoint[i].x - sphereCentre.x, dy = floorPoint[i].y - sphereCentre.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const double redBias = result[i * 3] - result[i * 3 + 1];
            if (distance < 3.0f) { nearRedBias += redBias; ++nearCount; }
            else if (distance > 9.0f) { farRedBias += redBias; ++farCount; }
        }
        Check(finite, "indirect light output is finite and not negative");
        Check(nearCount > 100 && farCount > 100, "floor samples near and far from the emitter");
        nearRedBias /= std::max<size_t>(nearCount, 1); farRedBias /= std::max<size_t>(farCount, 1);
        std::printf("indirect light: floor red bias near %.4f (%zu px), far %.4f (%zu px)\n", nearRedBias, nearCount, farRedBias, farCount);
        Check(nearRedBias > .02, "the floor next to the red emitter takes its colour");
        Check(std::abs(farRedBias) < .01, "the floor far from the emitter stays neutral");
        Check(nearRedBias > farRedBias + .02, "the bounce is local to the emitter");

        // 3. The colour grade of the tone map (the LUT is this function sampled on a 32^3 lattice).
        {
            for (int step = 0; step <= 10; ++step) {
                float colour[3] = {step / 10.0f, .25f + .05f * step, 1.0f - step / 10.0f}, graded[3] = {colour[0], colour[1], colour[2]};
                GradeColour(graded, 1.0f, 0.0f, 1.0f);
                Check(std::abs(graded[0] - colour[0]) < 1e-6f && std::abs(graded[1] - colour[1]) < 1e-6f && std::abs(graded[2] - colour[2]) < 1e-6f, "neutral grade is the identity");
            }
            const float source[3] = {.6f, .3f, .1f}, luma = .2126f * source[0] + .7152f * source[1] + .0722f * source[2];
            float greyed[3] = {source[0], source[1], source[2]}, halfSat[3] = {source[0], source[1], source[2]};
            GradeColour(greyed, 0.0f, 0.0f, 1.0f);
            GradeColour(halfSat, .5f, 0.0f, 1.0f);
            const float halfLuma = .2126f * halfSat[0] + .7152f * halfSat[1] + .0722f * halfSat[2];
            Check(std::abs(greyed[0] - luma) < 1e-5f && std::abs(greyed[1] - luma) < 1e-5f && std::abs(greyed[2] - luma) < 1e-5f, "zero saturation is grey at the same luma");
            Check(std::abs(halfLuma - luma) < 1e-5f && halfSat[0] < source[0] && halfSat[2] > source[2], "saturation keeps luma and pulls channels toward it");
            float hot[3] = {.9f, .9f, .9f}, cold[3] = {.05f, .05f, .05f};
            GradeColour(hot, 1.0f, 1.0f, 1.0f);
            GradeColour(cold, 1.0f, 1.0f, 1.0f);
            Check(hot[0] > .9f && hot[2] < .9f && std::abs(hot[1] - .9f) < 1e-6f, "warmth lifts red and cuts blue in highlights");
            Check(std::abs(cold[0] - .05f) < 1e-6f && std::abs(cold[2] - .05f) < 1e-6f, "warmth leaves shadows alone");
            float up[3] = {.8f, .8f, .8f}, down[3] = {.05f, .05f, .05f}, mid[3] = {.18f, .18f, .18f};
            GradeColour(up, 1.0f, 0.0f, 1.2f);
            GradeColour(down, 1.0f, 0.0f, 1.2f);
            GradeColour(mid, 1.0f, 0.0f, 1.2f);
            Check(up[0] > .8f && down[0] < .05f && std::abs(mid[0] - .18f) < 1e-6f, "contrast pivots at 18% grey");
            float previous = -1.0f;
            bool ordered = true, inRange = true;
            for (int step = 0; step < 32; ++step) {
                float ramp[3] = {step / 31.0f, step / 31.0f, step / 31.0f};
                GradeColour(ramp, 1.6f, 1.0f, 2.5f);
                for (const float channel : ramp) inRange = inRange && std::isfinite(channel) && channel >= 0.0f && channel <= 1.0f;
                ordered = ordered && ramp[0] >= previous;
                previous = ramp[0];
            }
            Check(inRange && ordered, "the grade stays in range and keeps the ramp ordered");
        }

        // 4. The colour LUT in the tone map. PSTonemap looks the LUT up (built from the grade, as RenderDevice builds it) after
        //    the filmic curve, at the grade-off colour. The output must be the lattice's trilinear value there, and the lattice
        //    must match the grade it was built from.
        {
            const UINT tmSize = 256, tmLut = 32;
            const float saturation = 1.3f, warmth = 0.6f, contrast = 1.1f;
            const auto tonemapBlob = Compile(scene, "PSTonemap", "ps_5_0");
            ComPtr<ID3D11PixelShader> tonemapShader;
            HR(device->CreatePixelShader(tonemapBlob->GetBufferPointer(), tonemapBlob->GetBufferSize(), nullptr, tonemapShader.GetAddressOf()), "tone map pixel shader");
            // HDR input: a ramp from 0 to 3 in each channel, so the filmic curve and the LUT both see the whole range.
            std::vector<float> hdrTexels(size_t(tmSize) * tmSize * 4);
            for (UINT y = 0; y < tmSize; ++y) for (UINT x = 0; x < tmSize; ++x) {
                float *texel = &hdrTexels[(size_t(y) * tmSize + x) * 4];
                texel[0] = 3.0f * x / (tmSize - 1); texel[1] = 3.0f * y / (tmSize - 1); texel[2] = 1.5f * (x + y) / (tmSize - 1); texel[3] = 1;
            }
            ComPtr<ID3D11ShaderResourceView> hdrView;
            Texture(device.Get(), tmSize, tmSize, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, hdrTexels.data(), tmSize * 16, &hdrView);
            // One ramp per channel, as RenderDevice builds it (R16G16B16A16_UNORM, default ramp i*257). PSTonemap reads each channel's
            // own ramp at (encoded*255+.5)/256, so the default ramp is the identity and the output is the linear tone-mapped value.
            std::vector<unsigned short> gammaTable(256 * 4);
            for (UINT i = 0; i < 256; ++i) for (UINT c = 0; c < 4; ++c) gammaTable[i * 4 + c] = static_cast<unsigned short>(c < 3 ? i * 257 : 65535);
            D3D11_TEXTURE1D_DESC gammaDesc{}; gammaDesc.Width = 256; gammaDesc.MipLevels = gammaDesc.ArraySize = 1; gammaDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM; gammaDesc.Usage = D3D11_USAGE_IMMUTABLE; gammaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA gammaInit{gammaTable.data(), 0, 0};
            ComPtr<ID3D11Texture1D> gammaTexture; HR(device->CreateTexture1D(&gammaDesc, &gammaInit, gammaTexture.GetAddressOf()), "gamma curve");
            ComPtr<ID3D11ShaderResourceView> gammaView; HR(device->CreateShaderResourceView(gammaTexture.Get(), nullptr, gammaView.GetAddressOf()), "gamma curve view");
            // The colour LUT as the renderer builds it: 32^3, linear, sampled after the curve.
            std::vector<float> lutTexels(size_t(tmLut) * tmLut * tmLut * 4);
            for (UINT b = 0; b < tmLut; ++b) for (UINT g = 0; g < tmLut; ++g) for (UINT r = 0; r < tmLut; ++r) {
                float colour[3] = {float(r) / (tmLut - 1), float(g) / (tmLut - 1), float(b) / (tmLut - 1)};
                GradeColour(colour, saturation, warmth, contrast);
                float *texel = &lutTexels[(size_t(b) * tmLut * tmLut + size_t(g) * tmLut + r) * 4];
                texel[0] = colour[0]; texel[1] = colour[1]; texel[2] = colour[2]; texel[3] = 1;
            }
            D3D11_TEXTURE3D_DESC lutDesc{}; lutDesc.Width = lutDesc.Height = lutDesc.Depth = tmLut; lutDesc.MipLevels = 1; lutDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; lutDesc.Usage = D3D11_USAGE_IMMUTABLE; lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA lutInit{lutTexels.data(), tmLut * 16, tmLut * tmLut * 16};
            ComPtr<ID3D11Texture3D> lutTexture; HR(device->CreateTexture3D(&lutDesc, &lutInit, lutTexture.GetAddressOf()), "tone map LUT");
            ComPtr<ID3D11ShaderResourceView> lutView; HR(device->CreateShaderResourceView(lutTexture.Get(), nullptr, lutView.GetAddressOf()), "tone map LUT view");
            // FrameData (b1): CameraExposure = (0, 0, 0, 1) is what the tone map reads. PostData (b2): texel size, no bloom,
            // exposure trim 1, and the LUT switch in the last component of PostGrade.
            std::vector<float> frameConstants(256, 0.0f); frameConstants[3] = 1.0f;
            D3D11_BUFFER_DESC frameDesc{}; frameDesc.ByteWidth = UINT(frameConstants.size() * 4); frameDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA frameInit{frameConstants.data(), 0, 0};
            ComPtr<ID3D11Buffer> frameBuffer; HR(device->CreateBuffer(&frameDesc, &frameInit, frameBuffer.GetAddressOf()), "frame constants");
            const float postOff[12] = {1.0f / tmSize, 1.0f / tmSize, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
            const float postOn[12] = {1.0f / tmSize, 1.0f / tmSize, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
            ComPtr<ID3D11Buffer> postOffBuffer, postOnBuffer;
            D3D11_BUFFER_DESC postDesc{}; postDesc.ByteWidth = sizeof(postOff); postDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA postOffInit{postOff, 0, 0}, postOnInit{postOn, 0, 0};
            HR(device->CreateBuffer(&postDesc, &postOffInit, postOffBuffer.GetAddressOf()), "post constants");
            HR(device->CreateBuffer(&postDesc, &postOnInit, postOnBuffer.GetAddressOf()), "post constants");
            // Renders the tone map into a float target and reads it back. The output is linear: PSTonemap's sRGB encode and decode cancel.
            auto toneMap = [&](bool applyGrade) {
                ComPtr<ID3D11RenderTargetView> toneTargetView;
                ComPtr<ID3D11Texture2D> toneTarget = Texture(device.Get(), tmSize, tmSize, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET, nullptr, 0, nullptr, &toneTargetView);
                ID3D11RenderTargetView *toneRtv = toneTargetView.Get();
                context->OMSetRenderTargets(1, &toneRtv, nullptr);
                D3D11_VIEWPORT toneViewport{0, 0, float(tmSize), float(tmSize), 0, 1};
                context->RSSetViewports(1, &toneViewport);
                context->RSSetState(rasterState.Get());
                context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
                context->IASetInputLayout(nullptr);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(presentVS.Get(), nullptr, 0);
                context->PSSetShader(tonemapShader.Get(), nullptr, 0);
                ID3D11ShaderResourceView *toneViews[4] = {hdrView.Get(), gammaView.Get(), nullptr, lutView.Get()};
                context->PSSetShaderResources(0, 4, toneViews);
                ID3D11SamplerState *toneSampler = linear.Get();
                context->PSSetSamplers(0, 1, &toneSampler);
                ID3D11Buffer *frameSlot = frameBuffer.Get();
                ID3D11Buffer *postSlot = applyGrade ? postOnBuffer.Get() : postOffBuffer.Get();
                context->PSSetConstantBuffers(1, 1, &frameSlot);
                context->PSSetConstantBuffers(2, 1, &postSlot);
                context->Draw(3, 0);
                ID3D11ShaderResourceView *unbind[4] = {};
                context->PSSetShaderResources(0, 4, unbind);
                D3D11_TEXTURE2D_DESC toneStagingDesc{}; toneTarget->GetDesc(&toneStagingDesc);
                toneStagingDesc.Usage = D3D11_USAGE_STAGING; toneStagingDesc.BindFlags = 0; toneStagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> toneStaging; HR(device->CreateTexture2D(&toneStagingDesc, nullptr, toneStaging.GetAddressOf()), "tone map staging");
                context->CopyResource(toneStaging.Get(), toneTarget.Get());
                D3D11_MAPPED_SUBRESOURCE toneMapped{}; HR(context->Map(toneStaging.Get(), 0, D3D11_MAP_READ, 0, &toneMapped), "tone map readback");
                std::vector<float> toneReadback(size_t(tmSize) * tmSize * 4);
                for (UINT y = 0; y < tmSize; ++y) std::memcpy(&toneReadback[size_t(y) * tmSize * 4], static_cast<const unsigned char *>(toneMapped.pData) + y * toneMapped.RowPitch, tmSize * 16);
                context->Unmap(toneStaging.Get(), 0);
                return toneReadback;
            };
            const auto tonePlain = toneMap(false), toneGraded = toneMap(true);
            // The lattice point for a channel value v sits at texel index v*31, the position the shader's
            // mapped*(31/32)+.5/32 coordinate reaches. The reference reads the lattice as it was written, so a transposed
            // or misindexed LUT fails the comparison below.
            auto latticeAt = [&](UINT ix, UINT iy, UINT iz, UINT channel) { return lutTexels[(size_t(iz) * tmLut * tmLut + size_t(iy) * tmLut + ix) * 4 + channel]; };
            auto lutTrilinear = [&](const float (&lutInput)[3], UINT channel) {
                float lutPos[3], lutFrac[3];
                UINT lutBase[3];
                for (UINT axis = 0; axis < 3; ++axis) {
                    lutPos[axis] = std::clamp(lutInput[axis], 0.0f, 1.0f) * (tmLut - 1);
                    lutBase[axis] = std::min(UINT(lutPos[axis]), tmLut - 2);
                    lutFrac[axis] = lutPos[axis] - float(lutBase[axis]);
                }
                double lutValue = 0;
                for (UINT corner = 0; corner < 8; ++corner) {
                    double lutWeight = 1;
                    UINT lutIndex[3];
                    for (UINT axis = 0; axis < 3; ++axis) {
                        const bool high = ((corner >> axis) & 1) != 0;
                        lutIndex[axis] = lutBase[axis] + (high ? 1u : 0u);
                        lutWeight *= high ? lutFrac[axis] : 1 - lutFrac[axis];
                    }
                    lutValue += lutWeight * latticeAt(lutIndex[0], lutIndex[1], lutIndex[2], channel);
                }
                return float(lutValue);
            };
            // The fitted filmic curve of the tone map (saturated), for the grade-off check.
            auto filmicCurve = [](float h) { return std::clamp((h * (2.51f * h + .03f)) / (h * (2.43f * h + .59f) + .14f), 0.0f, 1.0f); };
            double toneWorstPlain = 0, toneWorstError = 0, toneWorstGrade = 0, toneLargestChange = 0;
            for (UINT y = 0; y < tmSize; y += 4) for (UINT x = 0; x < tmSize; x += 4) {
                const size_t i = (size_t(y) * tmSize + x) * 4;
                // The HDR ramp at this pixel (see hdrTexels) and the lattice input the LUT should see.
                const float hdrIn[3] = {3.0f * x / (tmSize - 1), 3.0f * y / (tmSize - 1), 1.5f * (x + y) / (tmSize - 1)};
                const float lutInput[3] = {tonePlain[i], tonePlain[i + 1], tonePlain[i + 2]};
                float gradedRef[3] = {lutInput[0], lutInput[1], lutInput[2]};
                GradeColour(gradedRef, saturation, warmth, contrast);
                for (UINT channel = 0; channel < 3; ++channel) {
                    toneWorstPlain = std::max(toneWorstPlain, double(std::abs(lutInput[channel] - filmicCurve(hdrIn[channel]))));
                    toneWorstError = std::max(toneWorstError, double(std::abs(toneGraded[i + channel] - lutTrilinear(lutInput, channel))));
                    toneWorstGrade = std::max(toneWorstGrade, double(std::abs(toneGraded[i + channel] - gradedRef[channel])));
                    toneLargestChange = std::max(toneLargestChange, double(std::abs(toneGraded[i + channel] - tonePlain[i + channel])));
                }
            }
            std::printf("tone map colour LUT: worst error against the filmic curve %.5f, against the lattice %.5f, against the continuous grade %.5f, largest change %.4f\n",
                        toneWorstPlain, toneWorstError, toneWorstGrade, toneLargestChange);
            Check(toneWorstPlain < 0.002, "the grade-off tone map is the filmic curve of the HDR ramp (identity gamma)");
            Check(toneLargestChange > 0.02, "the colour LUT changes the tone-mapped colours");
            Check(toneWorstError < 0.01, "the tone map samples the colour LUT at its input (trilinear reference within 0.01)");
            // The grade clamps at 0 and 1, so a 31-step lattice misses those kinks by up to about 0.01 per axis.
            Check(toneWorstGrade < 0.03, "the colour LUT reproduces the grade within lattice error");
        }

        // 5. The atmosphere's IBL probes (WeatherProbes.cpp, Atmosphere.hlsl). A synthetic sky, linear in the direction, goes
        //    through the cube and the two filters; the GPU results are checked against the same sample directions on the CPU.
        {
            const auto atmosphere = std::string(AtmosphereShaderSource);
            const auto specularBlob = Compile(atmosphere, "CSWeatherSpecular", "cs_5_0");
            const auto diffuseBlob = Compile(atmosphere, "CSWeatherDiffuse", "cs_5_0");
            ComPtr<ID3D11ComputeShader> specularCS, diffuseCS;
            HR(device->CreateComputeShader(specularBlob->GetBufferPointer(), specularBlob->GetBufferSize(), nullptr, specularCS.GetAddressOf()), "specular probe shader");
            HR(device->CreateComputeShader(diffuseBlob->GetBufferPointer(), diffuseBlob->GetBufferSize(), nullptr, diffuseCS.GetAddressOf()), "diffuse probe shader");
            // The raw sky: a 64^2 R16G16B16A16_FLOAT cube, as the probes use, each texel holding the sky at its own direction.
            const UINT skySize = 64;
            std::vector<unsigned short> skyHalves(size_t(skySize) * skySize * 6 * 4);
            for (UINT face = 0; face < 6; ++face) for (UINT y = 0; y < skySize; ++y) for (UINT x = 0; x < skySize; ++x) {
                const ProbeVector d = ProbeCubeDirection(face, (x + .5f) / skySize * 2 - 1, (y + .5f) / skySize * 2 - 1);
                unsigned short *texel = &skyHalves[((size_t(face) * skySize + y) * skySize + x) * 4];
                for (UINT c = 0; c < 3; ++c) texel[c] = DirectX::PackedVector::XMConvertFloatToHalf(ProbeSky(d, c));
                texel[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
            }
            D3D11_SUBRESOURCE_DATA skyInit[6]{};
            for (UINT face = 0; face < 6; ++face) skyInit[face] = {&skyHalves[size_t(face) * skySize * skySize * 4], skySize * 8, 0};
            D3D11_TEXTURE2D_DESC skyDesc{};
            skyDesc.Width = skyDesc.Height = skySize; skyDesc.MipLevels = 1; skyDesc.ArraySize = 6; skyDesc.SampleDesc.Count = 1;
            skyDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; skyDesc.Usage = D3D11_USAGE_IMMUTABLE; skyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; skyDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
            ComPtr<ID3D11Texture2D> skyTexture; HR(device->CreateTexture2D(&skyDesc, skyInit, skyTexture.GetAddressOf()), "synthetic sky");
            ComPtr<ID3D11ShaderResourceView> skyView; HR(device->CreateShaderResourceView(skyTexture.Get(), nullptr, skyView.GetAddressOf()), "synthetic sky view");
            // Runs one probe kernel into a probeSide x probeSide x 6 target and reads it back as RGBA floats.
            auto runProbe = [&](ID3D11ComputeShader *probeShader, UINT probeSide, float probeRoughness) {
                D3D11_TEXTURE2D_DESC outDesc{};
                outDesc.Width = outDesc.Height = probeSide; outDesc.MipLevels = 1; outDesc.ArraySize = 6; outDesc.SampleDesc.Count = 1;
                outDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; outDesc.Usage = D3D11_USAGE_DEFAULT; outDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                ComPtr<ID3D11Texture2D> probeTarget; HR(device->CreateTexture2D(&outDesc, nullptr, probeTarget.GetAddressOf()), "probe target");
                ComPtr<ID3D11UnorderedAccessView> probeView; HR(device->CreateUnorderedAccessView(probeTarget.Get(), nullptr, probeView.GetAddressOf()), "probe target view");
                const float probeFilter[4] = {probeRoughness, float(probeSide), 0, 0};
                D3D11_BUFFER_DESC filterDesc{}; filterDesc.ByteWidth = 16; filterDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                D3D11_SUBRESOURCE_DATA filterInit{probeFilter, 0, 0};
                ComPtr<ID3D11Buffer> filterBuffer; HR(device->CreateBuffer(&filterDesc, &filterInit, filterBuffer.GetAddressOf()), "probe filter");
                ID3D11ShaderResourceView *skySlot = skyView.Get();
                ID3D11Buffer *filterSlot = filterBuffer.Get();
                ID3D11SamplerState *clampSlot = linear.Get();
                ID3D11UnorderedAccessView *probeUAV = probeView.Get();
                context->CSSetShader(probeShader, nullptr, 0);
                context->CSSetShaderResources(33, 1, &skySlot);
                context->CSSetConstantBuffers(8, 1, &filterSlot);
                context->CSSetSamplers(12, 1, &clampSlot);
                context->CSSetUnorderedAccessViews(0, 1, &probeUAV, nullptr);
                context->Dispatch((probeSide + 7) / 8, (probeSide + 7) / 8, 6);
                ID3D11UnorderedAccessView *noUAV = nullptr; context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
                ID3D11ShaderResourceView *noSRV = nullptr; context->CSSetShaderResources(33, 1, &noSRV);
                context->CSSetShader(nullptr, nullptr, 0);
                D3D11_TEXTURE2D_DESC readDesc = outDesc; readDesc.Usage = D3D11_USAGE_STAGING; readDesc.BindFlags = 0; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> probeStaging; HR(device->CreateTexture2D(&readDesc, nullptr, probeStaging.GetAddressOf()), "probe staging");
                context->CopyResource(probeStaging.Get(), probeTarget.Get());
                std::vector<float> probeOut(size_t(probeSide) * probeSide * 6 * 4);
                for (UINT face = 0; face < 6; ++face) {
                    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(context->Map(probeStaging.Get(), face, D3D11_MAP_READ, 0, &mapped), "probe readback");
                    for (UINT y = 0; y < probeSide; ++y) {
                        const unsigned short *row = reinterpret_cast<const unsigned short *>(static_cast<const unsigned char *>(mapped.pData) + size_t(y) * mapped.RowPitch);
                        for (UINT x = 0; x < probeSide; ++x) for (UINT c = 0; c < 4; ++c)
                            probeOut[((size_t(face) * probeSide + y) * probeSide + x) * 4 + c] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + c]);
                    }
                    context->Unmap(probeStaging.Get(), face);
                }
                return probeOut;
            };
            // Orientation: the mip 0 copy reads the raw sky at each texel's own direction, so every texel returns the sky there.
            const auto mirrorOut = runProbe(specularCS.Get(), 64, 0.0f);
            double probeWorstOrientation = 0;
            for (UINT face = 0; face < 6; ++face) for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x) {
                const ProbeVector d = ProbeCubeDirection(face, (x + .5f) / 64 * 2 - 1, (y + .5f) / 64 * 2 - 1);
                const size_t t = ((size_t(face) * 64 + y) * 64 + x) * 4;
                for (UINT c = 0; c < 3; ++c) probeWorstOrientation = std::max(probeWorstOrientation, double(std::abs(mirrorOut[t + c] - ProbeSky(d, c))));
            }
            // Diffuse: the mean of the sky over 64 cosine-weighted directions per texel, which is E/pi (the consumer multiplies by albedo alone).
            const auto diffuseOut = runProbe(diffuseCS.Get(), 16, 0.0f);
            double probeWorstDiffuse = 0;
            for (UINT face = 0; face < 6; ++face) for (UINT y = 0; y < 16; ++y) for (UINT x = 0; x < 16; ++x) {
                const ProbeVector n = ProbeCubeDirection(face, (x + .5f) / 16 * 2 - 1, (y + .5f) / 16 * 2 - 1);
                ProbeVector tangent{}, bitangent{};
                ProbeBasis(n, tangent, bitangent);
                float sum[3] = {0, 0, 0};
                for (UINT i = 0; i < 64; ++i) {
                    const float xi = (float(i) + .5f) / 64, phi = 6.2831853f * ProbeRadicalInverse(i), r = std::sqrt(xi), z = std::sqrt(1 - xi);
                    const ProbeVector d = tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) + n * z;
                    for (UINT c = 0; c < 3; ++c) sum[c] += ProbeSky(d, c);
                }
                const size_t t = ((size_t(face) * 16 + y) * 16 + x) * 4;
                for (UINT c = 0; c < 3; ++c) probeWorstDiffuse = std::max(probeWorstDiffuse, double(std::abs(diffuseOut[t + c] - sum[c] / 64)));
            }
            // Glossy: the GGX-weighted mean at mip 3 (roughness 0.5, alpha 0.25), with the kernel's 64 half-vector samples per texel.
            const float glossyRoughness = 3.0f / 6.0f, glossyAlpha = glossyRoughness * glossyRoughness;
            const auto glossyOut = runProbe(specularCS.Get(), 8, glossyRoughness);
            double probeWorstGlossy = 0;
            for (UINT face = 0; face < 6; ++face) for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
                const ProbeVector n = ProbeCubeDirection(face, (x + .5f) / 8 * 2 - 1, (y + .5f) / 8 * 2 - 1);
                ProbeVector tangent{}, bitangent{};
                ProbeBasis(n, tangent, bitangent);
                float sum[3] = {0, 0, 0}, weightSum = 0;
                for (UINT i = 0; i < 64; ++i) {
                    const float phi = 6.2831853f * float(i) / 64, xi = ProbeRadicalInverse(i);
                    const float z = std::sqrt((1 - xi) / std::max(1 + (glossyAlpha * glossyAlpha - 1) * xi, .000001f));
                    const float r = std::sqrt(std::max(0.0f, 1 - z * z));
                    const ProbeVector h = tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) + n * z;
                    const ProbeVector l = ProbeNormalise(h * (2 * ProbeDot(n, h)) + n * -1.0f);
                    const float nl = std::clamp(ProbeDot(n, l), 0.0f, 1.0f);
                    if (nl > 0) {
                        for (UINT c = 0; c < 3; ++c) sum[c] += ProbeSky(l, c) * nl;
                        weightSum += nl;
                    }
                }
                const size_t t = ((size_t(face) * 8 + y) * 8 + x) * 4;
                for (UINT c = 0; c < 3; ++c) probeWorstGlossy = std::max(probeWorstGlossy, double(std::abs(glossyOut[t + c] - sum[c] / std::max(weightSum, .000001f))));
            }
            // Every probe output is a finite, non-negative radiance.
            bool probeValid = true;
            for (const auto *outputs : {&mirrorOut, &diffuseOut, &glossyOut}) for (float v : *outputs) probeValid = probeValid && std::isfinite(v) && v >= 0;
            std::printf("atmosphere IBL: worst orientation error %.5f, diffuse %.5f, glossy mip 3 %.5f\n", probeWorstOrientation, probeWorstDiffuse, probeWorstGlossy);
            Check(probeWorstOrientation < .005, "the raw sky is read back at each cube texel's own direction");
            Check(probeWorstDiffuse < .005, "the diffuse probe is the cosine-weighted mean of the sky (E/pi)");
            Check(probeWorstGlossy < .005, "the glossy probe is the GGX-weighted mean of the sky at mip 3");
            Check(probeValid, "the probe outputs are finite and non-negative");
        }

        // 6. Contact shadows (GTAO.hlsl ContactVisibility) on the production constants (GTAOGPU::PrepareConstants). A flat floor
        //    must cast none; a low wall must shade the floor on its sun side near its base and nothing far from it.
        {
            const UINT contactSide = 256;
            const DirectX::XMFLOAT3 contactEye{6.0f, 0.0f, 4.0f};
            const DirectX::XMMATRIX contactView = DirectX::XMMatrixLookAtLH(DirectX::XMLoadFloat3(&contactEye), DirectX::XMVectorSet(0, 0, .5f, 1), DirectX::XMVectorSet(0, 0, 1, 0));
            const DirectX::XMMATRIX contactProjection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, 1.0f, .1f, 100.0f);
            const DirectX::XMMATRIX contactViewProjection = DirectX::XMMatrixMultiply(contactView, contactProjection);
            const DirectX::XMMATRIX contactInverse = DirectX::XMMatrixInverse(nullptr, contactViewProjection);
            // The sun, toward it from the floor: normalize(-1, 0, .6), low on the wall's side.
            const float sunLength = std::sqrt(1.36f);
            GenZH::Modern::ScreenMaterialGPU::Camera contactCamera{};
            DirectX::XMFLOAT4X4 viewStore, projectionStore, inverseStore;
            DirectX::XMStoreFloat4x4(&viewStore, contactView);
            DirectX::XMStoreFloat4x4(&projectionStore, contactProjection);
            DirectX::XMStoreFloat4x4(&inverseStore, contactInverse);
            std::memcpy(contactCamera.viewMatrix, &viewStore, sizeof(viewStore));
            std::memcpy(contactCamera.projectionMatrix, &projectionStore, sizeof(projectionStore));
            std::memcpy(contactCamera.inverseVP, &inverseStore, sizeof(inverseStore));
            contactCamera.viewport[2] = float(contactSide);
            contactCamera.viewport[3] = float(contactSide);
            contactCamera.depthRange[1] = 1.0f;
            contactCamera.eye[0] = contactEye.x; contactCamera.eye[1] = contactEye.y; contactCamera.eye[2] = contactEye.z; contactCamera.eye[3] = 1.0f;
            contactCamera.sunDirection[0] = -1.0f / sunLength; contactCamera.sunDirection[1] = 0.0f; contactCamera.sunDirection[2] = .6f / sunLength; contactCamera.sunDirection[3] = 1.0f;
            GenZH::Modern::GTAOGPU::Constants contactConstants{};
            Check(GenZH::Modern::GTAOGPU::PrepareConstants(contactCamera, contactSide, contactSide, 8.0f, 1.0f, contactConstants), "GTAO constants accept the camera");
            contactConstants.contact[0] = 1.0f; // contact strength, as the renderer sets it
            // The visible surfaces per pixel, found analytically: a floor at z = 0 and, in the wall scene, a wall at x = 0 (|y| <= 20, 0 <= z <= 1).
            struct ContactScene {
                std::vector<float> depth, worldX;
                std::vector<unsigned short> normal;
                std::vector<int> kind;
            };
            auto buildScene = [&](bool withWall) {
                ContactScene built;
                built.depth.assign(size_t(contactSide) * contactSide, 1.0f);
                built.worldX.assign(size_t(contactSide) * contactSide, 0.0f);
                built.normal.assign(size_t(contactSide) * contactSide * 4, 0);
                built.kind.assign(size_t(contactSide) * contactSide, 0);
                const DirectX::XMVECTOR eyeVector = DirectX::XMLoadFloat3(&contactEye);
                for (UINT y = 0; y < contactSide; ++y) for (UINT x = 0; x < contactSide; ++x) {
                    const float ndcX = (x + .5f) / contactSide * 2 - 1, ndcY = 1 - (y + .5f) / contactSide * 2;
                    const DirectX::XMVECTOR nearPoint = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(ndcX, ndcY, 0, 1), contactInverse);
                    const DirectX::XMVECTOR farPoint = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(ndcX, ndcY, 1, 1), contactInverse);
                    const DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(farPoint, eyeVector));
                    const float dx = DirectX::XMVectorGetX(direction), dy = DirectX::XMVectorGetY(direction), dz = DirectX::XMVectorGetZ(direction);
                    float bestT = std::numeric_limits<float>::max();
                    int bestKind = 0;
                    if (dz < -1e-6f) {
                        const float t = -contactEye.z / dz, px = contactEye.x + t * dx, py = contactEye.y + t * dy;
                        if (t > 0 && std::abs(px) <= 40 && std::abs(py) <= 40) { bestT = t; bestKind = 1; }
                    }
                    if (withWall && dx < -1e-6f) {
                        const float t = -contactEye.x / dx, py = contactEye.y + t * dy, pz = contactEye.z + t * dz;
                        if (t > 0 && t < bestT && std::abs(py) <= 20 && pz >= 0 && pz <= 1) { bestT = t; bestKind = 2; }
                    }
                    if (bestKind == 0) continue;
                    const size_t index = size_t(y) * contactSide + x;
                    const DirectX::XMVECTOR hit = DirectX::XMVectorAdd(eyeVector, DirectX::XMVectorScale(direction, bestT));
                    built.depth[index] = DirectX::XMVectorGetZ(DirectX::XMVector3TransformCoord(hit, contactViewProjection));
                    built.worldX[index] = DirectX::XMVectorGetX(hit);
                    built.kind[index] = bestKind;
                    const DirectX::XMFLOAT3 surfaceNormal = bestKind == 1 ? DirectX::XMFLOAT3{0.0f, 0.0f, 1.0f} : DirectX::XMFLOAT3{1.0f, 0.0f, 0.0f};
                    const DirectX::PackedVector::HALF packed[4] = {DirectX::PackedVector::XMConvertFloatToHalf(surfaceNormal.x), DirectX::PackedVector::XMConvertFloatToHalf(surfaceNormal.y), DirectX::PackedVector::XMConvertFloatToHalf(surfaceNormal.z), DirectX::PackedVector::XMConvertFloatToHalf(1.0f)};
                    for (UINT c = 0; c < 4; ++c) built.normal[index * 4 + c] = packed[c];
                }
                return built;
            };
            // ContactVisibility per pixel (-1 where the GTAO path has no valid surface), read back as floats.
            const std::string contactEntry = R"(
RWTexture2D<float> ContactProbe : register(u3);
[numthreads(8,8,1)]
void CSContactProbe(uint2 pixel:SV_DispatchThreadID) {
 if(any(pixel>=uint2(AO.ViewportSize)))return;
 float3 world=0;
 if(!ReconstructVisible(int2(pixel),world)){ContactProbe[pixel]=-1;return;}
 float3 normal=normalize(MappedNormalAO.Load(int3(pixel,0)).xyz);
 ContactProbe[pixel]=ContactVisibility(world,normal);
}
)";
            const auto contactBlob = Compile(std::string(GenZH::Modern::GTAOShaderSource) + contactEntry, "CSContactProbe", "cs_5_0");
            ComPtr<ID3D11ComputeShader> contactShader;
            HR(device->CreateComputeShader(contactBlob->GetBufferPointer(), contactBlob->GetBufferSize(), nullptr, contactShader.GetAddressOf()), "contact probe shader");
            D3D11_BUFFER_DESC contactDesc{};
            contactDesc.ByteWidth = sizeof(GenZH::Modern::GTAOGPU::Constants);
            contactDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA contactInit{&contactConstants, 0, 0};
            ComPtr<ID3D11Buffer> contactBuffer;
            HR(device->CreateBuffer(&contactDesc, &contactInit, contactBuffer.GetAddressOf()), "contact constants");
            auto probeContact = [&](const ContactScene &contactScene) {
                ComPtr<ID3D11ShaderResourceView> depthView, normalView;
                Texture(device.Get(), contactSide, contactSide, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, contactScene.depth.data(), contactSide * 4, &depthView);
                Texture(device.Get(), contactSide, contactSide, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, contactScene.normal.data(), contactSide * 8, &normalView);
                ComPtr<ID3D11Texture2D> probeTarget = Texture(device.Get(), contactSide, contactSide, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, nullptr);
                ComPtr<ID3D11UnorderedAccessView> probeView;
                HR(device->CreateUnorderedAccessView(probeTarget.Get(), nullptr, probeView.GetAddressOf()), "contact probe view");
                ID3D11Buffer *constantSlot = contactBuffer.Get();
                ID3D11ShaderResourceView *depthSlot = depthView.Get(), *normalSlot = normalView.Get();
                ID3D11UnorderedAccessView *probeUAV = probeView.Get();
                context->CSSetShader(contactShader.Get(), nullptr, 0);
                context->CSSetConstantBuffers(13, 1, &constantSlot);
                context->CSSetShaderResources(37, 1, &depthSlot);
                context->CSSetShaderResources(41, 1, &normalSlot);
                context->CSSetUnorderedAccessViews(3, 1, &probeUAV, nullptr);
                context->Dispatch(contactSide / 8, contactSide / 8, 1);
                ID3D11UnorderedAccessView *noUAV = nullptr; context->CSSetUnorderedAccessViews(3, 1, &noUAV, nullptr);
                ID3D11ShaderResourceView *noSRVs[2]{}; context->CSSetShaderResources(37, 2, noSRVs);
                context->CSSetShader(nullptr, nullptr, 0);
                D3D11_TEXTURE2D_DESC readDesc{}; probeTarget->GetDesc(&readDesc);
                readDesc.Usage = D3D11_USAGE_STAGING; readDesc.BindFlags = 0; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> contactStaging; HR(device->CreateTexture2D(&readDesc, nullptr, contactStaging.GetAddressOf()), "contact staging");
                context->CopyResource(contactStaging.Get(), probeTarget.Get());
                D3D11_MAPPED_SUBRESOURCE contactMapped{}; HR(context->Map(contactStaging.Get(), 0, D3D11_MAP_READ, 0, &contactMapped), "contact readback");
                std::vector<float> visibility(size_t(contactSide) * contactSide);
                for (UINT y = 0; y < contactSide; ++y) std::memcpy(&visibility[size_t(y) * contactSide], static_cast<const unsigned char *>(contactMapped.pData) + y * contactMapped.RowPitch, contactSide * 4);
                context->Unmap(contactStaging.Get(), 0);
                return visibility;
            };
            const auto flatScene = buildScene(false), wallScene = buildScene(true);
            const auto flatVisibility = probeContact(flatScene), wallVisibility = probeContact(wallScene);
            double flatMin = 1, nearMin = 1, farMin = 1;
            size_t flatCount = 0, contactNearCount = 0, contactFarCount = 0;
            for (size_t i = 0; i < flatScene.kind.size(); ++i) if (flatScene.kind[i] == 1) {
                ++flatCount;
                flatMin = std::min(flatMin, double(flatVisibility[i]));
            }
            for (size_t i = 0; i < wallScene.kind.size(); ++i) if (wallScene.kind[i] == 1) {
                const float floorX = wallScene.worldX[i];
                if (floorX > .05f && floorX < .6f) { ++contactNearCount; nearMin = std::min(nearMin, double(wallVisibility[i])); }
                if (floorX > 2.2f) { ++contactFarCount; farMin = std::min(farMin, double(wallVisibility[i])); }
            }
            std::printf("contact shadows: flat floor min %.5f (%zu px), near the wall min %.4f (%zu px), far from it min %.5f (%zu px)\n",
                        flatMin, flatCount, nearMin, contactNearCount, farMin, contactFarCount);
            Check(flatCount > 1000 && flatMin > .999, "a flat floor casts no contact shadow of its own");
            Check(contactNearCount > 20 && nearMin < .5, "a low wall shades the floor on its sun side near its base");
            Check(contactFarCount > 200 && farMin > .999, "the floor far from the wall stays lit");
        }

        // 7. The hex stochastic tiling of the terrain library (LegacyScene.hlsl TerrainLibraryHex). The near layer (angle 0,
        //    stretch 1) and the far layer (angle .65, stretch 2.7) must both keep the layer's mean colour and must not be the
        //    same pattern. A synthetic 64^2 layer with a known mean is tiled at 0.5 world units per texel.
        {
            const UINT hexSide = 256;
            const std::string hexEntry = R"(
RWTexture2D<float4> HexNear : register(u4);
RWTexture2D<float4> HexFar : register(u5);
RWTexture2D<float4> HexPartner : register(u6);
[numthreads(8,8,1)]
void CSHexProbe(uint2 pixel:SV_DispatchThreadID) {
 if(any(pixel>=256))return;
 float2 world=(float2(pixel)+.5)*.5;
 float2 dx=float2(.5,0),dy=float2(0,.5);
 HexNear[pixel]=TerrainLibraryHex(0,world,dx,dy,0,1,false).color;
 HexFar[pixel]=TerrainLibraryHex(0,world,dx,dy,.65,2.7,false).color;
 HexPartner[pixel]=TerrainLibraryHex(0,world,dx,dy,1.9,1,true).color;
}
)";
            // The renderer's program generator (ProgramSource.h BuildProgramSource) fills the legacy source's markers. The probe
            // never reaches the legacy pixel path, so the texture slots are stubs and the programs are the fixed-function defaults.
            std::string hexTextures;
            for (UINT i = 0; i < 8; ++i) {
                const std::string slot = std::to_string(i);
                hexTextures += "Texture2D Texture" + slot + ":register(t" + slot + ");SamplerState Sampler" + slot + ":register(s" + slot + ");\n";
                hexTextures += "float4 Sample" + slot + "(float4 coord){return 1;}\n";
            }
            hexTextures += "float4 SampleStage(int index,float4 coord){return coord;}\n";
            std::string hexSource = GenZH::Modern::LegacyShaderSource;
            const auto replaceMarker = [&hexSource](const char *marker, const std::string &value) { hexSource.replace(hexSource.find(marker), std::strlen(marker), value); };
            replaceMarker("/*TEXTURES*/", hexTextures);
            replaceMarker("/*PIXEL_PROGRAM*/", "float4 LegacyPixel(float4 diffuse,float4 specular,float4 coords[8]){return FixedPixel(diffuse,specular,coords);}");
            replaceMarker("/*VERTEX_PROGRAM*/", "LegacyOutput LegacyVS(LegacyInput input){return FixedVS(input);}");
            const auto hexBlob = Compile(hexSource + hexEntry, "CSHexProbe", "cs_5_0");
            ComPtr<ID3D11ComputeShader> hexShader;
            HR(device->CreateComputeShader(hexBlob->GetBufferPointer(), hexBlob->GetBufferSize(), nullptr, hexShader.GetAddressOf()), "hex probe shader");
            // The layer: 64^2 RGBA32F, two identical slices (the library is a Texture2DArray). Colour is the mean plus
            // independent noise; the height in alpha is independent of the colour, so the hex selection does not bias it.
            const UINT layerSide = 64;
            const float layerTarget[3] = {.30f, .42f, .18f};
            auto hashUnit = [](UINT x, UINT y, UINT salt) {
                UINT h = x * 73856093u ^ y * 19349663u ^ salt * 83492791u;
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                return float(h & 0xffffffu) / 16777216.0f;
            };
            std::vector<float> layerTexels(size_t(layerSide) * layerSide * 2 * 4);
            for (UINT slice = 0; slice < 2; ++slice) for (UINT y = 0; y < layerSide; ++y) for (UINT x = 0; x < layerSide; ++x) {
                float *texel = &layerTexels[((size_t(slice) * layerSide + y) * layerSide + x) * 4];
                for (UINT c = 0; c < 3; ++c) texel[c] = layerTarget[c] + .2f * (hashUnit(x, y, c + 1) - .5f);
                texel[3] = hashUnit(x, y, 7);
            }
            double layerMean[3] = {0, 0, 0};
            for (UINT y = 0; y < layerSide; ++y) for (UINT x = 0; x < layerSide; ++x) for (UINT c = 0; c < 3; ++c)
                layerMean[c] += layerTexels[(size_t(y) * layerSide + x) * 4 + c] / (double(layerSide) * layerSide);
            D3D11_TEXTURE2D_DESC layerDesc{};
            layerDesc.Width = layerDesc.Height = layerSide; layerDesc.MipLevels = 1; layerDesc.ArraySize = 2; layerDesc.SampleDesc.Count = 1;
            layerDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; layerDesc.Usage = D3D11_USAGE_IMMUTABLE; layerDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA layerInit[2]{};
            layerInit[0] = {layerTexels.data(), layerSide * 16, 0};
            layerInit[1] = {layerTexels.data() + size_t(layerSide) * layerSide * 4, layerSide * 16, 0};
            ComPtr<ID3D11Texture2D> layerTexture;
            HR(device->CreateTexture2D(&layerDesc, layerInit, layerTexture.GetAddressOf()), "hex layer");
            ComPtr<ID3D11ShaderResourceView> layerView;
            HR(device->CreateShaderResourceView(layerTexture.Get(), nullptr, layerView.GetAddressOf()), "hex layer view");
            // TerrainLibraryTable: layer 0's tile size in world units (.x).
            std::vector<float> tableFloats(64 * 4, 0.0f);
            tableFloats[0] = 4.0f;
            D3D11_BUFFER_DESC tableDesc{};
            tableDesc.ByteWidth = UINT(tableFloats.size() * 4); tableDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            tableDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; tableDesc.StructureByteStride = 16;
            D3D11_SUBRESOURCE_DATA tableInit{tableFloats.data(), 0, 0};
            ComPtr<ID3D11Buffer> tableBuffer;
            HR(device->CreateBuffer(&tableDesc, &tableInit, tableBuffer.GetAddressOf()), "hex table");
            ComPtr<ID3D11ShaderResourceView> tableView;
            HR(device->CreateShaderResourceView(tableBuffer.Get(), nullptr, tableView.GetAddressOf()), "hex table view");
            // The terrain sampler: linear, wrapping, as the renderer creates it.
            D3D11_SAMPLER_DESC wrapDesc{};
            wrapDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; wrapDesc.AddressU = wrapDesc.AddressV = wrapDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; wrapDesc.MaxLOD = D3D11_FLOAT32_MAX;
            ComPtr<ID3D11SamplerState> wrapSampler;
            HR(device->CreateSamplerState(&wrapDesc, wrapSampler.GetAddressOf()), "hex sampler");
            ComPtr<ID3D11Texture2D> nearTarget = Texture(device.Get(), hexSide, hexSide, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, nullptr);
            ComPtr<ID3D11Texture2D> farTarget = Texture(device.Get(), hexSide, hexSide, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, nullptr);
            ComPtr<ID3D11Texture2D> partnerTarget = Texture(device.Get(), hexSide, hexSide, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, nullptr);
            ComPtr<ID3D11UnorderedAccessView> nearView, farView, partnerView;
            HR(device->CreateUnorderedAccessView(nearTarget.Get(), nullptr, nearView.GetAddressOf()), "hex near view");
            HR(device->CreateUnorderedAccessView(farTarget.Get(), nullptr, farView.GetAddressOf()), "hex far view");
            HR(device->CreateUnorderedAccessView(partnerTarget.Get(), nullptr, partnerView.GetAddressOf()), "hex partner view");
            ID3D11UnorderedAccessView *hexUAVs[3] = {nearView.Get(), farView.Get(), partnerView.Get()};
            ID3D11ShaderResourceView *hexSRVs[3] = {layerView.Get(), layerView.Get(), tableView.Get()};
            ID3D11SamplerState *hexSampler = wrapSampler.Get();
            context->CSSetShader(hexShader.Get(), nullptr, 0);
            context->CSSetShaderResources(48, 3, hexSRVs);
            context->CSSetSamplers(14, 1, &hexSampler);
            context->CSSetUnorderedAccessViews(4, 3, hexUAVs, nullptr);
            context->Dispatch(hexSide / 8, hexSide / 8, 1);
            ID3D11UnorderedAccessView *noUAVs[3]{};
            context->CSSetUnorderedAccessViews(4, 3, noUAVs, nullptr);
            ID3D11ShaderResourceView *noSRVs[3]{};
            context->CSSetShaderResources(48, 3, noSRVs);
            context->CSSetShader(nullptr, nullptr, 0);
            auto readFloats = [&](ID3D11Texture2D *source) {
                D3D11_TEXTURE2D_DESC readDesc{};
                source->GetDesc(&readDesc);
                readDesc.Usage = D3D11_USAGE_STAGING; readDesc.BindFlags = 0; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                HR(device->CreateTexture2D(&readDesc, nullptr, staging.GetAddressOf()), "hex staging");
                context->CopyResource(staging.Get(), source);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "hex readback");
                std::vector<float> out(size_t(hexSide) * hexSide * 4);
                for (UINT y = 0; y < hexSide; ++y) std::memcpy(&out[size_t(y) * hexSide * 4], static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch, hexSide * 16);
                context->Unmap(staging.Get(), 0);
                return out;
            };
            const auto nearOut = readFloats(nearTarget.Get()), farOut = readFloats(farTarget.Get()), partnerOut = readFloats(partnerTarget.Get());
            const size_t hexTexels = size_t(hexSide) * hexSide;
            auto luminance = [](const float *rgb) { return .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2]; };
            auto lumaAt = [&](const std::vector<float> &out, size_t i) { return luminance(&out[i * 4]); };
            auto lumaMean = [&](const std::vector<float> &out) {
                double sum = 0;
                for (size_t i = 0; i < hexTexels; ++i) sum += lumaAt(out, i);
                return sum / double(hexTexels);
            };
            // The worst channel's mean error against the layer's mean colour.
            auto meanError = [&](const std::vector<float> &out) {
                double sums[3] = {0, 0, 0}, worst = 0;
                for (size_t i = 0; i < hexTexels; ++i) for (UINT c = 0; c < 3; ++c) sums[c] += out[i * 4 + c];
                for (UINT c = 0; c < 3; ++c) worst = std::max(worst, std::abs(sums[c] / double(hexTexels) - layerMean[c]));
                return worst;
            };
            // The luminance standard deviation, and the luminance correlation with the near layer.
            auto spread = [&](const std::vector<float> &out) {
                const double center = lumaMean(out);
                double variance = 0;
                for (size_t i = 0; i < hexTexels; ++i) variance += (lumaAt(out, i) - center) * (lumaAt(out, i) - center);
                return std::sqrt(variance / double(hexTexels));
            };
            auto correlation = [&](const std::vector<float> &out) {
                const double nearCenter = lumaMean(nearOut), outCenter = lumaMean(out);
                double covariance = 0, nearVariance = 0, outVariance = 0;
                for (size_t i = 0; i < hexTexels; ++i) {
                    const double a = lumaAt(nearOut, i) - nearCenter, b = lumaAt(out, i) - outCenter;
                    covariance += a * b;
                    nearVariance += a * a;
                    outVariance += b * b;
                }
                return covariance / std::sqrt(nearVariance * outVariance);
            };
            const double nearMeanError = meanError(nearOut), farMeanError = meanError(farOut), partnerMeanError = meanError(partnerOut);
            const double farCorrelation = correlation(farOut), partnerCorrelation = correlation(partnerOut);
            const double farSpread = spread(farOut), partnerSpread = spread(partnerOut);
            std::printf("hex layers: mean error near %.5f, far %.5f, partner %.5f; luminance correlation with near far %.4f, partner %.4f; spread far %.4f, partner %.4f\n",
                        nearMeanError, farMeanError, partnerMeanError, farCorrelation, partnerCorrelation, farSpread, partnerSpread);
            Check(nearMeanError < .02, "the near hex layer keeps the layer's mean colour");
            Check(farMeanError < .02, "the far hex layer keeps the layer's mean colour");
            Check(partnerMeanError < .02, "the partner hex layer keeps the layer's mean colour");
            Check(std::abs(farCorrelation) < .5 && farSpread > .01, "the far hex layer is a different, non-flat pattern from the near layer");
            Check(std::abs(partnerCorrelation) < .5 && partnerSpread > .01, "the partner hex layer is a different, non-flat pattern from the near layer");
        }

        // 8. The photoreal decal stain (LegacyScene.hlsl DecalStain): untouched ground at zero coverage, the stain colour at full
        //    coverage scaled by the relief, and darkening that only grows with coverage. Probed on the legacy source the renderer compiles.
        {
            const std::string decalEntry = R"(
RWTexture2D<float4> DecalProbe : register(u7);
[numthreads(8,8,1)]
void CSDecalProbe(uint2 pixel:SV_DispatchThreadID) {
 float cover=pixel.x/255.0;
 float relief=.7+.6*pixel.y/255.0;
 DecalProbe[pixel]=float4(DecalStain(float3(.5,.4,.3),cover,relief),1);
}
)";
            const auto decalBlob = Compile(PreparedLegacySource() + decalEntry, "CSDecalProbe", "cs_5_0");
            ComPtr<ID3D11ComputeShader> decalShader;
            HR(device->CreateComputeShader(decalBlob->GetBufferPointer(), decalBlob->GetBufferSize(), nullptr, decalShader.GetAddressOf()), "decal probe shader");
            ComPtr<ID3D11Texture2D> decalTarget = Texture(device.Get(), 256, 256, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, nullptr);
            ComPtr<ID3D11UnorderedAccessView> decalView;
            HR(device->CreateUnorderedAccessView(decalTarget.Get(), nullptr, decalView.GetAddressOf()), "decal probe view");
            ID3D11UnorderedAccessView *decalUAV = decalView.Get();
            context->CSSetShader(decalShader.Get(), nullptr, 0);
            context->CSSetUnorderedAccessViews(7, 1, &decalUAV, nullptr);
            context->Dispatch(32, 32, 1);
            ID3D11UnorderedAccessView *noDecalUAV = nullptr;
            context->CSSetUnorderedAccessViews(7, 1, &noDecalUAV, nullptr);
            context->CSSetShader(nullptr, nullptr, 0);
            D3D11_TEXTURE2D_DESC decalRead{};
            decalTarget->GetDesc(&decalRead);
            decalRead.Usage = D3D11_USAGE_STAGING; decalRead.BindFlags = 0; decalRead.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> decalStaging;
            HR(device->CreateTexture2D(&decalRead, nullptr, decalStaging.GetAddressOf()), "decal staging");
            context->CopyResource(decalStaging.Get(), decalTarget.Get());
            D3D11_MAPPED_SUBRESOURCE decalMapped{};
            HR(context->Map(decalStaging.Get(), 0, D3D11_MAP_READ, 0, &decalMapped), "decal readback");
            std::vector<float> decalOut(256 * 256 * 4);
            for (UINT y = 0; y < 256; ++y) std::memcpy(&decalOut[size_t(y) * 256 * 4], static_cast<const unsigned char *>(decalMapped.pData) + y * decalMapped.RowPitch, 256 * 16);
            context->Unmap(decalStaging.Get(), 0);
            // CPU reference: the native colour decoded from sRGB and darkened to the stain (x0.72), scaled by the relief of its row.
            auto srgbToLinear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
            const double stainRef[3] = {srgbToLinear(.5) * .72, srgbToLinear(.4) * .72, srgbToLinear(.3) * .72};
            double worstUntouched = 0, worstStain = 0, worstRise = -1;
            bool stainFinite = true;
            for (UINT y = 0; y < 256; ++y) {
                const double relief = .7 + .6 * y / 255.0;
                double previous = 2;
                for (UINT x = 0; x < 256; ++x) {
                    const float *texel = &decalOut[(size_t(y) * 256 + x) * 4];
                    for (UINT c = 0; c < 3; ++c) stainFinite = stainFinite && std::isfinite(texel[c]);
                    if (x == 0) for (UINT c = 0; c < 3; ++c) worstUntouched = std::max(worstUntouched, std::abs(double(texel[c]) - 1.0));
                    if (x == 255) for (UINT c = 0; c < 3; ++c) worstStain = std::max(worstStain, std::abs(texel[c] - stainRef[c] * relief));
                    worstRise = std::max(worstRise, double(texel[0]) - previous);
                    previous = texel[0];
                }
            }
            std::printf("decal stain: untouched ground error %.6f, full-coverage stain error %.5f, largest rise with coverage %.6f\n",
                        worstUntouched, worstStain, worstRise);
            Check(stainFinite, "the decal stain is finite");
            Check(worstUntouched < 1e-4, "zero coverage leaves the ground untouched");
            Check(worstStain < 2e-3, "full coverage darkens the ground to the stain colour, scaled by the relief");
            Check(worstRise < 1e-5, "the stain only darkens as the coverage grows");
        }

        std::printf("{\"passed\":true,\"checks\":%u,\"near_red_bias\":%.5f,\"far_red_bias\":%.5f,\"near_pixels\":%zu,\"far_pixels\":%zu}\n",
                    checks, nearRedBias, farRedBias, nearCount, farCount);
        return 0;
    } catch (const std::exception &e) {
        std::printf("{\"passed\":false,\"checks\":%u,\"error\":\"%s\"}\n", checks, e.what());
        return 1;
    }
}
