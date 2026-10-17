#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;
constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 32;
constexpr std::uint32_t Slices = 6;
constexpr std::uint32_t Overlaps = 32;
constexpr std::uint32_t LayerExport = (1u << 18u) | (1u << 21u);
constexpr std::size_t SliceBytes = 65536;
alignas(256) std::array<std::uint32_t, Width * Height * Slices> Results{};

alignas(256) constexpr std::array<std::uint32_t, 14> VertexCode{
    0xe0382000u, 0x80000005u, 0xe0382010u, 0x80000805u, 0xe0302020u, 0x80000405u, 0xbf8c3f70u, 0xf80000cfu,
    0x03020100u, 0xf80008d4u, 0x00040000u, 0xf800020fu, 0x0b0a0908u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 22> PixelCode{
    0x7e060f00u, 0x7e080f01u, 0x2c0a0490u, 0x34080886u, 0x340a0a8bu, 0x4a060903u, 0x4a060b03u, 0x34060682u,
    0xc8120002u, 0xe0305000u, 0x80000803u, 0xbf8c3f70u, 0xd5690008u, 0x02014308u, 0x3a100908u, 0xe0705000u,
    0x80000803u, 0xbf8c3f70u, 0x7e1802f2u, 0xf800180fu, 0x0c0c0c0cu, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 32> BlendCode{
    0x7e060f00u, 0x7e080f01u, 0x2c0a0490u, 0x34080886u, 0x340a0a8bu, 0x4a060903u, 0x4a060b03u, 0x34060682u,
    0xc8120002u, 0xc8160102u, 0xc81a0202u, 0xc81e0302u, 0xe95a5000u, 0x80000803u, 0xbf8c3f70u, 0x08160ef2u,
    0x10080907u, 0x100a0b07u, 0x100c0d07u, 0x1010110bu, 0x1012130bu, 0x1014150bu, 0x06101104u, 0x06121305u,
    0x06141506u, 0xe95e5000u, 0x80000803u, 0xbf8c3f70u, 0x7e1802f2u, 0xf800180fu, 0x0c0c0c0cu, 0xbf810000u,
};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> token;
    std::uint32_t layer;
};

class Surface {
public:
    Surface() {
#ifdef _WIN32
        data = static_cast<std::byte*>(VirtualAlloc(nullptr, SliceBytes * Slices, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        data = static_cast<std::byte*>(std::aligned_alloc(SliceBytes, SliceBytes * Slices));
#endif
        Require(data != nullptr, "layered interlock: cannot allocate the color surface");
        std::memset(data, 0, SliceBytes * Slices);
        GuestAllocations::Mutation().Add(data, SliceBytes * Slices, true, true, true);
    }
    ~Surface() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        VirtualFree(data, 0, MEM_RELEASE);
#else
        std::free(data);
#endif
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
private:
    std::byte* data = nullptr;
};

std::uint32_t Token(std::uint32_t primitive, std::uint32_t layer, std::uint32_t draw) {
    return 0x3f000000u | ((0x45d9f3bu * (primitive + 1u + layer * Overlaps + draw * Slices * Overlaps)) & 0x7fffffu);
}

std::array<float, 4> BlendSource(std::uint32_t primitive, std::uint32_t layer, std::uint32_t draw) {
    if (primitive == 0u) return {70000.0f, 70000.0f, 70000.0f, 1.0f};
    if (primitive == 1u) return {0, 0, 0, 0};
    if (primitive == 2u) return {-65504.0f, -65504.0f, -65504.0f, 1.0f};
    const auto scale = static_cast<float>((primitive + 1u) * (layer + 1u));
    if (primitive == 3u) return {std::ldexp(scale, -20), std::ldexp(scale, -19), std::ldexp(scale, -18), 1.0f};
    return {scale * 0.03125f, scale * 0.046875f, scale * 0.0703125f, 0.125f * static_cast<float>(1u + 2u * ((primitive + draw) % 4u))};
}

std::uint32_t SmallFloat(float value, std::uint32_t mantissaBits) {
    const auto infinity = 31u << mantissaBits;
    if (std::isnan(value)) return infinity | 1u;
    if (!(value > 0)) return 0;
    if (std::isinf(value)) return infinity;
    const double scale = std::ldexp(1.0, mantissaBits);
    if (value < std::ldexp(1.0, -14)) return static_cast<std::uint32_t>(std::floor(value * std::ldexp(1.0, 14) * scale));
    int exponent = 0;
    const double fraction = std::frexp(static_cast<double>(value), &exponent);
    if (exponent > 16) return infinity - 1u;
    return (static_cast<std::uint32_t>(exponent + 14) << mantissaBits) | static_cast<std::uint32_t>(std::floor((fraction * 2.0 - 1.0) * scale));
}

float Unpack(std::uint32_t field, std::uint32_t mantissaBits) {
    const auto exponent = field >> mantissaBits;
    Require(exponent < 31u, "layered blend model produced an infinity or NaN");
    const auto mantissa = field & ((1u << mantissaBits) - 1u);
    return exponent == 0u ? std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissaBits)) : std::ldexp(1.0f + std::ldexp(static_cast<float>(mantissa), -static_cast<int>(mantissaBits)), static_cast<int>(exponent) - 15);
}

std::uint32_t Blend(std::uint32_t word, const std::array<float, 4>& source) {
    const std::array<std::uint32_t, 3> widths{5, 6, 6};
    const std::array<std::uint32_t, 3> offsets{0, 10, 21};
    std::uint32_t result = 0;
    for (std::uint32_t channel = 0; channel < widths.size(); ++channel) {
        const auto field = (word >> offsets[channel]) & ((1u << (widths[channel] + 5u)) - 1u);
        const auto destination = Unpack(field, widths[channel]);
        const volatile float left = source[channel] * source[3];
        const volatile float right = destination * (1.0f - source[3]);
        result |= SmallFloat(left + right, widths[channel]) << offsets[channel];
    }
    return result;
}

std::array<std::uint32_t, 4> Descriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, stride == 0u ? 0x31016facu : 0x01016facu};
}

void Draw(AgcDriver::VulkanDevice& device, const Surface& surface, std::uint32_t wave, std::uint32_t drawIndex, bool reverse, bool blend, std::uint32_t overlaps) {
    const std::array<std::array<float, 4>, 3> corners{{{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}}};
    std::vector<Vertex> vertices;
    for (std::uint32_t step = 0; step < overlaps; ++step) {
        const auto primitive = reverse ? overlaps - 1u - step : step;
        for (std::uint32_t layer = 0; layer < Slices; ++layer) {
            const auto token = std::bit_cast<float>(Token(primitive, layer, drawIndex));
            const auto source = blend ? BlendSource(primitive, layer, drawIndex) : std::array<float, 4>{token, token, token, token};
            for (const auto& position : corners) vertices.push_back({position, source, layer});
        }
    }
    const auto target = device.Target();
    const auto vertexData = Descriptor(vertices.data(), sizeof(Vertex), vertices.size());
    ShaderRecompiler::ShaderVertexStageInfo vertexInfo{};
    vertexInfo.paClVsOutCntl = LayerExport;
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {wave, 0, vertexData, std::nullopt, std::nullopt, vertexInfo, vertexMemory},
        target, {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = wave == 32u;
    pixel.interpolatorCount = 1;
    pixel.interpolatorSettings[0] = 0x400u;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::Ancillary);
    pixel.posX = pixel.posY = pixel.ancillary = pixel.layerExported = true;
    pixel.orderedPixelShader = true;
    pixel.executeOnNoop = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const auto pixelData = Descriptor(Results.data(), 0, sizeof(Results));
    const auto code = blend ? std::span<const std::uint32_t>(BlendCode) : std::span<const std::uint32_t>(PixelCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {wave, 0, pixelData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target, PixelPushLayout(vertexPush, target)
    };
    fragment.context.floatMode = ShaderRecompiler::ShaderFloatMode{.ieeeMode = true};
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, wave, wave, std::nullopt, std::nullopt};
    state.color = {surface.Address(), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, SliceBytes, 0xe4u};
    state.color.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    state.color.surfaceAddress = state.color.arrayAddress = surface.Address();
    state.color.surfaceExtent = {Width, Height};
    state.color.arrayLastSlice = Slices - 1u;
    state.color.layers = Slices;
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.renderLayers = Slices;
    state.layerExports = LayerExport;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(vertices.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Check(std::uint32_t wave, std::uint32_t draws, bool reverse, bool blend, std::uint32_t overlaps) {
    for (std::uint32_t layer = 0; layer < Slices; ++layer) {
        for (std::uint32_t pixel = 0; pixel < Width * Height; ++pixel) {
            const auto index = layer * Width * Height + pixel;
            auto expected = blend ? 0x780f01e0u : 0x13579bdfu + index;
            for (std::uint32_t draw = 0; draw < draws; ++draw) {
                for (std::uint32_t step = 0; step < overlaps; ++step) {
                    const auto primitive = reverse ? overlaps - 1u - step : step;
                    expected = blend ? Blend(expected, BlendSource(primitive, layer, draw)) : (expected * 33u) ^ Token(primitive, layer, draw);
                }
            }
            Require(Results[index] == expected, "layered interlock " + std::string(blend ? "blend" : "integer") + " overlaps " + std::to_string(overlaps) + " wave" + std::to_string(wave) + " draw " + std::to_string(draws) + " layer " + std::to_string(layer) + " pixel " + std::to_string(pixel) + ": primitive order or addressing mismatch: " + std::to_string(Results[index]) + " != " + std::to_string(expected));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!TargetHasCapability(device->Target(), spv::CapabilityFragmentShaderPixelInterlockEXT) || !TargetHasCapability(device->Target(), spv::CapabilityShaderViewportIndexLayerEXT)) {
            std::puts("skipped, the device lacks pixel interlock or vertex layer export");
            return VulkanTestSkipped;
        }
        Surface surface;
        for (const auto wave : {64u, 32u}) {
            for (const bool reverse : {false, true}) {
                for (const bool blend : {false, true}) {
                    for (const auto overlaps : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, Overlaps}) {
                        for (std::uint32_t i = 0; i < Results.size(); ++i) Results[i] = blend ? 0x780f01e0u : 0x13579bdfu + i;
                        for (std::uint32_t draw = 0; draw < 2; ++draw) {
                            Draw(*device, surface, wave, draw, reverse, blend, overlaps);
                            Check(wave, draw + 1u, reverse, blend, overlaps);
                        }
                    }
                }
            }
        }
        std::puts("layered pixel interlock tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
