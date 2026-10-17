#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    const auto result = _putenv_s(name, value);
#else
    const auto result = setenv(name, value, 1);
#endif
    if (result != 0) throw std::runtime_error("cannot set " + std::string(name));
}

int Usage(int result) {
    std::fprintf(result == 0 ? stdout : stderr,
                 "usage: agc_pipeline_warmup <shader-cache-directory> [--threads N] [--entries N]\n"
                 "  builds the current build's recorded pipelines without running the game\n"
                 "  run with the game stopped; the GPU and driver must match the journal\n"
                 "  thread and entry defaults follow APS5_PIPELINE_JOURNAL_THREADS and APS5_PIPELINE_JOURNAL_ENTRIES\n");
    return result;
}

}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") return Usage(0);
    if (argc < 2 || std::string_view(argv[1]).starts_with("--")) return Usage(2);
    try {
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::is_directory(directory)) throw std::runtime_error("shader cache directory does not exist: " + directory.string());
        SetEnvironment("ANYPS5_SHADER_CACHE_DIR", directory.string().c_str());
        bool threadsSet = false;
        bool entriesSet = false;
        for (int index = 2; index < argc; ++index) {
            const std::string_view option(argv[index]);
            const bool threads = option == "--threads";
            if (!threads && option != "--entries") return Usage(2);
            auto& seen = threads ? threadsSet : entriesSet;
            if (seen || ++index == argc) return Usage(2);
            seen = true;
            const std::string_view value(argv[index]);
            unsigned count = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count == 0u) return Usage(2);
            SetEnvironment(threads ? "APS5_PIPELINE_JOURNAL_THREADS" : "APS5_PIPELINE_JOURNAL_ENTRIES", argv[index]);
        }
        if (!AgcDriver::Graphics::PipelineJournalEnabled()) throw std::runtime_error("pipeline journal or shader cache is disabled");
        const auto journal = AgcDriver::Graphics::PipelineJournalPath();
        if (!std::filesystem::is_regular_file(journal)) throw std::runtime_error("no pipeline journal for this build: " + journal.string());
        std::printf("Warming pipelines from %s\n", journal.string().c_str());
        {
            AgcDriver::VulkanDevice device(AgcDriver::VulkanDevice::PipelineWarmup{});
            device.WaitForPipelineJournalReplay();
        }
        const auto totals = AgcDriver::Graphics::PipelineJournalTotals();
        std::printf("Journal warm-up: %u threads, %llu graphics and %llu compute pipelines built; %llu present, %llu skipped, %llu rejected, %llu failed, %llu unreadable records\n",
                    totals.replayThreads, static_cast<unsigned long long>(totals.replayedGraphics), static_cast<unsigned long long>(totals.replayedCompute),
                    static_cast<unsigned long long>(totals.replayPresent), static_cast<unsigned long long>(totals.replaySkipped),
                    static_cast<unsigned long long>(totals.replayRejected), static_cast<unsigned long long>(totals.replayFailed), static_cast<unsigned long long>(totals.droppedRecords));
        return totals.loadedRecipes == 0u || totals.replayThreads == 0u || totals.replaySkipped != 0u || totals.replayRejected != 0u || totals.replayFailed != 0u || totals.droppedRecords != 0u ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_pipeline_warmup: %s\n", error.what());
        return 1;
    }
}
