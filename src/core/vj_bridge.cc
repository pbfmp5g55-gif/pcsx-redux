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
#include <functional>
#include <memory>
#include <mutex>

#include "core/system.h"
#include "support/eventbus.h"
#include "vj/Params.h"
#include "vj/PrimitiveInterceptor.h"

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

        g_interceptor = std::make_unique<::vj::PrimitiveInterceptor>();
        g_interceptor->setSubmitCallback([](const ::vj::Primitive& p) {
            if (g_currentSubmit) g_currentSubmit(p);
        });
        g_interceptor->beginFrame(g_params, 0);

        if (g_system && g_system->m_eventBus) {
            g_listener = std::make_unique<EventBus::Listener>(g_system->m_eventBus);
            g_listener->listen<Events::GPU::VSync>([](auto) {
                if (!g_interceptor) return;
                g_interceptor->endFrame();
                g_lastFramePrims = g_thisFramePrims;
                g_thisFramePrims = 0;
                g_frameCounter++;
                g_interceptor->beginFrame(g_params, static_cast<int>(g_lastFramePrims));
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
    });
}

}  // namespace

bool isEnabled() { return g_enabled.load(); }
void setEnabled(bool e) { g_enabled.store(e); }

::vj::Params& params() {
    ensureInit();  // make sure env-var seeding has happened before anyone reads
    return g_params;
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

}  // namespace vj
}  // namespace PCSX
