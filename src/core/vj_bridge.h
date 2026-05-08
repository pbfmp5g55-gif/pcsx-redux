/***************************************************************************
 *   VJ subsystem bridge for PCSX-Redux
 *
 *   Phase 3 (current): primitive logging hook only.
 *   Phase 4+ will route through libvj's PrimitiveInterceptor and mutate
 *   vertices / UVs / colors in place. See ps1-primitive-vj/docs/PCSX_REDUX_HOOKS.md
 *
 *   Call sites: src/core/gpu.cc, immediately after each
 *   m_gpuLogger->addNode(...) and before m_gpu->write0(this).
 ***************************************************************************/

#pragma once

#include <cstdint>

#include "core/gpu.h"

namespace PCSX {
namespace vj {

bool isEnabled();
void setEnabled(bool enabled);

void initialize();
void shutdown();

void beginFrame();
void endFrame();

namespace detail {
void logPrim(const char* kind, unsigned vertexCount, bool textured, bool semi, int extra1 = 0, int extra2 = 0);
}

template <PCSX::GPU::Shading sh, PCSX::GPU::Shape shape, PCSX::GPU::Textured t, PCSX::GPU::Blend b,
          PCSX::GPU::Modulation m>
inline void onPrimitive(const PCSX::GPU::Poly<sh, shape, t, b, m>& p) {
    if (!isEnabled()) return;
    constexpr unsigned vcount = (shape == PCSX::GPU::Shape::Tri) ? 3u : 4u;
    detail::logPrim("poly", vcount, t == PCSX::GPU::Textured::Yes, b == PCSX::GPU::Blend::Semi);
}

template <PCSX::GPU::Shading sh, PCSX::GPU::LineType lt, PCSX::GPU::Blend b>
inline void onPrimitive(const PCSX::GPU::Line<sh, lt, b>& l) {
    if (!isEnabled()) return;
    detail::logPrim("line", static_cast<unsigned>(l.x.size()), false, b == PCSX::GPU::Blend::Semi);
}

template <PCSX::GPU::Size s, PCSX::GPU::Textured t, PCSX::GPU::Blend b, PCSX::GPU::Modulation m>
inline void onPrimitive(const PCSX::GPU::Rect<s, t, b, m>& r) {
    if (!isEnabled()) return;
    detail::logPrim("rect", 4u, t == PCSX::GPU::Textured::Yes, b == PCSX::GPU::Blend::Semi, r.w, r.h);
}

}  // namespace vj
}  // namespace PCSX
