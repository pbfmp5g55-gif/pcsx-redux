/***************************************************************************
 *   VJ subsystem bridge implementation. See vj_bridge.h.
 *
 *   Phase 4 wiring:
 *   - One process-wide vj::PrimitiveInterceptor (lazy-initialised on first
 *     primitive). Its SubmitFn delegates to a thread_local writeback closure
 *     that the templated onPrimitive() in vj_bridge.h sets up per-call.
 *   - VSync EventBus listener installed on first primitive too: pumps
 *     endFrame() / beginFrame() boundaries so DepthDelayQueue and Limiter
 *     advance correctly. With the default Params (depth=0) all submissions
 *     are synchronous, so the writeback closure is always called within the
 *     same stack frame as onPrimitive().
 *   - Diagnostic log retained: stderr is unreliable from a Windows GUI
 *     subsystem binary, so we tee to a file (default "vj.log", overridable
 *     via VJ_LOG env var).
 ***************************************************************************/

#include "core/vj_bridge.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/system.h"
#include "support/eventbus.h"
#include "vj/AutoMode.h"
#include "vj/FilterPresetBank.h"
#include "vj/MidiController.h"
#include "vj/Params.h"
#include "vj/PrimitiveInterceptor.h"
#include "vj/PrimitiveStream.h"
#include "vj/RtMidiController.h"

namespace PCSX {
namespace vj {

namespace {

std::atomic<bool> g_enabled{true};

// Lazy-initialised process singletons. ensureInit() is called from intercept()
// the first time a primitive is observed.
std::once_flag g_initFlag;
std::unique_ptr<::vj::PrimitiveInterceptor> g_interceptor;
std::unique_ptr<EventBus::Listener> g_listener;

// Live params. Edit these (or expose via UI later) to drive glitch effects.
// Phase 4 MVP defaults: every term zero → libvj passes every primitive through
// unmodified (no behaviour change vs. an unhooked build).
::vj::Params g_params;

// Auto mode (LFO modulation). Disabled by default; enable with VJ_AUTO=1.
::vj::AutoModeParams g_auto;

// MIDI state. g_midi is created/destroyed at runtime when the user opens or
// closes a port from the UI, so all access is guarded by g_midiMutex. The
// CC->axis mapping is kept in g_midiMapping so it survives across reopens.
std::mutex g_midiMutex;
std::unique_ptr<::vj::RtMidiController> g_midi;
::vj::CCMapping g_midiMapping;
std::atomic<bool> g_midiEnabled{false};

// Filter preset bank state. Independent of the 8-axis MIDI above — different
// CC, different code path. Bank itself is mutated from the UI thread; the
// VSync reader only takes a snapshot of the values it needs each frame.
::vj::FilterPresetBank g_filterBank;
std::mutex g_filterBankMutex;
std::atomic<bool> g_filterMidiEnabled{false};
std::atomic<int>  g_filterPresetCC{28};
std::atomic<bool> g_filterInterpolation{true};
std::atomic<int>  g_filterLastCC{-1};

// Recording state. g_recordBuffer accumulates primitives from onPrimitive()
// within a frame; the VSync listener flushes it via the writer using a
// known-up-front primitive count (PrimitiveStream requires the count at
// beginFrame).
std::mutex                          g_recordMutex;
std::unique_ptr<::vj::PrimitiveStreamWriter> g_recordWriter;
std::vector<::vj::Primitive>        g_recordBuffer;
std::string                         g_recordPath;
std::atomic<uint64_t>               g_recordedFrames{0};

uint64_t g_frameCounter = 0;
uint64_t g_thisFramePrims = 0;
uint64_t g_lastFramePrims = 0;
constexpr uint64_t kLogEvery = 4096;

// pcsx-redux.main is built as WINDOWS_GUI; the CRT does not connect FILE*
// stderr to a usable handle there, so fprintf(stderr,...) silently drops.
// Tee everything to a file for diagnostics.
FILE* logSink() {
    static FILE* f = []() -> FILE* {
        const char* p = std::getenv("VJ_LOG");
        if (!p || !*p) p = "vj.log";
        FILE* r = std::fopen(p, "w");
        if (r) std::setvbuf(r, nullptr, _IOLBF, 4096);
        return r;
    }();
    return f;
}

void vjLog(const char* fmt, ...) {
    va_list a1;
    va_start(a1, fmt);
    std::vfprintf(stderr, fmt, a1);
    va_end(a1);
    if (FILE* f = logSink()) {
        va_list a2;
        va_start(a2, fmt);
        std::vfprintf(f, fmt, a2);
        va_end(a2);
        std::fflush(f);
    }
}

// Per-call writeback. Set by onPrimitive() before interceptAndSubmit(), cleared
// after. interceptor->setSubmitCallback() trampolines into this. With depth=0
// (default), the callback always fires synchronously inside interceptAndSubmit
// and this never escapes the call.
thread_local std::function<void(const ::vj::Primitive&)> g_currentSubmit;

// Pull a float [0,1]-ish param from env. Empty/unset -> default (current value).
float envFloat(const char* name, float def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return std::strtof(v, nullptr);
}

int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return static_cast<int>(std::strtol(v, nullptr, 10));
}

void ensureInit() {
    std::call_once(g_initFlag, []() {
        // Allow live tuning without rebuild via env vars. All default to 0.
        g_params.master   = envFloat("VJ_MASTER",   g_params.master);
        g_params.chance   = envFloat("VJ_CHANCE",   g_params.chance);
        g_params.geometry = envFloat("VJ_GEOMETRY", g_params.geometry);
        g_params.texture  = envFloat("VJ_TEXTURE",  g_params.texture);
        g_params.missing  = envFloat("VJ_MISSING",  g_params.missing);
        g_params.color    = envFloat("VJ_COLOR",    g_params.color);
        g_params.depth    = envFloat("VJ_DEPTH",    g_params.depth);
        g_params.chaos    = envFloat("VJ_CHAOS",    g_params.chaos);

        g_params.filter.texturedOnly = envInt("VJ_FILTER_TEXTURED", 0) != 0;
        g_params.filter.minArea      = envFloat("VJ_FILTER_MIN_AREA", 0.0f);
        g_params.filter.maxArea      = envFloat("VJ_FILTER_MAX_AREA", 0.0f);
        g_params.filter.regionX0     = envFloat("VJ_FILTER_REGION_X0", 0.0f);
        g_params.filter.regionY0     = envFloat("VJ_FILTER_REGION_Y0", 0.0f);
        g_params.filter.regionX1     = envFloat("VJ_FILTER_REGION_X1", 0.0f);
        g_params.filter.regionY1     = envFloat("VJ_FILTER_REGION_Y1", 0.0f);
        g_params.filter.everyN       = envInt("VJ_FILTER_EVERY_N", 0);

        g_auto.enabled = envInt("VJ_AUTO", 0) != 0;
        g_auto.depth   = envFloat("VJ_AUTO_DEPTH", g_auto.depth);
        g_auto.rate    = envFloat("VJ_AUTO_RATE",  g_auto.rate);

        // Demo preset bank. 16 slots filled with useful starting points;
        // users can overwrite any of them from the UI and persist via
        // pcsx.json on shutdown.
        {
            auto& b = g_filterBank;
            auto setSlot = [&](int i, const char* name, const ::vj::FilterParams& fp) {
                b.slot(i).name   = name;
                b.slot(i).params = fp;
            };
            ::vj::FilterParams f;
            setSlot(0, "All",          f);
            f = {}; f.texturedOnly = true;
            setSlot(1, "Keys only",    f);
            f = {}; f.minArea = 15000.0f;
            setSlot(2, "BG only",      f);
            f = {}; f.maxArea = 2000.0f;
            setSlot(3, "Small obj",    f);
            f = {}; f.everyN = 2;
            setSlot(4, "Sparse 2",     f);
            f = {}; f.everyN = 4;
            setSlot(5, "Sparse 4",     f);
            f = {}; f.everyN = 8;
            setSlot(6, "Sparse 8",     f);
            f = {}; f.regionX0 = 0.0f;   f.regionY0 = 0.0f;
                    f.regionX1 = 320.0f; f.regionY1 = 120.0f;
            setSlot(7, "Top half",     f);
            f = {}; f.regionX0 = 0.0f;   f.regionY0 = 120.0f;
                    f.regionX1 = 320.0f; f.regionY1 = 240.0f;
            setSlot(8, "Bottom half",  f);
            f = {}; f.regionX0 = 0.0f;   f.regionY0 = 0.0f;
                    f.regionX1 = 160.0f; f.regionY1 = 240.0f;
            setSlot(9, "Left half",    f);
            f = {}; f.regionX0 = 160.0f; f.regionY0 = 0.0f;
                    f.regionX1 = 320.0f; f.regionY1 = 240.0f;
            setSlot(10, "Right half",  f);
            f = {}; f.regionX0 = 80.0f;  f.regionY0 = 60.0f;
                    f.regionX1 = 240.0f; f.regionY1 = 180.0f;
            setSlot(11, "Center",      f);
            f = {}; f.texturedOnly = true; f.everyN = 4;
            setSlot(12, "Keys+Sparse4", f);
            f = {}; f.minArea = 15000.0f;  f.everyN = 4;
            setSlot(13, "BG+Sparse4",   f);
            f = {}; f.maxArea = 2000.0f;
                    f.regionX0 = 80.0f;  f.regionY0 = 60.0f;
                    f.regionX1 = 240.0f; f.regionY1 = 180.0f;
            setSlot(14, "Small+Center", f);
            f = {}; f.minArea = 20000.0f;  f.everyN = 2;
            setSlot(15, "Heavy BG",     f);

            // Best-effort restore of a previously-saved bank from disk. If
            // the file is missing or unreadable, the demo bank above stays.
            g_filterBank.loadFrom("vj-presets.bin");
        }
        g_filterMidiEnabled.store(envInt("VJ_FILTER_PRESET_MIDI", 0) != 0);
        g_filterPresetCC.store(envInt("VJ_FILTER_PRESET_CC", 28));
        g_filterInterpolation.store(envInt("VJ_FILTER_PRESET_INTERP", 1) != 0);

        // MIDI seed. Both VJ_MIDI_ENABLED=1 and a valid VJ_MIDI_PORT are
        // required to actually open a port at startup. The UI can change both
        // at runtime via the midi::* API.
        g_midiEnabled.store(envInt("VJ_MIDI_ENABLED", 0) != 0);
        const int seedMidiPort = envInt("VJ_MIDI_PORT", -1);
        if (seedMidiPort >= 0) {
            try {
                g_midi = std::make_unique<::vj::RtMidiController>(
                    static_cast<unsigned int>(seedMidiPort));
                g_midi->setMapping(g_midiMapping);
                vjLog("[VJ] MIDI port %d opened: %s\n", seedMidiPort,
                      g_midi->portName().c_str());
            } catch (const std::exception& e) {
                vjLog("[VJ] MIDI port %d open failed: %s\n", seedMidiPort, e.what());
            }
        }

        g_interceptor = std::make_unique<::vj::PrimitiveInterceptor>();
        g_interceptor->setSubmitCallback([](const ::vj::Primitive& p) {
            if (g_currentSubmit) g_currentSubmit(p);
            // Capture the as-drawn primitive for any active recording.
            std::lock_guard<std::mutex> lk(g_recordMutex);
            if (g_recordWriter) g_recordBuffer.push_back(p);
        });
        g_interceptor->beginFrame(
            ::vj::applyAutoMode(g_params, g_auto, static_cast<int>(g_frameCounter)),
            0);

        if (g_system && g_system->m_eventBus) {
            g_listener = std::make_unique<EventBus::Listener>(g_system->m_eventBus);
            g_listener->listen<Events::GPU::VSync>([](auto) {
                if (!g_interceptor) return;
                g_interceptor->endFrame();
                g_lastFramePrims = g_thisFramePrims;
                g_thisFramePrims = 0;
                g_frameCounter++;
                // If MIDI is active, overwrite the 8 effect-axis fields from
                // the latest CC snapshot. filter and auto-mode are untouched.
                if (g_midiEnabled.load()) {
                    std::lock_guard<std::mutex> lk(g_midiMutex);
                    if (g_midi) {
                        const auto mp = g_midi->buildParams();
                        g_params.master   = mp.master;
                        g_params.chance   = mp.chance;
                        g_params.geometry = mp.geometry;
                        g_params.texture  = mp.texture;
                        g_params.missing  = mp.missing;
                        g_params.color    = mp.color;
                        g_params.depth    = mp.depth;
                        g_params.chaos    = mp.chaos;
                    }
                }
                // Filter preset MIDI: independent path, drives g_params.filter
                // from the preset bank based on a dedicated CC value.
                if (g_filterMidiEnabled.load()) {
                    int val = -1;
                    {
                        std::lock_guard<std::mutex> lk(g_midiMutex);
                        if (g_midi) val = g_midi->getCC(g_filterPresetCC.load());
                    }
                    if (val >= 0) {
                        g_filterLastCC.store(val);
                        std::lock_guard<std::mutex> lk(g_filterBankMutex);
                        g_params.filter = g_filterInterpolation.load()
                            ? g_filterBank.selectInterpolated(val)
                            : g_filterBank.selectSnap(val);
                    }
                }
                // Flush the recording buffer for the frame that just ended.
                {
                    std::lock_guard<std::mutex> lk(g_recordMutex);
                    if (g_recordWriter) {
                        g_recordWriter->beginFrame(
                            static_cast<int>(g_frameCounter),
                            static_cast<int>(g_recordBuffer.size()));
                        for (const auto& p : g_recordBuffer) {
                            g_recordWriter->writePrimitive(p);
                        }
                        g_recordBuffer.clear();
                        g_recordedFrames.fetch_add(1);
                    }
                }
                g_interceptor->beginFrame(
                    ::vj::applyAutoMode(g_params, g_auto, static_cast<int>(g_frameCounter)),
                    static_cast<int>(g_lastFramePrims));
            });
        }

        vjLog("[VJ] Phase 4 bridge live (master=%.2f chance=%.2f geom=%.2f "
              "tex=%.2f miss=%.2f color=%.2f depth=%.2f chaos=%.2f, "
              "depth-delay %s, listener %s)\n",
              g_params.master, g_params.chance, g_params.geometry,
              g_params.texture, g_params.missing, g_params.color,
              g_params.depth, g_params.chaos,
              g_params.depth > 0.001f ? "ON" : "OFF",
              g_listener ? "installed" : "MISSING (g_system unavailable)");

        vjLog("[VJ] filter (texturedOnly=%d minArea=%.0f maxArea=%.0f "
              "region=[%.0f,%.0f]-[%.0f,%.0f] everyN=%d)\n",
              g_params.filter.texturedOnly ? 1 : 0,
              g_params.filter.minArea, g_params.filter.maxArea,
              g_params.filter.regionX0, g_params.filter.regionY0,
              g_params.filter.regionX1, g_params.filter.regionY1,
              g_params.filter.everyN);

        vjLog("[VJ] auto (enabled=%d depth=%.2f rate=%.2f)\n",
              g_auto.enabled ? 1 : 0, g_auto.depth, g_auto.rate);

        vjLog("[VJ] midi (enabled=%d port=%d name=%s)\n",
              g_midiEnabled.load() ? 1 : 0,
              g_midi ? static_cast<int>(g_midi->portIndex()) : -1,
              g_midi ? g_midi->portName().c_str() : "(none)");
    });
}

}  // namespace

bool isEnabled() { return g_enabled.load(); }
void setEnabled(bool e) { g_enabled.store(e); }

::vj::Params& params() {
    ensureInit();  // make sure env-var seeding has happened before anyone reads
    return g_params;
}

::vj::AutoModeParams& autoParams() {
    ensureInit();
    return g_auto;
}

unsigned long long lastFramePrimitiveCount() {
    return static_cast<unsigned long long>(g_lastFramePrims);
}

namespace detail {

bool intercept(::vj::Primitive& prim,
               const std::function<void(const ::vj::Primitive&)>& writeBack) {
    ensureInit();
    if (!g_interceptor) {
        // No interceptor available — pass through unchanged.
        return true;
    }

    bool submitted = false;
    g_currentSubmit = [&writeBack, &submitted](const ::vj::Primitive& p) {
        writeBack(p);
        submitted = true;
    };

    g_interceptor->interceptAndSubmit(prim);
    g_currentSubmit = nullptr;

    g_thisFramePrims++;
    if ((g_thisFramePrims % kLogEvery) == 1) {
        vjLog("[VJ] frame#%llu prim#%llu kind=%d v=%u tex=%d submitted=%d\n",
              static_cast<unsigned long long>(g_frameCounter),
              static_cast<unsigned long long>(g_thisFramePrims),
              static_cast<int>(prim.kind),
              static_cast<unsigned>(prim.vertices.size()),
              prim.textured ? 1 : 0,
              submitted ? 1 : 0);
    }

    return submitted;
}

}  // namespace detail

namespace midi {

bool isEnabled() { return g_midiEnabled.load(); }

void setEnabled(bool e) {
    ensureInit();
    g_midiEnabled.store(e);
}

std::vector<std::string> listPorts() {
    ensureInit();
    return ::vj::RtMidiController::listPorts();
}

bool openPort(int portIndex) {
    ensureInit();
    std::lock_guard<std::mutex> lk(g_midiMutex);
    const ::vj::CCMapping saved = g_midi ? g_midi->mapping() : g_midiMapping;
    g_midi.reset();
    if (portIndex < 0) {
        g_midiMapping = saved;
        vjLog("[VJ] MIDI port closed\n");
        return true;
    }
    try {
        g_midi = std::make_unique<::vj::RtMidiController>(
            static_cast<unsigned int>(portIndex));
        g_midi->setMapping(saved);
        g_midiMapping = saved;
        vjLog("[VJ] MIDI port %d opened: %s\n", portIndex,
              g_midi->portName().c_str());
        return true;
    } catch (const std::exception& e) {
        vjLog("[VJ] MIDI port %d open failed: %s\n", portIndex, e.what());
        return false;
    }
}

int openedPort() {
    std::lock_guard<std::mutex> lk(g_midiMutex);
    return g_midi ? static_cast<int>(g_midi->portIndex()) : -1;
}

std::string openedPortName() {
    std::lock_guard<std::mutex> lk(g_midiMutex);
    return g_midi ? g_midi->portName() : std::string();
}

int getAxisCC(::vj::Axis axis) {
    ensureInit();
    std::lock_guard<std::mutex> lk(g_midiMutex);
    return g_midi ? g_midi->axisCC(axis) : g_midiMapping[axis];
}

void setAxisCC(::vj::Axis axis, int cc) {
    ensureInit();
    std::lock_guard<std::mutex> lk(g_midiMutex);
    g_midiMapping[axis] = cc;
    if (g_midi) g_midi->setAxisCC(axis, cc);
}

int lastReceivedCC() {
    std::lock_guard<std::mutex> lk(g_midiMutex);
    return g_midi ? g_midi->lastReceivedCC() : -1;
}

void clearLastReceivedCC() {
    std::lock_guard<std::mutex> lk(g_midiMutex);
    if (g_midi) g_midi->clearLastReceivedCC();
}

int getCC(int cc) {
    std::lock_guard<std::mutex> lk(g_midiMutex);
    return g_midi ? g_midi->getCC(cc) : -1;
}

}  // namespace midi

namespace filter {

::vj::FilterPresetBank& presetBank() {
    ensureInit();
    return g_filterBank;
}

bool isMidiEnabled() { return g_filterMidiEnabled.load(); }
void setMidiEnabled(bool e) {
    ensureInit();
    g_filterMidiEnabled.store(e);
}

int  presetCC() { return g_filterPresetCC.load(); }
void setPresetCC(int cc) {
    if (cc < 0) cc = 0;
    if (cc > 127) cc = 127;
    g_filterPresetCC.store(cc);
}

bool interpolation() { return g_filterInterpolation.load(); }
void setInterpolation(bool on) { g_filterInterpolation.store(on); }

int currentCC() { return g_filterLastCC.load(); }

bool saveBank(const std::string& path) {
    ensureInit();
    std::lock_guard<std::mutex> lk(g_filterBankMutex);
    const bool ok = g_filterBank.saveTo(path);
    vjLog("[VJ] filter: saveBank %s %s\n",
          path.c_str(), ok ? "OK" : "FAILED");
    return ok;
}

bool loadBank(const std::string& path) {
    ensureInit();
    std::lock_guard<std::mutex> lk(g_filterBankMutex);
    const bool ok = g_filterBank.loadFrom(path);
    vjLog("[VJ] filter: loadBank %s %s\n",
          path.c_str(), ok ? "OK" : "FAILED");
    return ok;
}

std::string defaultBankPath() { return "vj-presets.bin"; }

}  // namespace filter

namespace record {

bool isRecording() {
    std::lock_guard<std::mutex> lk(g_recordMutex);
    return g_recordWriter != nullptr;
}

bool start(const std::string& path) {
    ensureInit();
    std::lock_guard<std::mutex> lk(g_recordMutex);
    auto w = std::make_unique<::vj::PrimitiveStreamWriter>();
    if (!w->open(path)) {
        vjLog("[VJ] record: failed to open %s\n", path.c_str());
        return false;
    }
    g_recordWriter = std::move(w);
    g_recordPath   = path;
    g_recordBuffer.clear();
    g_recordedFrames.store(0);
    vjLog("[VJ] record: started -> %s\n", path.c_str());
    return true;
}

void stop() {
    std::lock_guard<std::mutex> lk(g_recordMutex);
    if (!g_recordWriter) return;
    g_recordWriter->close();
    g_recordWriter.reset();
    vjLog("[VJ] record: stopped (%llu frames -> %s)\n",
          static_cast<unsigned long long>(g_recordedFrames.load()),
          g_recordPath.c_str());
    g_recordPath.clear();
    g_recordBuffer.clear();
}

std::string currentPath() {
    std::lock_guard<std::mutex> lk(g_recordMutex);
    return g_recordPath;
}

uint64_t recordedFrames() { return g_recordedFrames.load(); }

}  // namespace record

}  // namespace vj
}  // namespace PCSX
