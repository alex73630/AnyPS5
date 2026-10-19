#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

int main(int argc, char** argv) {
    try {
        using namespace AgcDriver;
        using DriverDetail::DrawPipeline;
        using DriverDetail::DrawFinisher;
        if (DrawPipeline::Depth() == 0) return 77;
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        const bool disabled = mode == "disabled" || mode == "finisher-disabled";
        const bool finisher = mode.starts_with("finisher");
        const char* worker = finisher ? "draw finisher" : "draw committer";
        if (finisher && !DrawFinisher::Enabled()) throw std::runtime_error("draw finisher is disabled");
        if (PerformanceTrace::Configured() == disabled) throw std::runtime_error("unexpected trace configuration");
        if (!disabled) PerformanceTrace::Present(1, 1, nullptr);
        bool checked = false;
        const char* failure = nullptr;
        const auto checkWorker = [&] {
            auto* trace = PerformanceTrace::Thread();
            if (disabled) {
                if (trace != nullptr) { failure = "unconfigured draw worker allocated a trace buffer"; return; }
                checked = true;
                return;
            }
            if (trace == nullptr) { failure = "draw worker CPU spans are absent from the trace"; return; }
            {
                PerformanceTimer timer("DrawPipeline.TraceRegression", true);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                timer.Finish();
                timer.Finish();
            }
            for (int index = 0; index != 8; ++index) {
                PerformanceTimer timer("DrawPipeline.ShortRegression", true);
            }
            { PerformanceTimer timer("DrawPipeline.DefaultRegression"); }
            const auto snapshot = trace->Collect();
            if (snapshot.name != worker || snapshot.dropped) { failure = "draw worker trace identity or capacity is incorrect"; return; }
            const auto found = [](const auto& event) { return event.scope != nullptr && std::string_view(event.scope) == "DrawPipeline.TraceRegression" && event.stage == nullptr && event.length > std::chrono::steady_clock::duration::zero(); };
            if (std::none_of(snapshot.slices.begin(), snapshot.slices.end(), found) || std::none_of(snapshot.totals.begin(), snapshot.totals.end(), found))
                { failure = "draw work was missing from trace slices or totals"; return; }
            const auto measured = std::find_if(snapshot.counters.begin(), snapshot.counters.end(), [](const auto& counter) { return std::string_view(counter.scope) == "DrawPipeline.TraceRegression" && counter.reason == nullptr && counter.timed; });
            if (measured == snapshot.counters.end() || measured->count != 1 || measured->length <= std::chrono::steady_clock::duration::zero())
                { failure = "the measured timer lost its duration or counted a repeated finish"; return; }
            const auto shortTimers = std::find_if(snapshot.counters.begin(), snapshot.counters.end(), [](const auto& counter) { return std::string_view(counter.scope) == "DrawPipeline.ShortRegression" && counter.timed; });
            if (shortTimers == snapshot.counters.end() || shortTimers->count != 8)
                { failure = "short timers were absent from per-frame measurements"; return; }
            if (std::any_of(snapshot.counters.begin(), snapshot.counters.end(), [](const auto& counter) { return std::string_view(counter.scope) == "DrawPipeline.DefaultRegression"; }))
                { failure = "an ordinary timer unexpectedly allocated a per-frame measurement"; return; }
            checked = true;
        };
        if (finisher) {
            DrawFinisher::Get().Enqueue(checkWorker);
            DrawFinisher::Get().Drain();
        } else {
            DrawPipeline::Queue0().Enqueue(checkWorker, {});
            DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        }
        if (failure != nullptr) throw std::runtime_error(failure);
        if (!checked) throw std::runtime_error("draw worker did not run the trace check");
        std::cout << worker << " tracing checked\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
