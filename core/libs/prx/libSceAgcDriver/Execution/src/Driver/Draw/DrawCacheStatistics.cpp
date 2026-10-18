#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <cstdlib>
#include <bit>

namespace AgcDriver::DriverDetail {

bool Driver::traceDrawCache() {
    static const bool trace = std::getenv("APS5_TRACE_DRAW_CACHE") != nullptr;
    return trace;
}

void Driver::traceInsert(std::unordered_set<std::uint64_t>& set, std::uint64_t key) {
    if (set.size() >= (1u << 16u)) set.clear();
    set.insert(key);
}

void Driver::traceDataUserMismatch(const DrawProgram& current, const DrawProgram& previous, const std::vector<std::shared_ptr<DispatchVariant>>& variants) {
    std::lock_guard cacheLock(drawCacheMutex);
    auto& counters = drawEntryCounters;
    ++counters.dataUserRejected;
    counters.dataUserTailChanged += current.tailContinuation != previous.tailContinuation;
    if (current.userData.size() != previous.userData.size() || current.userData.size() > 64 || variants.empty() || variants.front() == nullptr || !variants.front()->tracedUserUse || variants.front()->compiled == nullptr) {
        ++counters.dataUserUnknown;
        return;
    }
    const auto& variant = *variants.front();
    std::uint64_t changed = 0, forwarded = 0;
    for (std::size_t word = 0; word < current.userData.size(); ++word) {
        if (current.userData[word] != previous.userData[word]) changed |= std::uint64_t{1} << word;
    }
    for (const auto word : variant.compiled->shaderDataUserWords) {
        if (word < 64) forwarded |= std::uint64_t{1} << word;
    }
    const auto used = variant.tracedUserBases | variant.tracedUserValues | forwarded;
    counters.dataUserEligible += variant.tracedRelocationEligible;
    counters.dataUserBaseWords += std::popcount(changed & variant.tracedUserBases);
    counters.dataUserValueWords += std::popcount(changed & variant.tracedUserValues);
    counters.dataUserForwardedWords += std::popcount(changed & forwarded);
    counters.dataUserUnusedWords += std::popcount(changed & ~used);
    counters.dataUserOnlyUnused += (changed & used) == 0;
    counters.dataUserOnlyBases += (changed & variant.tracedUserBases) != 0 && (changed & (~variant.tracedUserBases | variant.tracedUserValues)) == 0;
}

void Driver::maybeReportDrawCache(bool profile) {
    auto& counters = drawEntryCounters;
    if (!profile && !traceDrawCache()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.lastReport < std::chrono::seconds(10)) return;
    reportDrawCache(counters, profile);
    if (drawRelocationEnabled()) reportDrawRelocation();
    counters.lastReport = now;
}

void Driver::reportDrawCache(DrawEntryCounters& counters, bool profile) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string text;
    if (traceDrawCache()) {
        char line[1536];
        std::uint64_t misses = 0;
        for (const auto value : counters.misses) misses += value;
        std::snprintf(line, sizeof(line), "[draw-cache] 10 s: %llu lookups, %llu hits (%.1f%%), %llu stage misses, %llu without an entry, %llu data-only hits (stages: %llu reused, %llu compiled; %llu verified); no entry: new registers %llu, user words/pointers only %llu, evicted %llu, seen but never inserted %llu; stage misses: front differing %llu, fragment differing %llu, other differing %llu, layout %llu, gate %llu, stage count %llu; differing captures: same runs %llu (%.1f words changed each), other runs %llu; %zu entries, %llu evicted\n", count(counters.lookups), count(counters.hits), counters.lookups != 0 ? 100.0 * static_cast<double>(counters.hits) / static_cast<double>(counters.lookups) : 0.0, count(misses), count(counters.absent), count(counters.dataHits), count(counters.dataStagesReused), count(counters.dataStagesCompiled), count(counters.dataVerified), count(counters.absentNewRegisters), count(counters.absentUserWords), count(counters.absentEvicted), count(counters.absentNeverInserted), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.differingSameRuns), counters.differingSameRuns != 0 ? static_cast<double>(counters.differingWords) / static_cast<double>(counters.differingSameRuns) : 0.0, count(counters.differingRunsChanged), drawCache.size(), count(counters.evictions));
        text += line;
        std::snprintf(line, sizeof(line), "[draw-cache] data-only user-word rejections (10 s, front variant): %llu stages, %llu unknown, %llu tail changes, %llu relocation eligible; changed words (overlapping): bases %llu, values %llu, forwarded %llu, unused %llu; stages with only unused changes %llu, only base changes %llu\n", count(counters.dataUserRejected), count(counters.dataUserUnknown), count(counters.dataUserTailChanged), count(counters.dataUserEligible), count(counters.dataUserBaseWords), count(counters.dataUserValueWords), count(counters.dataUserForwardedWords), count(counters.dataUserUnusedWords), count(counters.dataUserOnlyUnused), count(counters.dataUserOnlyBases));
        text += line;
        std::snprintf(line, sizeof(line), "[draw-cache] register-key miss component changes (10 s): stable registers %llu, user data %llu, auxiliary registers %llu, device %llu, first shader set %llu, unchanged components %llu\n", count(counters.registerKeyMissChanges[0]), count(counters.registerKeyMissChanges[1]), count(counters.registerKeyMissChanges[2]), count(counters.registerKeyMissChanges[3]), count(counters.registerKeyMissChanges[4]), count(counters.registerKeyMissChanges[5]));
        text += line;
    }
    if (profile) text += profileDrawCacheLines(counters);
    AgcDriver::ProfilePrint_nid_no_patch("%s", text.c_str());
    counters = DrawEntryCounters{};
}

DrawEntryCounters Driver::DrawCacheCounters() {
    std::lock_guard cacheLock(drawCacheMutex);
    return drawEntryCounters;
}

std::string Driver::profileDrawCacheLines(const DrawEntryCounters& counters) {
    std::string text;
    const auto append = [&](const char* format, auto... values) {
        char line[2048];
        std::snprintf(line, sizeof(line), format, values...);
        text += line;
    };
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups > counters.absent ? counters.lookups - counters.absent : 0;
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char item[32];
        std::snprintf(item, sizeof(item), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += item;
    }
    append("[draw-cache] %llu lookups (10 s): %llu no entry, %llu validated in %.1f us each (key, lookup and every stage): %llu hits (every stage equal; stage hits by variant rank 1..k %s), misses by reason: front stage differing %llu, fragment differing %llu, other stage differing %llu, layout (push offset) %llu, gate %llu, stage count %llu; %llu stage validations (%llu equal, %.2f variants compared each); %llu inserts (%llu shape updates, %llu variants inserted, %llu evicted beyond k, %llu already present, %llu unstable), %zu entries (%llu variants, ~%.1f MiB), %llu evictions in total, %llu LRU moves; verify: %llu hits captured again, %llu stages differed\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.hits), ranks.c_str(), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.stageValidations), count(counters.stageEqual), counters.stageValidations != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(counters.stageValidations) : 0.0, count(counters.inserts), count(counters.shapeUpdates), count(counters.variantsInserted), count(counters.variantsEvicted), count(counters.present), count(counters.unstable), drawCache.size(), count(drawCacheVariants), static_cast<double>(drawCacheVariantBytes) / (1024.0 * 1024.0), count(drawCacheEvictions), count(counters.touches), count(counters.verifyHits), count(counters.verifyMismatches));
    append("[draw-cache] register key (10 s): %llu lookups, %llu hits, key build %.1f us each; decode skipped %llu, partial (state/pixel/programs from the entry) %llu; facade log != table %llu; verify: %llu decodes compared, %llu differed\n", count(counters.registerKeyLookups), count(counters.registerKeyHits), counters.registerKeyLookups != 0 ? counters.keyUs / static_cast<double>(counters.registerKeyLookups) : 0.0, count(counters.decodeSkipped), count(counters.decodePartial), count(counters.facadeMismatches), count(counters.verifyDecodes), count(counters.verifyDecodeMismatches));
    append("[draw-cache] stage data hits (10 s): %llu stages (%llu words refreshed), verified %llu; stage inserts with data positions %llu (%.1f positions each)\n", count(counters.stageDataHits), count(counters.stageDataWords), count(counters.stageDataVerified), count(counters.stageDataInserts), counters.stageDataInserts != 0 ? static_cast<double>(counters.stageDataPositions) / static_cast<double>(counters.stageDataInserts) : 0.0);
    return text;
}

}
