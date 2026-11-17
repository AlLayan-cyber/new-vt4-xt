// Halden VT-4 — diagnostics log.
//
// Writes what the plugin does and how long it takes to
//   Documents/Halden VT-4/diagnostics.txt
// so that a hang or a dropout on a particular machine can be traced:
// lifecycle calls from the host (prepare, release, destruction), every
// background build, the audio thread's load per block (maximum, average,
// overruns of the block's time budget) and stalls of the message thread.
//
// The audio thread only writes atomics; a low-priority logger thread
// collects them once a second and appends to the file.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

namespace vt4diag
{

// one line to the log (any thread except the audio thread)
void log (const juce::String& line);

// lowers the priority of the calling worker thread (background builds)
void lowerCurrentThreadPriority();

// milliseconds on a monotonic clock
double nowMs() noexcept;

// Per plugin instance: written by the audio thread, read by the logger.
struct AudioStats
{
    std::atomic<int> blocks { 0 }, overruns { 0 }, samples { 0 }, windows { 0 };
    double winSecs = 0.0;      // audio thread only
    int winSamples = 0;
    std::atomic<float> maxLoad { 0.0f }, sumLoad { 0.0f };
    std::atomic<int> swaps { 0 };              // unit cross-fades started
    std::atomic<int> bypassChanges { 0 };
    std::atomic<bool> sleeping { false }, circuitRunning { false };
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<int> blockSize { 0 };

    // Audio thread, once per block: the time spent and the block's length.
    // The load is judged over windows of at least ~5 ms (hosts split blocks
    // at automation points; a 1-sample block says nothing about deadlines).
    void addBlock (int numSamples, double seconds, double rate) noexcept
    {
        blocks.fetch_add (1, std::memory_order_relaxed);
        samples.fetch_add (numSamples, std::memory_order_relaxed);
        winSecs += seconds; winSamples += numSamples;
        if ((double) winSamples < 0.005 * rate) return;
        const float load = (float) (winSecs * rate / (double) winSamples);
        winSecs = 0.0; winSamples = 0;
        windows.fetch_add (1, std::memory_order_relaxed);
        if (load > 1.0f) overruns.fetch_add (1, std::memory_order_relaxed);
        float m = maxLoad.load (std::memory_order_relaxed);
        while (load > m && ! maxLoad.compare_exchange_weak (m, load)) {}
        float s = sumLoad.load (std::memory_order_relaxed);
        while (! sumLoad.compare_exchange_weak (s, s + load)) {}
    }
};

// Registers an instance with the logger for its lifetime.
class Instance
{
public:
    Instance();
    ~Instance();
    int id() const noexcept { return number; }
    AudioStats stats;

    // the editor's timer marks the message thread alive
    void uiTick() noexcept { lastUiTick.store (nowMs(), std::memory_order_relaxed); }
    void uiOpen (bool open) noexcept { editorOpen.store (open); if (open) uiTick(); }

    std::atomic<double> lastUiTick { 0.0 };
    std::atomic<bool> editorOpen { false };
    double stallReported = 0.0;     // logger thread only

private:
    int number = 0;
};

} // namespace vt4diag
