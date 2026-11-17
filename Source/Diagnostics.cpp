#if defined (_WIN32)
 // two kernel32 functions (declared here rather than pulling all of
 // windows.h in next to JUCE's headers)
 extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
 extern "C" __declspec(dllimport) int __stdcall SetThreadPriority (void* thread, int priority);
#elif defined (__APPLE__)
 #include <pthread.h>
 #include <pthread/qos.h>
#endif

#include "Diagnostics.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace vt4diag
{

double nowMs() noexcept
{
    return juce::Time::getMillisecondCounterHiRes();
}

void lowerCurrentThreadPriority()
{
   #if defined (_WIN32)
    SetThreadPriority (GetCurrentThread(), -2);    // THREAD_PRIORITY_LOWEST
   #elif defined (__APPLE__)
    pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
   #endif
}

namespace
{
extern std::mutex gMutex;

class Logger : private juce::Thread
{
public:
    Logger() : juce::Thread ("Halden VT-4 log")
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Halden VT-4");
        dir.createDirectory();
        file = dir.getChildFile ("diagnostics.txt");
        if (file.getSize() > 2 * 1024 * 1024)
        {
            auto old = dir.getChildFile ("diagnostics-old.txt");
            old.deleteFile();
            file.moveFileTo (old);
        }
        juce::String head;
        head << "\n==== " << juce::Time::getCurrentTime().toString (true, true, true, true)
             << "  Halden VT-4 "
            #ifdef JucePlugin_VersionString
             << JucePlugin_VersionString
            #endif
             << "  host: " << juce::PluginHostType().getHostDescription()
             << "  OS: " << juce::SystemStats::getOperatingSystemName()
             << "  CPU: " << juce::SystemStats::getCpuModel() << " (" << juce::SystemStats::getNumCpus() << " threads)\n";
        file.appendText (head, false, false, "\n");
        startThread (juce::Thread::Priority::low);
    }

    ~Logger() override
    {
        stopThread (3000);
        flush();
    }

    void add (const juce::String& line)
    {
        juce::String s;
        s << stamp() << "  " << line << "\n";
        std::lock_guard<std::mutex> l (m);
        pending += s;
    }

    void enrol (Instance* i)  { std::lock_guard<std::mutex> l (m); instances.push_back (i); }
    void leave (Instance* i)
    {
        std::lock_guard<std::mutex> l (m);
        instances.erase (std::remove (instances.begin(), instances.end(), i), instances.end());
    }
    int nextId() { return ++counter; }

private:
    static juce::String stamp()
    {
        const auto t = juce::Time::getCurrentTime();
        return t.formatted ("%H:%M:%S") + "." + juce::String (t.getMilliseconds()).paddedLeft ('0', 3);
    }

    void run() override
    {
        int tick = 0;
        while (! threadShouldExit())
        {
            wait (500);
            ++tick;
            {
                std::lock_guard<std::mutex> l (gMutex);   // instances stay enrolled meanwhile
                poll (tick % 20 == 0);
            }
            flush();
        }
    }

    // audio statistics every second (or every 10 s when all is well), and
    // message-thread stalls while an editor is open
    void poll (bool summary)
    {
        std::lock_guard<std::mutex> l (m);
        const double now = nowMs();
        for (auto* i : instances)
        {
            auto& st = i->stats;
            if (i->editorOpen.load())
            {
                const double gap = now - i->lastUiTick.load();
                if (gap > 400.0 && gap > i->stallReported + 1000.0)
                {
                    pendingLine (i, "message thread (host GUI) not responding for " + juce::String ((int) gap) + " ms");
                    i->stallReported = gap;
                }
                else if (gap < 400.0)
                    i->stallReported = 0.0;
            }
            const int overruns = st.overruns.load();
            const float maxLoad = st.maxLoad.load();
            if (! summary && overruns == 0 && maxLoad < 0.8f) continue;
            const int blocks = st.blocks.exchange (0);
            st.overruns.exchange (0);
            st.maxLoad.exchange (0.0f);
            const float sum = st.sumLoad.exchange (0.0f);
            const int samples = st.samples.exchange (0);
            const int windows = st.windows.exchange (0);
            if (blocks == 0 || windows == 0) continue;
            juce::String s;
            s << "audio: " << blocks << " blocks (avg " << (samples / blocks) << " samples @ " << (int) st.sampleRate.load()
              << " Hz), load avg " << juce::String (100.0f * sum / (float) windows, 1) << " %, max " << juce::String (100.0f * maxLoad, 1)
              << " %, overruns " << overruns << " (load = compute time / real time, over ~5 ms windows)"
              << (st.sleeping.load() ? ", resting (bypassed)" : "") << (st.circuitRunning.load() ? "" : ", circuit not built yet");
            const int sw = st.swaps.exchange (0), bc = st.bypassChanges.exchange (0);
            if (sw > 0) s << ", unit cross-fades " << sw;
            if (bc > 0) s << ", bypass toggled " << bc << "x";
            pendingLine (i, s);
        }
    }

    void pendingLine (Instance* i, const juce::String& text)
    {
        pending << stamp() << "  [#" << i->id() << "] " << text << "\n";
    }

    void flush()
    {
        juce::String out;
        { std::lock_guard<std::mutex> l (m); out.swapWith (pending); }
        if (out.isNotEmpty()) file.appendText (out, false, false, "\n");
    }

    juce::File file;
    std::mutex m;
    juce::String pending;
    std::vector<Instance*> instances;
    int counter = 0;
};

// The logger lives while at least one plugin instance does (never torn
// down by the library's static destructors, where joining a thread could
// hang on Windows).
std::mutex gMutex;
Logger* gLogger = nullptr;
int gUsers = 0;
} // namespace

void log (const juce::String& line)
{
    std::lock_guard<std::mutex> l (gMutex);
    if (gLogger != nullptr) gLogger->add (line);
}

Instance::Instance()
{
    std::lock_guard<std::mutex> l (gMutex);
    if (gLogger == nullptr) gLogger = new Logger();
    ++gUsers;
    number = gLogger->nextId();
    gLogger->enrol (this);
}

Instance::~Instance()
{
    Logger* dead = nullptr;
    {
        std::lock_guard<std::mutex> l (gMutex);
        gLogger->leave (this);
        if (--gUsers == 0) { dead = gLogger; gLogger = nullptr; }
    }
    delete dead;
}

} // namespace vt4diag
