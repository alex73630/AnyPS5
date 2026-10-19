#include "IntermediateRepresentation/IrBuilder.hpp"
#include "IntermediateRepresentation/IrMetadata/StageIO.hpp"
#include "Optimization/ConstantFolder.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include <cstdio>
#include <pthread.h>
#include <string>
#include <stdexcept>

using namespace ShaderRecompiler;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckDiamond(unsigned mode) {
    IrProgram program;
    for (std::uint32_t id = 0; id < 4u; ++id) {
        auto& block = program.CreateBlock();
        program.BlockOrder().push_back(&block);
        BlockInfo info;
        info.id = id;
        info.terminator.kind = TerminatorKind::Return;
        program.Metadata().blockInfo.push_back(info);
    }
    auto& entry = *program.BlockOrder()[0];
    auto& left = *program.BlockOrder()[1];
    auto& right = *program.BlockOrder()[2];
    auto& merge = *program.BlockOrder()[3];
    program.SetEntryBlock(entry);
    entry.AddBranch(&left);
    entry.AddBranch(&right);
    left.AddBranch(&merge);
    right.AddBranch(&merge);
    auto& entryInfo = program.Metadata().blockInfo[0];
    entryInfo.terminator.kind = TerminatorKind::ConditionalBranch;
    entryInfo.terminator.trueBlock = 1u;
    entryInfo.terminator.falseBlock = 2u;
    for (std::uint32_t id = 1; id < 3u; ++id) {
        program.Metadata().blockInfo[id].terminator.kind = TerminatorKind::Branch;
        program.Metadata().blockInfo[id].terminator.trueBlock = 3u;
    }
    IrBuilder builder(program);
    builder.SetInsertionPoint(entry);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& limit = builder.Constant(16u);
    auto& condition = builder.Emit(IrOpcode::ULessThan32, IrType::Bool, {&lane, &limit});
    entryInfo.condition = &condition;
    auto& one = builder.Constant(1u);
    auto& two = builder.Constant(2u);
    IrValue* first = &lane;
    IrValue* second = &lane;
    if (mode == 0u || mode == 1u) {
        builder.SetInsertionPoint(left);
        first = mode == 0u ? &builder.Emit(IrOpcode::SelectU32, IrType::U32, {&condition, &one, &two}) : &builder.Emit(IrOpcode::LaneId, IrType::U32, {});
        builder.SetInsertionPoint(right);
        second = mode == 0u ? &builder.Emit(IrOpcode::SelectU32, IrType::U32, {&condition, &one, &two}) : &builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    } else if (mode == 3u || mode == 4u) {
        first = &one;
        second = &program.CreateValue(IrOpcode::Void, IrType::U32);
        second->SetImmediateU32(mode == 3u ? 1u : 2u);
    }
    builder.SetInsertionPoint(merge);
    auto& phi = builder.Emit(IrOpcode::Phi, IrType::U32, {});
    phi.AddPhiOperand(&left, first);
    phi.AddPhiOperand(&right, second);
    auto& reference = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&phi});
    ValidateProgram(program, true);
    ConstantFolder{}.Fold(program);
    DeadCodeEliminator{}.RemoveIdentities(program);
    DeadCodeEliminator{}.Eliminate(program);
    ValidateProgram(program, true);
    const auto* result = reference.Argument(0)->Resolve();
    if (mode == 0u || mode == 1u || mode == 4u) {
        Require(result == &phi && phi.Argument(0) == first && phi.Argument(1) == second, "phi lost distinct branch definitions or constants");
    } else {
        Require(result == first, "phi did not fold identical definitions or equal constants");
    }
}

void CheckUnobservedPhiCycles() {
    IrProgram empty;
    DeadCodeEliminator{}.EliminateUnobservedPhis(empty);
    IrProgram program;
    for (std::uint32_t id = 0; id < 4u; ++id) {
        auto& block = program.CreateBlock();
        program.BlockOrder().push_back(&block);
        BlockInfo info;
        info.id = id;
        info.terminator.kind = TerminatorKind::Return;
        program.Metadata().blockInfo.push_back(info);
    }
    auto& entry = *program.BlockOrder()[0];
    auto& header = *program.BlockOrder()[1];
    auto& latch = *program.BlockOrder()[2];
    auto& exit = *program.BlockOrder()[3];
    program.SetEntryBlock(entry);
    entry.AddBranch(&header);
    header.AddBranch(&latch);
    header.AddBranch(&exit);
    latch.AddBranch(&header);
    program.Metadata().blockInfo[0].terminator.kind = TerminatorKind::Branch;
    program.Metadata().blockInfo[0].terminator.trueBlock = 1u;
    auto& headerInfo = program.Metadata().blockInfo[1];
    headerInfo.terminator.kind = TerminatorKind::ConditionalBranch;
    headerInfo.terminator.trueBlock = 2u;
    headerInfo.terminator.falseBlock = 3u;
    program.Metadata().blockInfo[2].terminator.kind = TerminatorKind::Branch;
    program.Metadata().blockInfo[2].terminator.trueBlock = 1u;
    IrBuilder builder(program);
    builder.SetInsertionPoint(entry);
    auto& zero = builder.Constant(0u);
    auto& producer = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    const auto phi = [&](IrBlock& block, IrType type = IrType::U32) -> IrValue& {
        auto& value = program.CreateValue(IrOpcode::Phi, type);
        block.AppendInstruction(&value);
        return value;
    };
    auto& first = phi(header);
    auto& second = phi(latch);
    auto& tail = phi(header);
    auto& self = phi(header);
    auto& liveFirst = phi(header);
    auto& liveSecond = phi(latch);
    auto& condition = phi(header, IrType::Bool);
    first.AddPhiOperand(&entry, &producer);
    first.AddPhiOperand(&latch, &second);
    second.AddPhiOperand(&header, &first);
    tail.AddPhiOperand(&entry, &zero);
    tail.AddPhiOperand(&latch, &second);
    self.AddPhiOperand(&entry, &zero);
    self.AddPhiOperand(&latch, &self);
    liveFirst.AddPhiOperand(&entry, &zero);
    liveFirst.AddPhiOperand(&latch, &liveSecond);
    liveSecond.AddPhiOperand(&header, &liveFirst);
    condition.AddPhiOperand(&entry, &builder.ConstantBool(true));
    condition.AddPhiOperand(&latch, &condition);
    headerInfo.condition = &condition;
    builder.SetInsertionPoint(header);
    static_cast<void>(builder.Emit(IrOpcode::Reference, IrType::Void, {&condition}));
    builder.SetInsertionPoint(latch);
    auto& store = builder.Emit(IrOpcode::StoreVectorWindow, IrType::Void, {&zero, &liveSecond});
    ValidateProgram(program, true);
    DeadCodeEliminator{}.EliminateUnobservedPhis(program);
    ValidateProgram(program, true);
    Require(first.Parent() == nullptr && second.Parent() == nullptr && tail.Parent() == nullptr && self.Parent() == nullptr, "phi cycles: an unobserved cycle or its tail survived");
    Require(!first.HasUses() && !second.HasUses() && !tail.HasUses() && !self.HasUses(), "phi cycles: a removed cycle retained uses");
    Require(liveFirst.Parent() == &header && liveSecond.Parent() == &latch && liveFirst.Argument(1) == &liveSecond && liveSecond.Argument(0) == &liveFirst, "phi cycles: an observed cycle was changed");
    Require(store.Parent() == &latch && store.Argument(1) == &liveSecond, "phi cycles: an observable store was changed");
    Require(condition.Parent() == &header && headerInfo.condition == &condition && condition.Argument(1) == &condition, "phi cycles: a branch condition was changed");
    DeadCodeEliminator{}.Eliminate(program);
    Require(producer.Parent() == nullptr && !producer.HasUses(), "phi cycles: ordinary DCE retained a released producer");
    DeadCodeEliminator{}.EliminateUnobservedPhis(program);
    ValidateProgram(program, true);
    Require(entry.Instructions().empty() && header.Instructions().size() == 3u && latch.Instructions().size() == 2u, "phi cycles: repeated elimination changed the live graph");
}

void CheckLongAncillaryForwarding() {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.BlockOrder().push_back(&block);
    BlockInfo info;
    info.id = 0u;
    info.terminator.kind = TerminatorKind::Return;
    program.Metadata().blockInfo.push_back(info);
    program.SetEntryBlock(block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& ancillary = builder.Emit(IrOpcode::GetBuiltin, IrType::U32, {&builder.Constant(static_cast<std::uint32_t>(StageInputKind::PackedAncillary)), &builder.Constant(0u)});
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& condition = builder.Emit(IrOpcode::ULessThan32, IrType::Bool, {&lane, &builder.Constant(16u)});
    auto& zero = builder.Constant(0u);
    IrValue* forwarded = &ancillary;
    for (std::uint32_t link = 0; link < 20000u; ++link) forwarded = &builder.Emit(IrOpcode::SelectU32, IrType::U32, {&condition, &zero, forwarded});
    auto& field = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {forwarded, &builder.Constant(8u), &builder.Constant(4u)});
    builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&field});
    struct Run {
        IrProgram& program;
        std::string error;
    } run{program, {}};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256u * 1024u);
    pthread_t thread;
    const auto body = +[](void* argument) -> void* {
        auto& state = *static_cast<Run*>(argument);
        try {
            ConstantFolder{}.Fold(state.program);
        } catch (const std::exception& error) {
            state.error = error.what();
        }
        return nullptr;
    };
    Require(pthread_create(&thread, &attributes, body, &run) == 0, "long ancillary forwarding: cannot start the folding thread");
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
    Require(run.error.empty(), "long ancillary forwarding: folding failed");
}

void CheckDeepAncillary(bool whole, bool iteratesSamples) {
    IrProgram program;
    program.Resources().stage = IrShaderStage::Pixel;
    program.Metadata().pixelIteratesSamples = iteratesSamples;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& ancillary = builder.Emit(IrOpcode::GetBuiltin, IrType::U32, {&builder.Constant(static_cast<std::uint32_t>(StageInputKind::PackedAncillary)), &builder.Constant(0u)});
    auto& uses = program.CreateBlock();
    program.BlockOrder().push_back(&uses);
    builder.SetInsertionPoint(uses);
    const auto identity = [&](IrValue& source) -> IrValue& {
        auto& value = program.CreateValue(IrOpcode::Identity, IrType::U32);
        value.AddArgument(&source);
        uses.AppendInstruction(&value);
        return value;
    };
    auto& first = identity(ancillary);
    auto& condition = builder.Emit(IrOpcode::UndefU1, IrType::Bool, {});
    IrValue* tail = &first;
    for (std::uint32_t index = 0; index < 50000u; ++index) {
        tail = whole ? &builder.Emit(IrOpcode::SelectU32, IrType::U32, {&condition, tail, tail}) : &identity(*tail);
    }
    auto& extract = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {tail, &builder.Constant(8u), &builder.Constant(4u)});
    if (whole) {
        auto& phi = builder.Emit(IrOpcode::Phi, IrType::U32, {});
        phi.AddPhiOperand(&block, tail);
        first.ReplaceArgument(0u, &phi);
        phi.AddPhiOperand(&block, &ancillary);
    } else {
        static_cast<void>(builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {tail}));
    }
    IrBlock* selected = &block;
    ConstantFolder{}.Fold(program, std::span<IrBlock* const>(&selected, 1u));
    if (whole) {
        Require(ancillary.OperandUses().empty(), "deep ancillary forwarding retained the packed input");
        Require(extract.Argument(0) == tail && extract.Argument(1)->ImmediateU32() == 8u, "deep ancillary forwarding changed a downstream field extraction");
    } else {
        const auto* sample = extract.Argument(0)->Resolve();
        if (iteratesSamples) Require(sample->Opcode() == IrOpcode::GetBuiltin && sample->Argument(0)->ImmediateU32() == static_cast<std::uint32_t>(StageInputKind::SampleId), "deep ancillary direct extraction lost its sample ID");
        else Require(sample->HasImmediate() && sample->ImmediateU32() == 0u, "a once-per-pixel ancillary extraction retained a sample ID");
        Require(extract.Argument(1)->ImmediateU32() == 0u && first.Argument(0) == &ancillary, "deep ancillary direct extraction changed a whole-word reader");
    }
}

}

int main() {
    try {
        for (unsigned mode = 0; mode < 5u; ++mode) CheckDiamond(mode);
        CheckLongAncillaryForwarding();
        CheckUnobservedPhiCycles();
        for (const bool iteratesSamples : {false, true}) {
            CheckDeepAncillary(true, iteratesSamples);
            CheckDeepAncillary(false, iteratesSamples);
        }
        std::puts("constant phi dominance tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
