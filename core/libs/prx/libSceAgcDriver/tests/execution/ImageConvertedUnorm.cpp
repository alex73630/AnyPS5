#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
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

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t LoadWidth = 2048;
constexpr std::uint32_t LoadGroups = LoadWidth / Threads;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t UnormFormat = 30;
constexpr std::uint32_t UintFormat = 34;
constexpr std::uint32_t FloatFormat = 43;
constexpr std::size_t FloatOffset = 40960;
constexpr std::uint32_t SwizzleXYZ1 = 0x3acu;
constexpr std::uint32_t SwizzleXZY1 = 0x374u;
constexpr std::uint32_t SwizzleZYX1 = 0x32eu;
constexpr std::uint32_t SwizzleX011 = 0x244u;
constexpr std::uint32_t SwizzleYXZ1 = 0x3a5u;
constexpr std::uint32_t SwizzleXYZW = 0xfacu;
constexpr std::uint32_t D16 = 0x80000000u;
constexpr std::uint32_t DivisionUlps = 3;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t RoundTripOffset = 16384;
constexpr std::size_t StoreOffset = 32768;
constexpr std::array<std::uint32_t, 4> PointSampler{0x00000092u, 0x00fff000u, 0u, 0u};
constexpr std::array<std::uint32_t, 4> BilinearSampler{0x00000092u, 0x00fff000u, 0x00500000u, 0u};
constexpr std::array<std::uint32_t, 4> WrapBilinearSampler{0x00000090u, 0x00fff000u, 0x00500000u, 0u};
constexpr std::array<std::uint32_t, 4> WrapPointSampler{0x00000090u, 0x00fff000u, 0u, 0u};
constexpr std::array<std::uint32_t, 4> BorderSampler{0x000000b6u, 0x00fff000u, 0u, 0u};
constexpr std::uint32_t SampleStep = 0x3a200000u;
constexpr std::uint32_t OffGridStep = 0x3a206000u;
constexpr std::uint32_t SampleStart = 0x39c00000u;
constexpr std::uint32_t WrappedStart = 0xbac00000u;
constexpr std::uint32_t FloatStart = 0x3c000000u;
constexpr double SampleTolerance = 1.0e-5;

alignas(256) std::array<std::uint32_t, LoadWidth * 4> Output{};
alignas(256) std::array<std::uint32_t, Threads * 4> Input{};

constexpr std::array<std::uint32_t, 15> LoadCode(std::uint32_t dmask, std::uint32_t d16) {
    return {
        0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x34063c84u, 0x7e3e0280u, 0x7e140280u, 0x7e160280u, 0x7e180280u,
        0x7e1a0280u, 0xf0001008u | (dmask << 8u), 0x00010a1eu | d16, 0xbf8c3f70u, 0xe0781000u, 0x80000a03u, 0xbf810000u,
    };
}

constexpr std::array<std::uint32_t, 9> StoreCode(std::uint32_t dmask, std::uint32_t d16) {
    return {
        0x34060084u, 0xe0381000u, 0x80030a03u, 0x7e3c0300u, 0x7e3e0280u, 0xbf8c3f70u, 0xf0201008u | (dmask << 8u), 0x00010a1eu | d16,
        0xbf810000u,
    };
}

constexpr std::array<std::uint32_t, 9> SamplerCode(std::uint32_t word0) {
    return {
        0x34060084u, 0x7e280280u, 0x7e2a0280u, word0, 0x00610a14u, 0xbf8c3f70u, 0xe0781000u, 0x80000a03u,
        0xbf810000u,
    };
}

constexpr std::array<std::uint32_t, 13> SampleAtCode(std::uint32_t step, std::uint32_t start) {
    return {
        0x34060084u, 0x7e280d00u, 0x102828ffu, step, 0x062828ffu, start, 0x7e2a0280u, 0xf09c0f08u,
        0x00610a14u, 0xbf8c3f70u, 0xe0781000u, 0x80000a03u, 0xbf810000u,
    };
}

alignas(256) constexpr std::array<std::uint32_t, 10> RoundTrip{
    0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x7e3e0280u, 0xf0001708u, 0x00010a1eu, 0xbf8c3f70u, 0xf0201708u,
    0x00040a1eu, 0xbf810000u,
};

alignas(256) constexpr auto LoadXyzw = LoadCode(0xfu, 0u);
alignas(256) constexpr auto LoadXyz = LoadCode(0x7u, 0u);
alignas(256) constexpr auto LoadXy = LoadCode(0x3u, 0u);
alignas(256) constexpr auto LoadD16 = LoadCode(0xfu, D16);
alignas(256) constexpr auto StoreXyz = StoreCode(0x7u, 0u);
alignas(256) constexpr auto StoreXz = StoreCode(0x5u, 0u);
alignas(256) constexpr auto StoreD16 = StoreCode(0x7u, D16);
alignas(256) constexpr auto SampleLz = SamplerCode(0xf09c0f08u);
alignas(256) constexpr auto Gather4Lz = SamplerCode(0xf11c0108u);
alignas(256) constexpr auto GetLod = SamplerCode(0xf1800308u);
alignas(256) constexpr auto SampleAt = SampleAtCode(SampleStep, SampleStart);
alignas(256) constexpr auto SampleWrapped = SampleAtCode(SampleStep, WrappedStart);
alignas(256) constexpr auto SampleFloat = SampleAtCode(SampleStep, FloatStart);
alignas(256) constexpr auto SampleOffGrid = SampleAtCode(OffGridStep, SampleStart);
alignas(256) constexpr auto SampleWrappedOffGrid = SampleAtCode(OffGridStep, WrappedStart);
alignas(256) constexpr auto SampleFloatOffGrid = SampleAtCode(OffGridStep, FloatStart);

alignas(256) constexpr std::array<std::uint32_t, 10> SampleCube{
    0x34060084u, 0xe0381000u, 0x80041403u, 0xbf8c3f70u, 0xf09c0f18u,
    0x00610a14u, 0xbf8c3f70u, 0xe0781000u, 0x80000a03u, 0xbf810000u,
};

constexpr std::array<std::uint32_t, 11> StoreValues{
    0x00000000u, 0x3f800000u, 0x3f000000u, 0x3eaaaaabu, 0x80000000u, 0xbf800000u,
    0x40000000u, 0x7f800000u, 0x7fc00000u, 0x000116c2u, 0x3f7fbe77u,
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "image converted unorm: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(const void* data, std::uint32_t format, std::uint32_t swizzle, std::uint32_t width) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((width - 1u) & 3u) << 30u),
        (width - 1u) >> 2u,
        swizzle | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::uint32_t LoadTexel(std::uint32_t index) {
    return index | ((2047u - index) << 11u) | ((index & 1023u) << 22u);
}

std::uint32_t Field(std::uint32_t texel, std::uint32_t component) {
    return component == 2u ? texel >> 22u : (texel >> (component * 11u)) & 2047u;
}

struct Expected {
    std::uint32_t bits;
    std::uint32_t ulps;
};

Expected Selected(std::uint32_t texel, std::uint32_t swizzle, std::uint32_t component, bool unorm) {
    const auto selector = (swizzle >> (component * 3u)) & 7u;
    if (selector == 0u) return {0u, 0u};
    if (selector == 1u) return {unorm ? 0x3f800000u : 1u, 0u};
    const auto field = Field(texel, selector - 4u);
    const auto maximum = selector == 6u ? 1023u : 2047u;
    if (!unorm) return {field, 0u};
    return {std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<double>(field) / maximum)), field == 0u || field == maximum ? 0u : DivisionUlps};
}

std::uint32_t StoreField(std::uint32_t bits, std::uint32_t maximum) {
    const auto value = std::bit_cast<float>(bits);
    if (!(value > 0.0f)) return 0u;
    if (value >= 1.0f) return maximum;
    return static_cast<std::uint32_t>(std::nearbyint(static_cast<double>(value) * maximum));
}

void Run(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::array<std::uint32_t, 8>& texture, const std::array<std::uint32_t, 4>& extra, std::uint32_t groups, const std::array<std::uint32_t, 8>& target = {}) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(24, 0u);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(output.begin(), output.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    std::copy(extra.begin(), extra.end(), userData.begin() + 12);
    std::copy(target.begin(), target.end(), userData.begin() + 16);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

auto NativeSampleReference(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, std::span<const std::uint32_t> code, std::array<std::uint32_t, 8> texture, const std::array<std::uint32_t, 4>& sampler, const std::array<std::uint32_t, 8>& target = {}) {
    const auto results = Output;
    GuestBlock native;
    const auto resource = AgcDriver::Graphics::DecodeTextureResource(texture);
    const auto bytes = AgcDriver::Graphics::DescribeSurface(resource).guestBytes;
    Require(bytes <= BlockBytes, "native sample fixture exceeds its allocation");
    for (std::size_t offset = 0; offset < bytes; offset += 4u) {
        std::uint32_t word = 0;
        std::memcpy(&word, texels + offset, sizeof(word));
        word = AgcDriver::Graphics::RepackTextureWord(word);
        std::memcpy(native.Data() + offset, &word, sizeof(word));
    }
    const auto address = reinterpret_cast<std::uintptr_t>(native.Data());
    texture[0] = static_cast<std::uint32_t>(address >> 8u);
    texture[1] = (texture[1] & ~0x1ff000ffu) | (36u << 20u) | static_cast<std::uint32_t>(address >> 40u);
    const auto swizzle = texture[3] & 0xfffu;
    texture[3] &= ~0xfffu;
    for (std::uint32_t component = 0; component < 4u; ++component) {
        const auto selector = (swizzle >> (component * 3u)) & 7u;
        texture[3] |= (selector == 4u ? 6u : selector == 6u ? 4u : selector) << (component * 3u);
    }
    Run(device, code, texture, sampler, 1, target);
    const auto reference = Output;
    Require(reference == results, "repacked sampling differs from native format 36");
    Output = results;
    return reference;
}

void CheckLoad(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, std::span<const std::uint32_t> code, std::uint32_t dmask, std::uint32_t format, std::uint32_t swizzle, const std::string& what) {
    Run(device, code, TextureDescriptor(texels, format, swizzle, LoadWidth), {}, LoadGroups);
    for (std::uint32_t index = 0; index < LoadWidth; ++index) {
        std::uint32_t slot = 0;
        for (std::uint32_t component = 0; component < 4u; ++component) {
            if (((dmask >> component) & 1u) == 0u) continue;
            const auto expected = Selected(LoadTexel(index), swizzle, component, format == UnormFormat);
            const auto actual = Output[index * 4u + slot++];
            const auto distance = actual > expected.bits ? actual - expected.bits : expected.bits - actual;
            Require(distance <= expected.ulps, what + ": texel " + std::to_string(index) + " component " + std::to_string(component) + " is " + Hex(actual) + ", expected " + Hex(expected.bits) + " within " + std::to_string(expected.ulps) + " ULP");
        }
    }
}

void CheckRoundTrip(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    auto* target = texels + RoundTripOffset;
    std::fill(target, target + LoadWidth * 4u, static_cast<std::uint8_t>(0xa5u));
    Run(device, RoundTrip, TextureDescriptor(texels, UnormFormat, SwizzleXYZ1, LoadWidth), {}, LoadGroups, TextureDescriptor(target, UnormFormat, SwizzleXYZ1, LoadWidth));
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(target), LoadWidth * 4u, nullptr, "test");
    device.WaitIdle();
    for (std::uint32_t index = 0; index < LoadWidth; ++index) {
        std::uint32_t actual = 0;
        std::memcpy(&actual, target + index * 4u, 4u);
        Require(actual == LoadTexel(index), "image_load then image_store: texel " + std::to_string(index) + " is " + Hex(actual) + ", expected " + Hex(LoadTexel(index)));
    }
}

void CheckStore(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::span<const std::uint32_t> code, std::uint32_t dmask, const std::string& what) {
    std::fill(texels, texels + Threads * 4u, static_cast<std::uint8_t>(0xa5u));
    Input.fill(0u);
    std::array<std::uint32_t, Threads> expected{};
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::array<std::uint32_t, 3> values{StoreValues[tid % 11u], StoreValues[(tid + 4u) % 11u], StoreValues[(tid + 8u) % 11u]};
        std::uint32_t slot = 0;
        for (std::uint32_t component = 0; component < 3u; ++component) {
            if (((dmask >> component) & 1u) == 0u) continue;
            Input[tid * 4u + slot++] = values[component];
            expected[tid] |= StoreField(values[component], component == 2u ? 1023u : 2047u) << (component * 11u);
        }
    }
    Run(device, code, TextureDescriptor(texels, UnormFormat, SwizzleXYZ1, Threads), BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u)), 1);
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(texels), Threads * 4u, nullptr, "test");
    device.WaitIdle();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        std::uint32_t actual = 0;
        std::memcpy(&actual, texels + tid * 4u, 4u);
        Require(actual == expected[tid], what + ": texel " + std::to_string(tid) + " is " + Hex(actual) + ", expected " + Hex(expected[tid]));
    }
}

constexpr std::array<float, 11> FloatValues{0.0f, 1.0f, 0.5f, 2.0f, 0.25f, 3.0f, 1.5f, 10.0f, 0.75f, 6.5f, 0.125f};

constexpr std::array<std::array<std::uint32_t, 4>, 7> FloatStoreEdges{{
    {0xbf800000u, 0x80000000u, 0x7fc00000u, 0xfc000000u},
    {0x7f800000u, 0x7149f2cau, 0x477fff00u, 0xf7feffe0u},
    {0x3f830000u, 0x3f818000u, 0x35800000u, 0x002f01e0u},
    {0x00000001u, 0xff800000u, 0xffc00000u, 0xfc000000u},
    {0x477c0000u, 0x4788b800u, 0x33000000u, 0x001effdfu},
    {0x36400000u, 0x477e0000u, 0x3dcccccdu, 0x5cdefc01u},
    {0x7f800001u, 0x7f810000u, 0x7fa00000u, 0xfa1f07e1u},
}};

constexpr std::array<std::array<std::uint32_t, 4>, 3> FloatLoadEdges{{
    {0x3e0u | (0x7c0u << 10u) | (0x03fu << 21u), 0x7f800000u, 0x7f800000u, 0x387c0000u},
    {0x001u | (0x041u << 10u) | (0x7bfu << 21u), 0x36000000u, 0x38820000u, 0x477e0000u},
    {0x3dfu | (0x001u << 10u), 0x477c0000u, 0x35800000u, 0x00000000u},
}};

std::uint32_t SmallFloat(float value, std::uint32_t mantissaBits) {
    if (value == 0.0f) return 0u;
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = ((bits >> 23u) & 0xffu) - 127u + 15u;
    return (exponent << mantissaBits) | ((bits & 0x7fffffu) >> (23u - mantissaBits));
}

std::uint32_t FloatTexel(std::uint32_t index) {
    return SmallFloat(FloatValues[index % 11u], 5u) | (SmallFloat(FloatValues[(index + 4u) % 11u], 6u) << 10u) | (SmallFloat(FloatValues[(index + 8u) % 11u], 6u) << 21u);
}

void CheckFloatLoad(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    for (std::uint32_t index = 0; index < LoadWidth; ++index) {
        const auto texel = index < FloatLoadEdges.size() ? FloatLoadEdges[index][0] : FloatTexel(index);
        std::memcpy(texels + index * 4u, &texel, 4u);
    }
    Run(device, LoadXyzw, TextureDescriptor(texels, FloatFormat, SwizzleXYZ1, LoadWidth), {}, LoadGroups);
    for (std::uint32_t index = 0; index < LoadWidth; ++index) {
        std::array<std::uint32_t, 4> expected{std::bit_cast<std::uint32_t>(FloatValues[index % 11u]), std::bit_cast<std::uint32_t>(FloatValues[(index + 4u) % 11u]), std::bit_cast<std::uint32_t>(FloatValues[(index + 8u) % 11u]), std::bit_cast<std::uint32_t>(1.0f)};
        if (index < FloatLoadEdges.size()) {
            expected = {FloatLoadEdges[index][1], FloatLoadEdges[index][2], FloatLoadEdges[index][3], std::bit_cast<std::uint32_t>(1.0f)};
        }
        for (std::uint32_t component = 0; component < 4u; ++component) {
            const auto actual = Output[index * 4u + component];
            Require(actual == expected[component], "image_load of R10_G11_B11_FLOAT: texel " + std::to_string(index) + " component " + std::to_string(component) + " is " + Hex(actual) + ", expected " + Hex(expected[component]));
        }
    }
}

void CheckFloatStore(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    std::fill(texels, texels + Threads * 4u, static_cast<std::uint8_t>(0xa5u));
    Input.fill(0u);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        Input[tid * 4u + 0u] = std::bit_cast<std::uint32_t>(FloatValues[tid % 11u]);
        Input[tid * 4u + 1u] = std::bit_cast<std::uint32_t>(FloatValues[(tid + 4u) % 11u]);
        Input[tid * 4u + 2u] = std::bit_cast<std::uint32_t>(FloatValues[(tid + 8u) % 11u]);
        if (tid < FloatStoreEdges.size()) {
            for (std::uint32_t component = 0; component < 3u; ++component) {
                Input[tid * 4u + component] = FloatStoreEdges[tid][component];
            }
        }
    }
    Run(device, StoreXyz, TextureDescriptor(texels, FloatFormat, SwizzleXYZ1, Threads), BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u)), 1);
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(texels), Threads * 4u, nullptr, "test");
    device.WaitIdle();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        std::uint32_t actual = 0;
        std::memcpy(&actual, texels + tid * 4u, 4u);
        const auto expected = tid < FloatStoreEdges.size() ? FloatStoreEdges[tid][3] : FloatTexel(tid);
        Require(actual == expected, "image_store of R10_G11_B11_FLOAT: texel " + std::to_string(tid) + " is " + Hex(actual) + ", expected " + Hex(expected));
    }
}

void CheckFloatSample(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, const std::array<std::uint32_t, 4>& expected, const std::string& what) {
    Run(device, SampleLz, TextureDescriptor(texels, FloatFormat, SwizzleXYZ1, Threads), PointSampler, 1);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        for (std::uint32_t component = 0; component < 4u; ++component) {
            const auto actual = Output[tid * 4u + component];
            const bool nan = (expected[component] & 0x7f800000u) == 0x7f800000u && (expected[component] & 0x7fffffu) != 0u;
            Require(nan ? std::isnan(std::bit_cast<float>(actual)) : actual == expected[component], what + ": thread " + std::to_string(tid) + " component " + std::to_string(component) + " is " + Hex(actual) + ", expected " + Hex(expected[component]));
        }
    }
}

double ChannelValue(std::uint32_t texel, std::uint32_t swizzle, std::uint32_t component, bool floating) {
    const auto selector = (swizzle >> (component * 3u)) & 7u;
    if (selector == 0u) return 0.0;
    if (selector == 1u) return 1.0;
    if (floating) {
        const auto bits = selector == 4u ? texel & 0x3ffu : selector == 5u ? (texel >> 10u) & 0x7ffu : texel >> 21u;
        const auto mantissaBits = selector == 4u ? 5u : 6u;
        const auto exponent = bits >> mantissaBits;
        const auto mantissa = bits & ((1u << mantissaBits) - 1u);
        if (exponent == 0u) return std::ldexp(static_cast<double>(mantissa), -14 - static_cast<int>(mantissaBits));
        return std::ldexp(1.0 + static_cast<double>(mantissa) / (1u << mantissaBits), static_cast<int>(exponent) - 15);
    }
    return static_cast<double>(Field(texel, selector - 4u)) / (selector == 6u ? 1023.0 : 2047.0);
}

void CheckVolume(AgcDriver::VulkanDevice& device, std::uint32_t format) {
    constexpr std::array<std::uint32_t, 3> extents{4u, 3u, 2u};
    constexpr std::array<std::array<float, 3>, 8> coordinates{{
        {0.125f, 0.5f, 0.25f}, {0.3717f, 0.6193f, 0.5911f}, {-0.31f, 1.37f, -0.17f},
        {1.29f, -0.45f, 1.63f}, {0.63f, 0.21f, 0.49f}, {0.63f, 0.21f, 0.51f},
        {0.91f, 0.87f, 0.5009765625f}, {0.91f, 0.87f, 0.5029296875f}
    }};
    GuestBlock block;
    auto texture = TextureDescriptor(block.Data(), format, SwizzleZYX1, extents[0]);
    texture[2] |= (extents[1] - 1u) << 14u;
    texture[3] = SwizzleZYX1 | (10u << 28u);
    texture[4] = extents[2] - 1u;
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(texture));
    Require(geometry.guestBytes <= BlockBytes, "volume fixture exceeds its allocation");
    const auto texel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        const auto index = (z * extents[1] + y) * extents[0] + x;
        return format == FloatFormat ? FloatTexel(index) : LoadTexel(index * 53u);
    };
    for (std::uint32_t z = 0; z < extents[2]; ++z) {
        for (std::uint32_t y = 0; y < extents[1]; ++y) {
            for (std::uint32_t x = 0; x < extents[0]; ++x) {
                const auto value = texel(x, y, z);
                std::memcpy(block.Data() + geometry.GuestLayerOffset(z) + y * geometry.mips[0].pitchBytes + x * 4u, &value, sizeof(value));
            }
        }
    }
    alignas(256) auto code = SampleCube;
    code[4] = 0xf09c0f10u;
    std::array<std::uint32_t, 8> input{};
    const auto buffer = BufferDescriptor(Input.data(), sizeof(Input));
    std::copy(buffer.begin(), buffer.end(), input.begin());
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        for (std::uint32_t axis = 0; axis < 3u; ++axis) Input[tid * 4u + axis] = std::bit_cast<std::uint32_t>(coordinates[tid % coordinates.size()][axis]);
    }
    for (const bool linear : {false, true}) {
        for (std::uint32_t zFilter = 0; zFilter < 3u; ++zFilter) {
            for (std::uint32_t rotation = 0; rotation < 3u; ++rotation) {
                std::array<std::uint32_t, 3> modes{};
                auto sampler = linear ? BilinearSampler : PointSampler;
                sampler[0] = 1u << 27u;
                sampler[2] |= zFilter << 24u;
                for (std::uint32_t axis = 0; axis < 3u; ++axis) {
                    modes[axis] = (rotation + axis) % 3u;
                    sampler[0] |= modes[axis] << (axis * 3u);
                }
                Run(device, code, texture, sampler, 1, input);
                const bool linearZ = zFilter == 0u ? linear : zFilter == 2u;
                for (std::uint32_t tid = 0; tid < Threads; ++tid) {
                    std::array<int, 3> first{};
                    std::array<double, 3> weights{};
                    for (std::uint32_t axis = 0; axis < 3u; ++axis) {
                        const bool filtered = axis == 2u ? linearZ : linear;
                        const double position = coordinates[tid % coordinates.size()][axis] * extents[axis] - (filtered ? 0.5 : 0.0);
                        first[axis] = static_cast<int>(std::floor(position));
                        weights[axis] = filtered ? std::nearbyint((position - first[axis]) * 256.0) / 256.0 : 0.0;
                    }
                    const auto address = [&](int index, std::uint32_t axis) {
                        const auto extent = static_cast<int>(extents[axis]);
                        if (modes[axis] == 2u) return std::clamp(index, 0, extent - 1);
                        const auto cycle = static_cast<int>(std::floor(static_cast<double>(index) / extent));
                        const auto position = index - cycle * extent;
                        return modes[axis] == 1u && cycle % 2 != 0 ? extent - 1 - position : position;
                    };
                    for (std::uint32_t channel = 0; channel < 4u; ++channel) {
                        double expected = 0;
                        for (std::uint32_t tap = 0; tap < 8u; ++tap) {
                            double factor = 1;
                            std::array<std::uint32_t, 3> at{};
                            for (std::uint32_t axis = 0; axis < 3u; ++axis) {
                                const auto high = (tap >> axis) & 1u;
                                factor *= high ? weights[axis] : 1.0 - weights[axis];
                                at[axis] = static_cast<std::uint32_t>(address(first[axis] + static_cast<int>(high), axis));
                            }
                            expected += factor * ChannelValue(texel(at[0], at[1], at[2]), SwizzleZYX1, channel, format == FloatFormat);
                        }
                        const auto actual = std::bit_cast<float>(Output[tid * 4u + channel]);
                        Require(std::fabs(actual - expected) <= SampleTolerance * std::max(1.0, std::fabs(expected)), "converted volume format " + std::to_string(format) + " XY " + std::to_string(linear) + " Z " + std::to_string(zFilter) + " rotation " + std::to_string(rotation) + " thread " + std::to_string(tid) + " channel " + std::to_string(channel) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected));
                    }
                }
            }
        }
    }
    auto sampler = BilinearSampler;
    sampler[2] |= 3u << 24u;
    bool rejected = false;
    try { Run(device, code, texture, sampler, 1, input); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("reserved Z filter") != std::string::npos; }
    Require(rejected, "converted volume accepted reserved Z filtering");
    sampler[2] = 1u << 24u;
    rejected = false;
    try { Run(device, code, texture, sampler, 1, input); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("without TRUNC_COORD") != std::string::npos; }
    Require(rejected, "converted volume accepted unimplemented point coordinate rounding");
}

void CheckCube(AgcDriver::VulkanDevice& device, std::uint32_t format, bool linear, bool seamless) {
    constexpr std::uint32_t side = 4;
    constexpr std::uint32_t faces = 12;
    constexpr std::array<std::array<float, 2>, 11> coordinates{{
        {0.5f, 0.5f}, {0, 0.25f}, {1, 0.75f}, {0.25f, 0}, {0.75f, 1},
        {0, 0}, {1, 0}, {0, 1}, {1, 1}, {0.03125f, 0.65625f}, {-0.25f, 1.25f}
    }};
    GuestBlock block;
    auto texture = TextureDescriptor(block.Data(), format, SwizzleZYX1, side);
    texture[2] |= (side - 1u) << 14u;
    texture[3] = (texture[3] & 0x0fffffffu) | (11u << 28u);
    texture[4] = faces - 1u;
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(texture));
    Require(geometry.guestBytes <= BlockBytes, "cube fixture exceeds its allocation");
    const auto texel = [&](std::uint32_t face, std::uint32_t x, std::uint32_t y) {
        const auto index = face * side * side + y * side + x;
        return format == FloatFormat ? FloatTexel(index) : LoadTexel(index * 9u);
    };
    for (std::uint32_t face = 0; face < faces; ++face) {
        for (std::uint32_t y = 0; y < side; ++y) {
            for (std::uint32_t x = 0; x < side; ++x) {
                const auto value = texel(face, x, y);
                std::memcpy(block.Data() + geometry.GuestLayerOffset(face) + y * geometry.mips[0].pitchBytes + x * 4u, &value, sizeof(value));
            }
        }
    }
    const auto read = [&](std::uint32_t face, int x, int y, std::uint32_t component) {
        return ChannelValue(texel(face, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)), SwizzleZYX1, component, format == FloatFormat);
    };
    const auto adjacent = [&](std::uint32_t face, int x, int y, std::uint32_t component) {
        const double u = (x + 0.5) * 2.0 / side - 1.0;
        const double v = (y + 0.5) * 2.0 / side - 1.0;
        const std::array<std::array<double, 3>, 6> directions{{{1, -v, -u}, {-1, -v, u}, {u, 1, v}, {u, -1, -v}, {u, -v, 1}, {-u, -v, -1}}};
        const auto direction = directions[face % 6u];
        const auto ax = std::fabs(direction[0]);
        const auto ay = std::fabs(direction[1]);
        const auto az = std::fabs(direction[2]);
        std::uint32_t neighbor;
        double s, t;
        if (ax >= ay && ax >= az) {
            neighbor = direction[0] > 0 ? 0u : 1u;
            s = (direction[0] > 0 ? -direction[2] : direction[2]) / ax;
            t = -direction[1] / ax;
        } else if (ay >= az) {
            neighbor = direction[1] > 0 ? 2u : 3u;
            s = direction[0] / ay;
            t = (direction[1] > 0 ? direction[2] : -direction[2]) / ay;
        } else {
            neighbor = direction[2] > 0 ? 4u : 5u;
            s = (direction[2] > 0 ? direction[0] : -direction[0]) / az;
            t = -direction[1] / az;
        }
        return read(face / 6u * 6u + neighbor, std::clamp(static_cast<int>(std::floor((s + 1.0) * side / 2.0)), 0, 3), std::clamp(static_cast<int>(std::floor((t + 1.0) * side / 2.0)), 0, 3), component);
    };
    const auto tap = [&](std::uint32_t face, int x, int y, std::uint32_t component) {
        const auto cx = std::clamp(x, 0, 3);
        const auto cy = std::clamp(y, 0, 3);
        if (!seamless) return read(face, (x % 4 + 4) % 4, (y % 4 + 4) % 4, component);
        if (!linear || (x == cx && y == cy)) return read(face, cx, cy, component);
        if (x != cx && y != cy) return (read(face, cx, cy, component) + adjacent(face, x, cy, component) + adjacent(face, cx, y, component)) / 3.0;
        return adjacent(face, x, y, component);
    };
    std::array<std::uint32_t, 8> input{};
    const auto buffer = BufferDescriptor(Input.data(), sizeof(Input));
    std::copy(buffer.begin(), buffer.end(), input.begin());
    auto sampler = linear ? BilinearSampler : PointSampler;
    sampler[0] = seamless ? 0u : 1u << 28u;
    for (std::uint32_t first = 0; first < faces * coordinates.size(); first += Threads) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const auto index = (first + tid) % (faces * coordinates.size());
            const auto face = index / coordinates.size();
            const auto point = coordinates[index % coordinates.size()];
            Input[tid * 4u] = std::bit_cast<std::uint32_t>(1.0f + point[0]);
            Input[tid * 4u + 1u] = std::bit_cast<std::uint32_t>(1.0f + point[1]);
            Input[tid * 4u + 2u] = std::bit_cast<std::uint32_t>(static_cast<float>(face / 6u * 8u + face % 6u));
        }
        Run(device, SampleCube, texture, sampler, 1, input);
        const auto nativeResults = format == FloatFormat ? NativeSampleReference(device, block.Data(), SampleCube, texture, sampler, input) : Output;
        for (std::uint32_t tid = 0; tid < Threads && first + tid < faces * coordinates.size(); ++tid) {
            const auto index = first + tid;
            const auto face = index / coordinates.size();
            const auto point = coordinates[index % coordinates.size()];
            const double u = seamless ? std::clamp(point[0], 0.0f, 1.0f) : point[0];
            const double v = seamless ? std::clamp(point[1], 0.0f, 1.0f) : point[1];
            const double x = u * side - (linear ? 0.5 : 0.0);
            const double y = v * side - (linear ? 0.5 : 0.0);
            const auto ix = static_cast<int>(std::floor(x));
            const auto iy = static_cast<int>(std::floor(y));
            const double wx = std::nearbyint((x - ix) * 256.0) / 256.0;
            const double wy = std::nearbyint((y - iy) * 256.0) / 256.0;
            for (std::uint32_t component = 0; component < 4u; ++component) {
                double expected = tap(face, ix, iy, component);
                if (linear) expected = std::lerp(std::lerp(expected, tap(face, ix + 1, iy, component), wx), std::lerp(tap(face, ix, iy + 1, component), tap(face, ix + 1, iy + 1, component), wx), wy);
                if (format == FloatFormat) expected = std::bit_cast<float>(nativeResults[tid * 4u + component]);
                const auto actual = std::bit_cast<float>(Output[tid * 4u + component]);
                Require(std::fabs(actual - expected) <= SampleTolerance * std::max(1.0, std::fabs(expected)), "converted cube face " + std::to_string(face) + " sample " + std::to_string(index % coordinates.size()) + " channel " + std::to_string(component) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected));
            }
        }
    }
}

void CheckMipSample(AgcDriver::VulkanDevice& device, std::uint32_t format, std::uint32_t type, bool linear) {
    GuestBlock block;
    auto texture = TextureDescriptor(block.Data(), format, SwizzleZYX1, 4u);
    texture[2] |= 3u << 14u;
    texture[3] = SwizzleZYX1 | (type << 28u) | (2u << 16u);
    texture[4] = type == 11u ? 11u : type == 13u ? 2u : type == 10u ? 3u : 0u;
    texture[5] = 2u << 4u;
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(texture));
    Require(geometry.guestBytes <= BlockBytes, "mip fixture exceeds its allocation");
    const auto layers = texture[4] + 1u;
    const auto texel = [&](std::uint32_t layer, std::uint32_t level) {
        const auto index = layer * 3u + level;
        return format == FloatFormat ? FloatTexel(index) : LoadTexel(index * 47u);
    };
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t level = 0; level < 3u; ++level) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto& mip = geometry.mips[level];
            const auto value = texel(layer, level);
            for (std::uint32_t y = 0; y < mip.height; ++y) {
                for (std::uint32_t x = 0; x < mip.width; ++x) std::memcpy(block.Data() + geometry.GuestLayerOffset(layer) + mip.tiledOffset + y * mip.pitchBytes + x * 4u, &value, sizeof(value));
            }
        }
    }
    alignas(256) auto code = SampleCube;
    code[4] = type == Type2D ? 0xf0900f08u : type == 10u ? 0xf0900f10u : 0xf0900f18u;
    std::array<std::uint32_t, 8> input{};
    const auto buffer = BufferDescriptor(Input.data(), sizeof(Input));
    std::copy(buffer.begin(), buffer.end(), input.begin());
    struct Case { std::uint32_t filter; float minimum; float maximum; float bias; std::uint32_t base; };
    constexpr std::array<Case, 7> cases{{{0, 1, 2, 0, 0}, {1, 0, 15, 0, 0}, {2, 0, 15, 0, 0}, {2, 0.75f, 1.25f, 0.5f, 0}, {2, 0, 2, -0.5f, 0}, {2, 0, 15, 0, 1}, {1, 0, 15, 0, 1}}};
    constexpr std::array<float, 10> lods{-1, 0, 0.25f, 0.75f, 1, 1.5f, 2, 4, 0.49f, 1.50390625f};
    for (const auto& test : cases) {
        auto sampler = linear ? BilinearSampler : PointSampler;
        if (type == 10u) {
            sampler[0] |= 1u << 27u;
            sampler[2] |= 1u << 24u;
        }
        sampler[1] = static_cast<std::uint32_t>(test.minimum * 256.0f) | (static_cast<std::uint32_t>(test.maximum * 256.0f) << 12u);
        sampler[2] |= (test.filter << 26u) | (static_cast<std::uint32_t>(static_cast<std::int32_t>(test.bias * 256.0f)) & 0x3fffu);
        texture[3] = (texture[3] & ~(15u << 12u)) | (test.base << 12u);
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const auto layer = tid % layers;
            Input[tid * 4u] = std::bit_cast<std::uint32_t>(type == 11u ? 1.5f : 0.5f);
            Input[tid * 4u + 1u] = Input[tid * 4u];
            Input[tid * 4u + 2u] = std::bit_cast<std::uint32_t>(type == Type2D ? lods[tid % lods.size()] : static_cast<float>(type == 11u ? layer / 6u * 8u + layer % 6u : layer));
            if (type == 10u) Input[tid * 4u + 2u] = std::bit_cast<std::uint32_t>((static_cast<float>(layer) + 0.5f) / layers);
            Input[tid * 4u + 3u] = std::bit_cast<std::uint32_t>(lods[tid % lods.size()]);
        }
        Run(device, code, texture, sampler, 1, input);
        const auto nativeResults = format == FloatFormat && type != 10u ? NativeSampleReference(device, block.Data(), code, texture, sampler, input) : Output;
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const auto layer = tid % layers;
            const float lod = test.filter == 0 ? 0 : std::clamp(std::clamp(lods[tid % lods.size()] + test.bias, test.minimum, test.maximum), 0.0f, static_cast<float>(2u - test.base));
            const auto low = static_cast<std::uint32_t>(test.filter == 2 ? std::floor(lod) : std::floor(lod + 0.5f)) + test.base;
            const auto high = std::min(low + 1u, 2u);
            const auto weight = test.filter == 2 ? std::nearbyint((lod - std::floor(lod)) * 256.0f) / 256.0f : 0.0f;
            const auto lowLayer = type == 10u ? layer >> low : layer;
            const auto highLayer = type == 10u ? layer >> high : layer;
            for (std::uint32_t component = 0; component < 4u; ++component) {
                auto expected = std::lerp(ChannelValue(texel(lowLayer, low), SwizzleZYX1, component, format == FloatFormat), ChannelValue(texel(highLayer, high), SwizzleZYX1, component, format == FloatFormat), weight);
                if (format == FloatFormat && type != 10u) expected = std::bit_cast<float>(nativeResults[tid * 4u + component]);
                const auto actual = std::bit_cast<float>(Output[tid * 4u + component]);
                Require(std::fabs(actual - expected) <= SampleTolerance * std::max(1.0, std::fabs(expected)), "converted mip sample type " + std::to_string(type) + " filter " + std::to_string(test.filter) + " base " + std::to_string(test.base) + " thread " + std::to_string(tid) + " channel " + std::to_string(component) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected));
            }
        }
    }
}

void CheckUnnormalizedSample(AgcDriver::VulkanDevice& device, std::uint32_t format, bool offset) {
    GuestBlock block;
    auto texture = TextureDescriptor(block.Data(), format, SwizzleZYX1, 4u);
    texture[2] |= 3u << 14u;
    texture[3] |= 2u << 16u;
    texture[5] = 2u << 4u;
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(texture));
    Require(geometry.guestBytes <= BlockBytes, "unnormalized fixture exceeds its allocation");
    const auto texel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t level) {
        const auto index = level * 17u + y * 4u + x;
        return format == FloatFormat ? FloatTexel(index) : LoadTexel(index * 23u);
    };
    for (std::uint32_t level = 0; level < 3u; ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t y = 0; y < mip.height; ++y) {
            for (std::uint32_t x = 0; x < mip.width; ++x) {
                const auto value = texel(x, y, level);
                std::memcpy(block.Data() + mip.tiledOffset + y * mip.pitchBytes + x * 4u, &value, sizeof(value));
            }
        }
    }
    alignas(256) auto code = SampleCube;
    code[4] = offset ? 0xf0d00f08u : 0xf0900f08u;
    std::array<std::uint32_t, 8> input{};
    const auto buffer = BufferDescriptor(Input.data(), sizeof(Input));
    std::copy(buffer.begin(), buffer.end(), input.begin());
    constexpr std::array<float, 8> coordinates{-3.0f, -0.25f, 0.0f, 0.7503f, 1.2517f, 2.5f, 3.75f, 7.0f};
    struct Case { std::uint32_t mag; std::uint32_t min; std::uint32_t x; std::uint32_t y; std::uint32_t border; std::uint32_t swizzle; };
    constexpr std::array<Case, 12> cases{{
        {0, 0, 0, 0, 0, SwizzleZYX1}, {0, 1, 0, 0, 0, SwizzleZYX1}, {0, 0, 2, 2, 0, SwizzleZYX1}, {0, 1, 2, 2, 0, SwizzleZYX1},
        {1, 0, 0, 0, 0, SwizzleZYX1}, {1, 1, 2, 2, 0, SwizzleZYX1}, {1, 0, 6, 6, 0, SwizzleZYX1}, {1, 1, 6, 6, 1, SwizzleX011},
        {1, 1, 6, 6, 2, SwizzleX011}, {1, 0, 6, 2, 2, SwizzleZYX1}, {1, 1, 0, 6, 0, SwizzleZYX1}, {1, 0, 2, 6, 1, SwizzleX011}
    }};
    for (const auto& test : cases) {
        for (std::uint32_t base = 0; base < 3u; ++base) {
            auto sampler = PointSampler;
            sampler[0] = (1u << 15u) | test.x | (test.y << 3u);
            sampler[1] = 0x00200100u;
            sampler[2] = (test.min << 22u) | (test.mag << 20u) | (2u << 24u) | 256u;
            sampler[3] = test.border << 30u;
            texture[3] = (texture[3] & ~0xffffu) | (base << 12u) | test.swizzle;
            for (std::uint32_t tid = 0; tid < Threads; ++tid) {
                const auto first = tid * 4u + (offset ? 1u : 0u);
                Input[tid * 4u] = ((tid % 7u - 3u) & 0x3fu) | (((tid % 5u - 2u) & 0x3fu) << 8u);
                Input[first] = std::bit_cast<std::uint32_t>(coordinates[tid % coordinates.size()]);
                Input[first + 1u] = std::bit_cast<std::uint32_t>(coordinates[(tid / 4u) % coordinates.size()]);
                Input[first + 2u] = std::bit_cast<std::uint32_t>(static_cast<float>(tid % 7u) - 2.0f);
            }
            Run(device, code, texture, sampler, 1, input);
            const auto nativeResults = format == FloatFormat ? NativeSampleReference(device, block.Data(), code, texture, sampler, input) : Output;
            for (std::uint32_t tid = 0; tid < Threads; ++tid) {
                const auto position = [&](std::uint32_t component) {
                    const auto coordinate = std::bit_cast<float>(Input[tid * 4u + component + (offset ? 1u : 0u)]);
                    const auto delta = !offset ? 0 : component == 0u ? static_cast<int>(tid % 7u) - 3 : static_cast<int>(tid % 5u) - 2;
                    return static_cast<double>(coordinate) + delta - (test.mag == 1u ? 0.5 : 0.0);
                };
                const auto x = static_cast<int>(std::floor(position(0)));
                const auto y = static_cast<int>(std::floor(position(1)));
                const auto weightX = std::nearbyint((position(0) - x) * 256.0) / 256.0;
                const auto weightY = std::nearbyint((position(1) - y) * 256.0) / 256.0;
                const auto size = static_cast<int>(4u >> base);
                for (std::uint32_t component = 0; component < 4u; ++component) {
                    const auto tap = [&](int u, int v) {
                        if ((test.x == 6u && (u < 0 || u >= size)) || (test.y == 6u && (v < 0 || v >= size))) {
                            const auto selector = (test.swizzle >> (component * 3u)) & 7u;
                            return selector == 0u ? 0.0 : selector == 1u || test.border == 2u ? 1.0 : 0.0;
                        }
                        return ChannelValue(texel(std::clamp(u, 0, size - 1), std::clamp(v, 0, size - 1), base), test.swizzle, component, format == FloatFormat);
                    };
                    auto expected = test.mag == 0u ? tap(x, y) : std::lerp(std::lerp(tap(x, y), tap(x + 1, y), weightX), std::lerp(tap(x, y + 1), tap(x + 1, y + 1), weightX), weightY);
                    if (format == FloatFormat) expected = std::bit_cast<float>(nativeResults[tid * 4u + component]);
                    const auto actual = std::bit_cast<float>(Output[tid * 4u + component]);
                    Require(std::fabs(actual - expected) <= SampleTolerance * std::max(1.0, std::fabs(expected)), "unnormalized converted sample filter " + std::to_string(test.mag) + " border " + std::to_string(test.border) + " base " + std::to_string(base) + " thread " + std::to_string(tid) + " channel " + std::to_string(component) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected));
                }
            }
        }
    }
}

void CheckMipCount(AgcDriver::VulkanDevice& device, std::uint32_t format, std::uint32_t type) {
    GuestBlock block;
    auto texture = TextureDescriptor(block.Data(), format, SwizzleZYX1, 4u);
    texture[2] |= 3u << 14u;
    texture[3] = SwizzleZYX1 | (type << 28u) | (2u << 16u);
    texture[4] = type == 11u ? 5u : type == 10u || type == 13u ? 3u : 0u;
    texture[5] = 2u << 4u;
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(texture));
    Require(geometry.guestBytes <= BlockBytes, "mip-count fixture exceeds its allocation");
    std::memset(block.Data(), 0, BlockBytes);
    alignas(256) auto code = SampleCube;
    code[4] = 0xf0380800u;
    code[5] = 0x00010a14u;
    code[7] = 0xe0701000u;
    std::array<std::uint32_t, 8> input{};
    const auto buffer = BufferDescriptor(Input.data(), sizeof(Input));
    std::copy(buffer.begin(), buffer.end(), input.begin());
    for (std::uint32_t tid = 0; tid < Threads; ++tid) Input[tid * 4u] = tid % 4u == 3u ? UINT32_MAX : tid % 4u;
    for (const auto dimension : {0u, 2u, 5u}) {
        code[4] = 0xf0380800u | (dimension << 3u);
        for (std::uint32_t base = 0; base < 3u; ++base) {
            texture[3] = (texture[3] & ~(15u << 12u)) | (base << 12u);
            Run(device, code, texture, {}, 1, input);
            for (std::uint32_t tid = 0; tid < Threads; ++tid) Require(Output[tid * 4u] == 3u - base, "mip-count query depends on instruction dimension or input LOD");
        }
    }
    if (type == 11u) {
        code[4] = 0xf09c0f00u;
        code[5] = 0x00610a14u;
        bool rejected = false;
        try { Run(device, code, texture, PointSampler, 1, input); }
        catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("too few coordinate components") != std::string::npos; }
        Require(rejected, "mip-count query compatibility allowed a 1D sample of a cube");
    }
}

void CheckSample(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, std::span<const std::uint32_t> code, std::uint32_t step, std::uint32_t start, std::uint32_t format, std::uint32_t swizzle, const std::array<std::uint32_t, 4>& sampler, bool linear, bool wrap, const std::string& what, bool mirror = false) {
    Run(device, code, TextureDescriptor(texels, format, swizzle, LoadWidth), sampler, 1);
    const auto nativeResults = format == FloatFormat ? NativeSampleReference(device, texels, code, TextureDescriptor(texels, format, swizzle, LoadWidth), sampler) : Output;
    const auto texelAt = [&](std::int64_t index) {
        const auto width = static_cast<std::int64_t>(LoadWidth);
        auto addressed = wrap ? ((index % width) + width) % width : std::clamp<std::int64_t>(index, 0, width - 1);
        if (mirror) {
            const auto cycle = static_cast<std::int64_t>(std::floor(static_cast<double>(index) / width));
            const auto position = index - cycle * width;
            addressed = cycle % 2 == 0 ? position : width - 1 - position;
        }
        std::uint32_t texel = 0;
        std::memcpy(&texel, texels + addressed * 4u, 4u);
        return texel;
    };
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto x = (static_cast<double>(tid) * static_cast<double>(std::bit_cast<float>(step)) + static_cast<double>(std::bit_cast<float>(start))) * LoadWidth;
        for (std::uint32_t component = 0; component < 4u; ++component) {
            double expected = 0.0;
            if (linear) {
                const auto centre = x - 0.5;
                const auto first = static_cast<std::int64_t>(std::floor(centre));
                const auto weight = std::nearbyint((centre - std::floor(centre)) * 256.0) / 256.0;
                const auto left = ChannelValue(texelAt(first), swizzle, component, format == FloatFormat);
                const auto right = ChannelValue(texelAt(first + 1), swizzle, component, format == FloatFormat);
                expected = left + (right - left) * weight;
            } else {
                expected = ChannelValue(texelAt(static_cast<std::int64_t>(std::floor(x))), swizzle, component, format == FloatFormat);
            }
            if (format == FloatFormat) expected = std::bit_cast<float>(nativeResults[tid * 4u + component]);
            const auto actual = static_cast<double>(std::bit_cast<float>(Output[tid * 4u + component]));
            Require(std::fabs(actual - expected) <= SampleTolerance * std::max(1.0, std::fabs(expected)), what + ": thread " + std::to_string(tid) + " component " + std::to_string(component) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected));
        }
    }
}


void CheckConvertedPreparation(AgcDriver::VulkanDevice& device, const std::uint8_t* texels) {
    std::vector<std::uint32_t> userData(24, 0u);
    const auto texture = TextureDescriptor(texels, UnormFormat, SwizzleXYZ1, LoadWidth);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    std::copy(PointSampler.begin(), PointSampler.end(), userData.begin() + 12);
    const std::span<const std::uint32_t> code = SampleAt;
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    auto converted = request;
    converted.context.convertedSamples = true;
    const auto convertedMode = [](const ShaderRecompiler::ImageResource& mode) { return mode.conversionFormat == ShaderRecompiler::IrBufferFormat::Format11_11_10UNorm || mode.conversionFormat == ShaderRecompiler::IrBufferFormat::Format10_11_11Float; };
    const auto plain = ShaderRecompiler::GetResourcePlan(request);
    Require(plain->info.images.size() == 1u && std::ranges::none_of(plain->info.runtimeImageModes.at(0), convertedMode), "a sampled image is prepared with converted-sample modes before a converted descriptor was seen");
    const auto prepared = ShaderRecompiler::GetResourcePlan(converted);
    Require(prepared->info.images.size() == 1u && std::ranges::count_if(prepared->info.runtimeImageModes.at(0), convertedMode) == 1, "a sampled image is not prepared with the converted unorm sample mode once a converted descriptor was seen");
    const auto handle = ShaderRecompiler::PrepareShader(request);
    Require(!ShaderRecompiler::PreparedShaderInvocation::TryCreate(converted, handle).has_value(), "an artifact prepared without converted-sample modes matches a request that asks for them");
    const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle);
    Require(invocation.has_value(), "the prepared artifact does not match its own request");
    ShaderRecompiler::SrtRuntime runtime;
    runtime.userData = userData;
    runtime.shaderBase = request.shader.codeAddress;
    std::string reason;
    try {
        static_cast<void>(invocation->Capture(runtime));
    } catch (const ShaderRecompiler::ConvertedSampleRequired& error) {
        reason = error.what();
    }
    Require(!reason.empty(), "capturing a converted descriptor through an artifact prepared without converted-sample modes does not ask for them");
    const auto convertedHandle = ShaderRecompiler::PrepareShader(converted);
    const auto convertedInvocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(converted, convertedHandle);
    Require(convertedInvocation.has_value(), "the artifact prepared with converted-sample modes does not match its request");
    static_cast<void>(convertedInvocation->Capture(runtime));
    const auto nativeTexture = TextureDescriptor(texels, FloatFormat, SwizzleXYZ1, LoadWidth);
    std::copy(nativeTexture.begin(), nativeTexture.end(), userData.begin() + 4);
    static_cast<void>(invocation->Capture(runtime));
}

void RequireRefused(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::uint8_t* texels, std::uint32_t swizzle, const std::array<std::uint32_t, 4>& extra, const std::string& reason, const std::string& what, std::uint32_t format = UnormFormat) {
    std::string refusal;
    try {
        Run(device, code, TextureDescriptor(texels, format, swizzle, LoadWidth), extra, 1);
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    Require(refusal.find(reason) != std::string::npos, what + " of a converted image was not refused: " + refusal);
}

void CheckPacked16(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::uint32_t format) {
    const std::array<std::uint32_t, 4> widths = format == 133u ? std::array{5u, 6u, 5u, 0u} : format == 134u ? std::array{5u, 5u, 5u, 1u} : std::array{4u, 4u, 4u, 4u};
    const std::array<std::uint32_t, 4> offsets = format == 133u ? std::array{0u, 5u, 11u, 0u} : format == 134u ? std::array{0u, 5u, 10u, 15u} : std::array{0u, 4u, 8u, 12u};
    for (std::uint32_t index = 0; index < LoadWidth; ++index) {
        const auto value = static_cast<std::uint16_t>(index * 4051u + 0x39d7u);
        std::memcpy(texels + index * 2u, &value, 2u);
    }
    Run(device, LoadXyzw, TextureDescriptor(texels, format, SwizzleXYZW, LoadWidth), {}, LoadGroups);
    for (std::uint32_t index = 0; index < LoadWidth; ++index) {
        const auto value = static_cast<std::uint16_t>(index * 4051u + 0x39d7u);
        for (std::uint32_t component = 0; component < 4u; ++component) {
            const auto bits = widths[component] == 0u ? widths[0] : widths[component];
            const auto maximum = (1u << bits) - 1u;
            const auto actual = Output[index * 4u + component];
            Require(std::isfinite(std::bit_cast<float>(actual)) && StoreField(actual, maximum) == ((value >> offsets[component]) & maximum), "packed image_load has the wrong channel for format " + std::to_string(format));
        }
    }
    auto* target = texels + RoundTripOffset;
    alignas(256) static constexpr std::array<std::uint32_t, 10> roundTrip{
        0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x7e3e0280u, 0xf0001f08u, 0x00010a1eu, 0xbf8c3f70u, 0xf0201f08u,
        0x00040a1eu, 0xbf810000u,
    };
    Run(device, roundTrip, TextureDescriptor(texels, format, SwizzleXYZW, LoadWidth), {}, LoadGroups, TextureDescriptor(target, format, SwizzleXYZW, LoadWidth));
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(target), LoadWidth * 2u, nullptr, "test");
    device.WaitIdle();
    Require(std::memcmp(texels, target, LoadWidth * 2u) == 0, "packed image load/store changed guest bytes for format " + std::to_string(format));
    alignas(256) static constexpr auto store = StoreCode(0xfu, 0u);
    auto* written = texels + StoreOffset;
    alignas(256) static constexpr auto storeHalf = StoreCode(0xfu, D16);
    constexpr std::array halfValues{0x0000u, 0x3c00u, 0x3800u, 0x3400u, 0xbc00u, 0x4000u, 0x7c00u, 0x7e00u};
    constexpr std::array halfFloats{0.0f, 1.0f, 0.5f, 0.25f, -1.0f, 2.0f, INFINITY, NAN};
    for (std::uint32_t pass = 0; pass < 5u; ++pass) {
        const auto swizzle = pass == 3u ? SwizzleZYX1 : SwizzleXYZW;
        const auto dmask = pass == 4u ? 0x5u : 0xfu;
        std::array<std::uint16_t, Threads> expected{};
        Input.fill(0u);
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            std::uint32_t slot = 0;
            for (std::uint32_t component = 0; component < 4u; ++component) {
                if (((dmask >> component) & 1u) == 0u) continue;
                auto value = StoreValues[(tid + component * 3u + pass * 4u) % StoreValues.size()];
                if (tid == 0u) value = std::bit_cast<std::uint32_t>(pass == 0u ? std::array{1.0f, 0.5f, 0.25f, 1.0f}[component] : std::array{0.25f, 1.0f, 0.5f, 0.0f}[component]);
                if (pass == 2u) {
                    const auto index = (tid + component * 3u) % halfValues.size();
                    value = std::bit_cast<std::uint32_t>(halfFloats[index]);
                    Input[tid * 4u + slot / 2u] |= halfValues[index] << ((slot % 2u) * 16u);
                } else {
                    Input[tid * 4u + slot] = value;
                }
                ++slot;
                const auto selected = (swizzle >> (component * 3u)) & 7u;
                if (selected >= 4u && widths[selected - 4u] != 0u) expected[tid] |= static_cast<std::uint16_t>(StoreField(value, (1u << widths[selected - 4u]) - 1u) << offsets[selected - 4u]);
            }
        }
        auto* reference = texels + FloatOffset;
        std::memcpy(reference, expected.data(), Threads * 2u);
        Run(device, SampleLz, TextureDescriptor(reference, format, SwizzleXYZW, Threads), PointSampler, 1);
        std::array<std::uint32_t, 4> sampled{};
        std::copy_n(Output.begin(), 4u, sampled.begin());
        const auto descriptor = TextureDescriptor(written, format, SwizzleXYZW, Threads);
        Run(device, pass == 2u ? storeHalf : pass == 4u ? StoreXz : store, TextureDescriptor(written, format, swizzle, Threads), BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u)), 1);
        Run(device, SampleLz, descriptor, PointSampler, 1);
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            for (std::uint32_t component = 0; component < 4u; ++component) Require(Output[tid * 4u + component] == sampled[component], "sample after packed store differs for format " + std::to_string(format));
        }
        AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(written), Threads * 2u, nullptr, "test");
        device.WaitIdle();
        Require(std::memcmp(written, expected.data(), Threads * 2u) == 0, "packed image_store write-back differs for format " + std::to_string(format));
    }
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock block;
        auto* texels = block.Data();
        if (argc == 3 && std::string(argv[1]) == "--packed16") {
            const auto format = static_cast<std::uint32_t>(std::stoul(argv[2]));
            Require(format == 133u || format == 134u || format == 136u, "unsupported packed storage test format");
            CheckPacked16(*device, texels, format);
            std::cout << "Packed 16-bit storage image tests passed\n";
            return 0;
        }
        for (std::uint32_t index = 0; index < LoadWidth; ++index) {
            const auto texel = LoadTexel(index);
            std::memcpy(texels + index * 4u, &texel, 4u);
        }
        CheckLoad(*device, texels, LoadXyzw, 0xfu, UnormFormat, SwizzleXYZ1, "image_load dmask:0xf X Y Z 1");
        CheckLoad(*device, texels, LoadXy, 0x3u, UnormFormat, SwizzleXZY1, "image_load dmask:0x3 X Z Y 1");
        CheckLoad(*device, texels, LoadXyzw, 0xfu, UnormFormat, SwizzleZYX1, "image_load dmask:0xf Z Y X 1");
        CheckLoad(*device, texels, LoadXyzw, 0xfu, UnormFormat, SwizzleX011, "image_load dmask:0xf X 0 1 1");
        CheckLoad(*device, texels, LoadXyz, 0x7u, UintFormat, SwizzleYXZ1, "image_load of 10_11_11_UINT dmask:0x7 Y X Z 1");
        CheckRoundTrip(*device, texels);
        CheckStore(*device, texels + StoreOffset, StoreXyz, 0x7u, "image_store dmask:0x7");
        CheckStore(*device, texels + StoreOffset, StoreXz, 0x5u, "image_store dmask:0x5");
        CheckFloatStore(*device, texels + FloatOffset);
        Run(*device, StoreXyz, TextureDescriptor(texels + FloatOffset, FloatFormat, SwizzleXYZ1, Threads), BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u)), 1);
        CheckFloatSample(*device, texels + FloatOffset, {0u, 0u, 0x7fc00000u, 0x3f800000u}, "image_sample_lz of R10_G11_B11_FLOAT results still on the GPU");
        CheckFloatLoad(*device, texels + FloatOffset);
        CheckFloatSample(*device, texels + FloatOffset, {0x7f800000u, 0x7f800000u, 0x387c0000u, 0x3f800000u}, "image_sample_lz of R10_G11_B11_FLOAT");
        CheckSample(*device, texels + FloatOffset, SampleFloat, SampleStep, FloatStart, FloatFormat, SwizzleXYZ1, BilinearSampler, true, false, "bilinear image_sample_lz of R10_G11_B11_FLOAT");
        CheckSample(*device, texels, SampleAt, SampleStep, SampleStart, UnormFormat, SwizzleXYZ1, PointSampler, false, false, "point image_sample_lz X Y Z 1");
        CheckSample(*device, texels, SampleAt, SampleStep, SampleStart, UnormFormat, SwizzleZYX1, BilinearSampler, true, false, "bilinear image_sample_lz Z Y X 1");
        CheckSample(*device, texels, SampleWrapped, SampleStep, WrappedStart, UnormFormat, SwizzleX011, WrapPointSampler, false, true, "wrapped point image_sample_lz X 0 1 1");
        CheckSample(*device, texels, SampleWrapped, SampleStep, WrappedStart, UnormFormat, SwizzleXYZ1, WrapBilinearSampler, true, true, "wrapped bilinear image_sample_lz X Y Z 1");
        CheckSample(*device, texels + FloatOffset, SampleFloatOffGrid, OffGridStep, FloatStart, FloatFormat, SwizzleXYZ1, BilinearSampler, true, false, "off-grid bilinear image_sample_lz of R10_G11_B11_FLOAT");
        CheckSample(*device, texels, SampleOffGrid, OffGridStep, SampleStart, UnormFormat, SwizzleZYX1, BilinearSampler, true, false, "off-grid bilinear image_sample_lz Z Y X 1");
        CheckSample(*device, texels, SampleWrappedOffGrid, OffGridStep, WrappedStart, UnormFormat, SwizzleXYZ1, WrapBilinearSampler, true, true, "off-grid wrapped bilinear image_sample_lz X Y Z 1");
        CheckConvertedPreparation(*device, texels);
        for (const auto format : {UnormFormat, FloatFormat}) {
            CheckVolume(*device, format);
            GuestBlock mirrorBlock;
            for (std::uint32_t index = 0; index < LoadWidth; ++index) {
                const auto value = format == FloatFormat ? FloatTexel(index) : LoadTexel(index);
                std::memcpy(mirrorBlock.Data() + index * 4u, &value, sizeof(value));
            }
            CheckUnnormalizedSample(*device, format, false);
            CheckUnnormalizedSample(*device, format, true);
            alignas(256) constexpr auto mirrored = SampleAtCode(0x3e801000u, 0xc0400000u);
            for (const bool linear : {false, true}) {
                auto sampler = linear ? BilinearSampler : PointSampler;
                sampler[0] = 9u;
                CheckSample(*device, mirrorBlock.Data(), mirrored, 0x3e801000u, 0xc0400000u, format, SwizzleZYX1, sampler, linear, false, "mirrored converted sample", true);
            }
            for (const auto type : {Type2D, 13u, 11u, 10u}) {
                CheckMipCount(*device, format, type);
                CheckMipSample(*device, format, type, false);
                CheckMipSample(*device, format, type, true);
            }
            CheckCube(*device, format, false, true);
            CheckCube(*device, format, true, true);
            CheckCube(*device, format, true, false);
        }
        RequireRefused(*device, SampleLz, texels, SwizzleXYZ1, BorderSampler, "implemented only with wrap, mirrored-repeat or clamp-to-edge addressing", "image_sample_lz with clamp-to-border addressing");
        RequireRefused(*device, SampleLz, texels, SwizzleXYZ1, {0x8036u, 0x00fff000u, 0x00500000u, 0xc0000000u}, "border color table is not implemented", "unnormalized custom border sampling");
        RequireRefused(*device, SampleLz, texels, SwizzleXYZ1, {0x8036u, 0x00fff000u, 0u, 0u}, "point sampling of a converted image with clamp-to-border", "unnormalized point border sampling");
        RequireRefused(*device, SampleLz, texels, SwizzleXYZ1, {0x8092u, 0x00fff000u, 0x04000000u, 0u}, "requires point or bilinear filters and disabled mip filtering", "unnormalized mip sampling");
        RequireRefused(*device, Gather4Lz, texels, SwizzleXYZ1, PointSampler, "sampling, gathering or querying LOD of a converted unorm image", "image_gather4_lz");
        RequireRefused(*device, GetLod, texels, SwizzleXYZ1, PointSampler, "sampling, gathering or querying LOD of a converted unorm image", "image_get_lod");
        RequireRefused(*device, LoadD16, texels, SwizzleXYZ1, {}, "converted unorm image with 16-bit data", "image_load d16");
        RequireRefused(*device, StoreD16, texels + StoreOffset, SwizzleXYZ1, BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u)), "converted unorm image with 16-bit data", "image_store d16");
        RequireRefused(*device, LoadXyzw, texels, SwizzleXYZW, {}, "selects a channel the converted image format does not have", "image_load with DST_SEL X Y Z W");
        RequireRefused(*device, StoreXyz, texels + StoreOffset, SwizzleXYZW, BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u)), "selects a channel the converted image format does not have", "image_store with DST_SEL X Y Z W");
        std::puts("image converted unorm tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
