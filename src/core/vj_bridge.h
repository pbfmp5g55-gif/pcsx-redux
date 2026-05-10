/***************************************************************************
 *   VJ subsystem bridge for PCSX-Redux
 *
 *   Phase 4: live PCSX → libvj interception. onPrimitive() packs the GPU
 *   primitive into a vj::Primitive, runs it through a vj::PrimitiveInterceptor,
 *   writes the (possibly mutated) result back into the original PCSX primitive,
 *   and returns true if the primitive should be submitted (false = drop).
 *
 *   Defaults are master=0 / depth=0, so all primitives pass through unchanged.
 *   Raise master / chance / geometry / etc. via the Params held in vj_bridge.cc
 *   to actually trigger glitch effects.
 *
 *   Call sites: src/core/gpu.cc, immediately after each
 *   m_gpuLogger->addNode(...) and conditionally before m_gpu->write0(this):
 *
 *       g_emulator->m_gpuLogger->addNode(*this, origin, value, length);
 *       if (PCSX::vj::onPrimitive(*this)) m_gpu->write0(this);
 ***************************************************************************/

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

#include "core/gpu.h"
#include "vj/Primitive.h"

namespace PCSX {
namespace vj {

bool isEnabled();
void setEnabled(bool enabled);

namespace detail {
// Submits prim into the libvj interceptor. If the interceptor approves (or
// passes through unchanged), invokes writeBack with the (possibly mutated)
// primitive and returns true. If the interceptor drops the primitive,
// writeBack is not called and the function returns false.
bool intercept(::vj::Primitive& prim,
               const std::function<void(const ::vj::Primitive&)>& writeBack);
}  // namespace detail

template <PCSX::GPU::Shading sh, PCSX::GPU::Shape shape, PCSX::GPU::Textured t,
          PCSX::GPU::Blend b, PCSX::GPU::Modulation m>
inline bool onPrimitive(PCSX::GPU::Poly<sh, shape, t, b, m>& p) {
    if (!isEnabled()) return true;
    constexpr bool quad = (shape == PCSX::GPU::Shape::Quad);
    constexpr bool textured = (t == PCSX::GPU::Textured::Yes);
    constexpr unsigned vc = quad ? 4u : 3u;

    ::vj::Primitive prim;
    prim.kind = quad ? ::vj::PrimitiveKind::Quad : ::vj::PrimitiveKind::Triangle;
    prim.textured = textured;
    prim.vertices.resize(vc);
    for (unsigned i = 0; i < vc; ++i) {
        auto& vv = prim.vertices[i];
        vv.x = static_cast<float>(p.x[i] + p.offset.x);
        vv.y = static_cast<float>(p.y[i] + p.offset.y);
        if constexpr (textured) {
            vv.u = static_cast<float>(p.u[i]);
            vv.v = static_cast<float>(p.v[i]);
        }
        const uint32_t c = p.colors[i];
        vv.r = static_cast<uint8_t>((c >> 0) & 0xff);
        vv.g = static_cast<uint8_t>((c >> 8) & 0xff);
        vv.b = static_cast<uint8_t>((c >> 16) & 0xff);
        vv.a = 255;
    }

    return detail::intercept(prim, [&p](const ::vj::Primitive& mp) {
        const unsigned n = std::min<unsigned>(static_cast<unsigned>(mp.vertices.size()), vc);
        for (unsigned i = 0; i < n; ++i) {
            const auto& mv = mp.vertices[i];
            p.x[i] = static_cast<int>(mv.x) - p.offset.x;
            p.y[i] = static_cast<int>(mv.y) - p.offset.y;
            if constexpr (textured) {
                p.u[i] = static_cast<unsigned>(std::clamp(mv.u, 0.0f, 255.0f));
                p.v[i] = static_cast<unsigned>(std::clamp(mv.v, 0.0f, 255.0f));
            }
            const uint32_t r = mv.r;
            const uint32_t g = mv.g;
            const uint32_t bch = mv.b;
            p.colors[i] = (r << 0) | (g << 8) | (bch << 16);
        }
    });
}

// Phase 4 MVP: Lines and Rects pass through unmodified. libvj's primitive kinds
// (Triangle / Quad / Sprite) do not include Line, and Rect mutation requires
// non-trivial geometry handling (single-corner + size cannot represent skewed
// quads). These will be added in a later phase.
template <PCSX::GPU::Shading sh, PCSX::GPU::LineType lt, PCSX::GPU::Blend b>
inline bool onPrimitive(PCSX::GPU::Line<sh, lt, b>&) {
    return true;
}

template <PCSX::GPU::Size s, PCSX::GPU::Textured t, PCSX::GPU::Blend b, PCSX::GPU::Modulation m>
inline bool onPrimitive(PCSX::GPU::Rect<s, t, b, m>&) {
    return true;
}

}  // namespace vj
}  // namespace PCSX
