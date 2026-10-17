#include "SpirvBackend/SpirvEmitterHelpers.hpp"
#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include "SpirvBackend/SpirvBufferFormat.hpp"
#include "SpirvBackend/SpirvMemory/SpirvFormatConvert.hpp"
#include "RuntimeAbi.hpp"
#include "PipelineSpecialization.hpp"
#include <spirv/unified1/GLSL.std.450.h>
#include <stdexcept>

namespace ShaderRecompiler {
namespace {

std::uint32_t Arithmetic(SpirvEmitterState& state, spv::Op op, std::uint32_t a, std::uint32_t b) {
    const auto result = Binary(state, op, TypeF32(state), a, b);
    state.module.AddAnnotation(spv::OpDecorate, result, spv::DecorationNoContraction);
    return result;
}

std::uint32_t Factor(SpirvEmitterState& state, std::uint32_t factor, std::uint32_t source, std::uint32_t destination, std::uint32_t alpha, std::uint32_t channel) {
    const auto one = ConstantF32Value(state, 1.0f);
    const auto zero = ConstantF32Value(state, 0.0f);
    const auto complement = [&](auto value) { return Arithmetic(state, spv::OpFSub, one, value); };
    const auto& constants = state.inputInfo.pixel->packedBlend.constants;
    switch (factor) {
    case 0: case 7: return zero;
    case 1: case 6: return one;
    case 2: return source;
    case 3: return complement(source);
    case 4: return alpha;
    case 5: return complement(alpha);
    case 8: return destination;
    case 9: return complement(destination);
    case 13: return ConstantF32Value(state, constants[channel]);
    case 14: return complement(ConstantF32Value(state, constants[channel]));
    case 19: return ConstantF32Value(state, constants[3]);
    case 20: return complement(ConstantF32Value(state, constants[3]));
    default: throw std::runtime_error("unsupported packed float blend factor");
    }
}

}

void DefinePackedBlend(SpirvEmitterState& state) {
    if (state.program.Resources().stage != IrShaderStage::Pixel || state.inputInfo.pixel == nullptr || state.inputInfo.pixel->packedBlend.control == 0) return;
    state.packedBlendImageType = state.module.Type(spv::OpTypeImage, TypeU32(state), spv::Dim2D, 0u, 1u, 0u, 2u, spv::ImageFormatR32ui);
    state.packedBlendImage = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassUniformConstant, state.packedBlendImageType), spv::StorageClassUniformConstant);
    state.module.AddAnnotation(spv::OpDecorate, state.packedBlendImage, spv::DecorationDescriptorSet, state.fragmentDescriptorSet ? RuntimeAbi::FragmentDescriptorSet : RuntimeAbi::DescriptorSet);
    state.module.AddAnnotation(spv::OpDecorate, state.packedBlendImage, spv::DecorationBinding, RuntimeAbi::PackedBlendTargetBinding);
    state.module.AddAnnotation(spv::OpDecorate, state.packedBlendImage, spv::DecorationCoherent);
    state.packedBlendColor = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassPrivate, TypeF32Vector(state, 4u)), spv::StorageClassPrivate);
    state.packedBlendWritten = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassPrivate, TypeBool(state)), spv::StorageClassPrivate);
}

void InitializePackedBlend(SpirvEmitterState& state) {
    if (state.packedBlendWritten != 0) state.module.AddFunction(spv::OpStore, state.packedBlendWritten, ConstantBool(state, false));
}

void CommitPackedBlend(SpirvEmitterState& state) {
    if (state.packedBlendImage == 0) return;
    auto active = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeBool(state), active, state.packedBlendWritten);
    if (state.pixelValidMaskVariable != 0) {
        const auto mask = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeU32(state), mask, state.pixelValidMaskVariable);
        active = Binary(state, spv::OpLogicalAnd, TypeBool(state), active, EmitCompareU32Constant(state, spv::OpINotEqual, mask, 0u));
    }
    EmitIfCondition(state, active, [&] {
        const auto color = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeF32Vector(state, 4u), color, state.packedBlendColor);
        const auto alpha = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeExtract, TypeF32(state), alpha, color, 3u);
        const auto frag = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeF32Vector(state, 4u), frag, InputVariableForKind(state, StageInputKind::FragCoord));
        std::array<std::uint32_t, 3> coordinates{};
        for (std::uint32_t component = 0; component < 2u; ++component) {
            const auto value = state.module.AllocateId();
            state.module.AddFunction(spv::OpCompositeExtract, TypeF32(state), value, frag, component);
            coordinates[component] = Unary(state, spv::OpConvertFToS, TypeI32(state), value);
        }
        coordinates[2] = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeI32(state), coordinates[2], InputVariableForKind(state, StageInputKind::Layer));
        const auto exported = state.module.SpecializationConstant(TypeU32(state), PipelineSpecialization::LayerExported, 0u);
        coordinates[2] = Binary(state, spv::OpIMul, TypeI32(state), coordinates[2], Unary(state, spv::OpBitcast, TypeI32(state), exported));
        const auto position = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeConstruct, TypeI32Vector(state, 3u), position, coordinates[0], coordinates[1], coordinates[2]);
        const auto image = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, state.packedBlendImageType, image, state.packedBlendImage);
        const auto texel = state.module.AllocateId();
        state.module.AddFunction(spv::OpImageRead, TypeU32Vector(state, 4u), texel, image, position);
        const auto word = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), word, texel, 0u);
        const auto info = GetFormatInfo(IrBufferFormat::Format10_11_11Float);
        const auto control = state.inputInfo.pixel->packedBlend.control;
        auto packed = ConstantU32(state, 0u);
        for (std::uint32_t channel = 0; channel < 3u; ++channel) {
            const auto source = state.module.AllocateId();
            state.module.AddFunction(spv::OpCompositeExtract, TypeF32(state), source, color, channel);
            const auto field = state.module.AllocateId();
            state.module.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), field, word, ConstantU32(state, info.componentBitOffset[channel]), ConstantU32(state, info.componentBits[channel]));
            const auto destination = Unary(state, spv::OpBitcast, TypeF32(state), NormalizeFormatComponent(state, info, channel, field));
            const auto left = Arithmetic(state, spv::OpFMul, source, Factor(state, control & 31u, source, destination, alpha, channel));
            const auto right = Arithmetic(state, spv::OpFMul, destination, Factor(state, (control >> 8u) & 31u, source, destination, alpha, channel));
            std::uint32_t value = 0;
            switch ((control >> 5u) & 7u) {
            case 0: value = Arithmetic(state, spv::OpFAdd, left, right); break;
            case 1: value = Arithmetic(state, spv::OpFSub, left, right); break;
            case 4: value = Arithmetic(state, spv::OpFSub, right, left); break;
            default: throw std::runtime_error("packed float MIN/MAX blending is not implemented");
            }
            const auto encoded = EmitFormatStoreComponent(state, info, channel, Unary(state, spv::OpBitcast, TypeU32(state), value));
            packed = EmitOrU32(state, packed, EmitBinaryU32(state, spv::OpShiftLeftLogical, encoded, ConstantU32(state, info.componentBitOffset[channel])));
        }
        const auto output = state.module.AllocateId();
        const auto zero = ConstantU32(state, 0u);
        state.module.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 4u), output, packed, zero, zero, zero);
        state.module.AddFunction(spv::OpImageWrite, image, position, output);
    });
}

}
