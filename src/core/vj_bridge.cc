/***************************************************************************
 *   VJ subsystem bridge implementation. See vj_bridge.h.
 ***************************************************************************/

#include "core/vj_bridge.h"

#include <cstdio>

namespace PCSX {
namespace vj {

namespace {
bool g_enabled = true;
uint64_t g_frameCounter = 0;
uint64_t g_primCounter = 0;
constexpr uint64_t kLogEvery = 4096;
}  // namespace

bool isEnabled() { return g_enabled; }
void setEnabled(bool enabled) { g_enabled = enabled; }

void initialize() {
    g_frameCounter = 0;
    g_primCounter = 0;
    std::fprintf(stderr, "[VJ] subsystem initialized (Phase 3: logging hook only)\n");
}

void shutdown() {
    std::fprintf(stderr, "[VJ] subsystem shutdown after %llu frames, %llu primitives\n",
                 static_cast<unsigned long long>(g_frameCounter),
                 static_cast<unsigned long long>(g_primCounter));
}

void beginFrame() { ++g_frameCounter; }
void endFrame() {}

namespace detail {
void logPrim(const char* kind, unsigned vertexCount, bool textured, bool semi, int extra1, int extra2) {
    ++g_primCounter;
    if ((g_primCounter % kLogEvery) != 1) return;
    std::fprintf(stderr, "[VJ] prim#%llu kind=%s v=%u tex=%d semi=%d e1=%d e2=%d\n",
                 static_cast<unsigned long long>(g_primCounter), kind, vertexCount, textured ? 1 : 0,
                 semi ? 1 : 0, extra1, extra2);
}
}  // namespace detail

}  // namespace vj
}  // namespace PCSX
