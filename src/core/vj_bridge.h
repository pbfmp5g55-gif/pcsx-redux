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
#include <string>
#include <vector>

#include "core/gpu.h"
#include "vj/AutoMode.h"
#include "vj/FilterPresetBank.h"
#include "vj/MidiController.h"
#include "vj/Params.h"
#include "vj/Primitive.h"
#include "vj/PrimitiveStream.h"

namespace PCSX {
namespace vj {

bool isEnabled();
void setEnabled(bool enabled);

// Live process-wide glitch params. Mutating these between frames takes effect
// on the next vj::PrimitiveInterceptor::beginFrame(). Defaults are all zero
// (passthrough); env vars (VJ_MASTER / VJ_GEOMETRY / ...) seed them at startup.
::vj::Params& params();

// Live AutoMode (LFO modulation) config. enabled=false (default) passes Params
// through unchanged. Seeded by VJ_AUTO / VJ_AUTO_DEPTH / VJ_AUTO_RATE.
::vj::AutoModeParams& autoParams();

// Per-frame primitive counter — last fully-completed frame's total. Mainly
// useful for HUD-style overlays in the GUI.
unsigned long long lastFramePrimitiveCount();

// MIDI control — wraps a vj::RtMidiController. When midi::isEnabled() and a
// port is open, the 8 effect-axis fields of params() are overwritten from
// MIDI CCs on every VSync (filter / auto-mode configs are left intact).
namespace midi {

bool isEnabled();
void setEnabled(bool enabled);

// Snapshot of available MIDI input ports (calls RtMidi::getPortCount() under
// the hood). Safe to call from the UI thread.
std::vector<std::string> listPorts();

// Close any currently-open port, then open the given index. -1 closes only.
// Returns true on success. The current CC->axis mapping is preserved across
// open/close cycles.
bool openPort(int portIndex);

// -1 if no port is open.
int openedPort();
std::string openedPortName();

// Per-axis CC mapping. Defaults to 20..27 (see vj/MidiController.h `cc::`).
int getAxisCC(::vj::Axis axis);
void setAxisCC(::vj::Axis axis, int cc);

// CC number of the most recently received Control Change message, or -1.
// Used for the "Learn" UI flow: clear -> wait for user to move a control ->
// read.
int lastReceivedCC();
void clearLastReceivedCC();

// Current value of the given CC (0..127) or -1 if it has not been touched
// since the port was opened.
int getCC(int cc);

}  // namespace midi

// Filter preset bank — 16-slot bank of FilterParams, selectable via a single
// MIDI CC (when filter::isMidiEnabled()). With interpolation enabled the CC
// value blends between neighbouring slots; otherwise it hard-snaps to the
// closest slot. Independent of the 8-axis MIDI mapping above (different CC
// number, different code path).
namespace filter {

// Direct access for the UI (read/write all 16 slots and their names).
::vj::FilterPresetBank& presetBank();

bool isMidiEnabled();
void setMidiEnabled(bool enabled);

// CC number to listen on for the preset-select knob. Default 28 (sits just
// past the 8-axis defaults at 20..27).
int  presetCC();
void setPresetCC(int cc);

// When true, knob position blends adjacent slots; when false, hard-snap.
bool interpolation();
void setInterpolation(bool on);

// Last CC value observed on the preset CC (0..127), or -1 if none received
// since the port was opened. UI uses this to show "current knob position".
int currentCC();

}  // namespace filter

// Recording — captures every libvj-observed primitive (the mutated, post-
// interceptor copy that ends up on screen) into a vj::PrimitiveStream file.
// Toggleable at runtime; one frame per VSync. The file holds whatever
// primitives the GPU actually drew, including any glitch / filter / auto
// effects applied by libvj.
namespace record {

bool isRecording();

// Start recording to the given file path. Truncates any existing file.
// Returns true on success.
bool start(const std::string& path);

// Close the file and stop recording. No-op if not currently recording.
void stop();

// Filesystem path of the currently-open recording, or empty string when
// idle.
std::string currentPath();

// How many frames have been written since start().
uint64_t recordedFrames();

}  // namespace record

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
