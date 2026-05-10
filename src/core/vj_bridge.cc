/***************************************************************************
 *   VJ subsystem bridge implementation. See vj_bridge.h.
 ***************************************************************************/

#include "core/vj_bridge.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace PCSX {
namespace vj {

namespace {
bool g_enabled = true;
uint64_t g_frameCounter = 0;
uint64_t g_primCounter = 0;
constexpr uint64_t kLogEvery = 4096;

// pcsx-redux.main is a Windows GUI subsystem binary; the CRT does not connect
// FILE* stderr to a usable handle, so fprintf(stderr, ...) is silently dropped
// when launched via Start-Process / cmd 2> log redirection. Tee everything to
// a file as well. Path is overridable via VJ_LOG env var; default "vj.log" in
// the process working directory. Line-buffered + flushed each line so the file
// is readable even if the process is killed.
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
}  // namespace

bool isEnabled() { return g_enabled; }
void setEnabled(bool enabled) { g_enabled = enabled; }

void initialize() {
    g_frameCounter = 0;
    g_primCounter = 0;
    vjLog("[VJ] subsystem initialized (Phase 3: logging hook only)\n");
}

void shutdown() {
    vjLog("[VJ] subsystem shutdown after %llu frames, %llu primitives\n",
          static_cast<unsigned long long>(g_frameCounter),
          static_cast<unsigned long long>(g_primCounter));
}

void beginFrame() { ++g_frameCounter; }
void endFrame() {}

namespace detail {
void logPrim(const char* kind, unsigned vertexCount, bool textured, bool semi, int extra1, int extra2) {
    ++g_primCounter;
    if ((g_primCounter % kLogEvery) != 1) return;
    vjLog("[VJ] prim#%llu kind=%s v=%u tex=%d semi=%d e1=%d e2=%d\n",
          static_cast<unsigned long long>(g_primCounter), kind, vertexCount, textured ? 1 : 0,
          semi ? 1 : 0, extra1, extra2);
}
}  // namespace detail

}  // namespace vj
}  // namespace PCSX
