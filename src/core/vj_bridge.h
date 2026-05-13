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

// Save / load the preset bank to a binary file. Auto-load is attempted from
// the configured default path on first init, so a previously-saved bank
// survives across emulator restarts.
bool saveBank(const std::string& path);
bool loadBank(const std::string& path);

// Default path used by the UI for save/load (and auto-load at startup).
std::string defaultBankPath();

}  // namespace filter

// Notify the VJ subsystem of a CPU->VRAM upload (PS1 GP0 0xA0 command).
// The data points to w*h 16-bpp pixels in PS1 5/5/5/mask layout. When
// recording is on, the upload is appended to the current frame's record
// buffer; when not, this is a no-op. Hooked from gpu.cc's BlitRamVram
// processWrite, right after the gpuLogger addNode.
void onVRAMUpload(int x, int y, int w, int h, const uint16_t* data);

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

// Live IPC streaming — sends the post-libvj primitive stream over a
// Windows shared-memory ring (vjmix::IpcRingWriter under the hood). The
// mixer / Phase B host reads the ring in real time. Independent of
// record:: — you can run both, neither, or one or the other.
namespace live {

bool isActive();

// Start (re-)creating the named ring. Returns false on filesystem failure.
bool start(const std::string& name);

// Tear down the ring.
void stop();

std::string currentName();
uint32_t    droppedCount();  // bumped when backpressure drops a record.

}  // namespace live

namespace detail {
// Submits prim into the libvj interceptor. If the interceptor approves (or
// passes through unchanged), invokes writeBack with the (possibly mutated)
// primitive and returns true. If the interceptor drops the primitive,
// writeBack is not called and the function returns false.
bool intercept(::vj::Primitive& prim,
               const std::function<void(const ::vj::Primitive&)>& writeBack);

// Read the 16- or 256-entry CLUT for a CLUT-indexed textured primitive
// straight out of PS1 VRAM at submission time. Output is resized to
// `paletteEntries` (16 or 8 bpp) of PS1 5/5/5/M uint16 colours. Anything
// other than 16 or 256 clears `out`. Called only on the textured path,
// so 15bpp direct-colour primitives never reach here.
void captureClut(uint16_t clutraw, int paletteEntries,
                 std::vector<uint16_t>& out);
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
    // PS1 semi-transparency = the GP0 command bit; the ABR sub-mode lives
    // in TPage and is not part of this template's parameters. For now we
    // collapse all semi-transparent primitives to Average mode (mode 0,
    // the most common case). Future work: pull ABR out of g_emulator's
    // last TPage register and pick the right sub-mode.
    prim.blendMode = (b == PCSX::GPU::Blend::Semi)
                         ? ::vj::BlendMode::Average
                         : ::vj::BlendMode::Opaque;
    // Pack the GPU's TPage and CLUT registers into hostTag so the mixer
    // (or any other consumer) can locate the texture page in VRAM and
    // the palette for 4bpp/8bpp CLUT sprites.
    //   bits 0..15   = clutraw (16 bits)  -- clutX/16 in bits 0..5,
    //                                        clutY    in bits 6..14
    //   bits 16..23  = reserved
    //   bits 24..39  = tpage.raw low 16 bits
    //                                     -- TPageX/64 in bits 0..3,
    //                                        TPageY*256 in bit 4,
    //                                        semi-transparency 5..6,
    //                                        TP (bpp) 7..8
    if constexpr (textured) {
        prim.hostTag = (static_cast<uint64_t>(p.clutraw) & 0xFFFFu) |
                       ((static_cast<uint64_t>(p.tpage.raw) & 0xFFFFu) << 24);
        // TP (bpp) lives in tpage.raw bits 7..8: 0=4bpp / 1=8bpp / 2=15bpp.
        // Only 4bpp and 8bpp use a CLUT; 15bpp samples VRAM directly.
        const unsigned tp = (static_cast<unsigned>(p.tpage.raw) >> 7) & 0x3u;
        const int paletteEntries = (tp == 0u) ? 16 : (tp == 1u) ? 256 : 0;
        if (paletteEntries > 0) {
            detail::captureClut(static_cast<uint16_t>(p.clutraw),
                                paletteEntries, prim.palette);
        }
    }
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

// Rects (PS1 GP0 0x60..0x7F sprite commands) are unpacked into a 4-vertex
// vj::Primitive (kind=Quad) so the mixer sees backgrounds, HUDs and other
// sprite-based geometry. pcsx-redux's own GPU rendering of the Rect is
// left intact — the writeBack closure is a noop, so libvj's glitch
// effects (geometry jitter etc.) don't mutate the rect on the emulator's
// own picture. That's the deliberate limit: a deformed quad cannot be
// expressed as a Rect (corner+size), and rejecting the rect entirely
// would break the game.
template <PCSX::GPU::Size s, PCSX::GPU::Textured t, PCSX::GPU::Blend b,
          PCSX::GPU::Modulation m>
inline bool onPrimitive(PCSX::GPU::Rect<s, t, b, m>& p) {
    if (!isEnabled()) return true;
    constexpr bool textured = (t == PCSX::GPU::Textured::Yes);
    // Raw-texture rects (textured && modulation off) have no `color` field
    // on the Rect struct (POLYFILL_NO_UNIQUE_ADDRESS makes it Empty), so
    // we only read .color when the template guarantees it exists.
    constexpr bool hasColor =
        (!textured) || (m == PCSX::GPU::Modulation::On);

    ::vj::Primitive prim;
    prim.kind = ::vj::PrimitiveKind::Quad;
    prim.textured = textured;
    prim.blendMode = (b == PCSX::GPU::Blend::Semi)
                         ? ::vj::BlendMode::Average
                         : ::vj::BlendMode::Opaque;
    if constexpr (textured) {
        prim.hostTag = (static_cast<uint64_t>(p.clutraw) & 0xFFFFu) |
                       ((static_cast<uint64_t>(p.tpage.raw) & 0xFFFFu) << 24);
        const unsigned tp = (static_cast<unsigned>(p.tpage.raw) >> 7) & 0x3u;
        const int paletteEntries = (tp == 0u) ? 16 : (tp == 1u) ? 256 : 0;
        if (paletteEntries > 0) {
            detail::captureClut(static_cast<uint16_t>(p.clutraw),
                                paletteEntries, prim.palette);
        }
    }
    const int x0 = p.x + p.offset.x;
    const int y0 = p.y + p.offset.y;
    const int x1 = x0 + p.w;
    const int y1 = y0 + p.h;
    // PS1 vertex-colour 0x80 is the "no modulation" sentinel that the
    // mixer's textured shader treats as 1.0x; using it as the default
    // keeps raw-texture rects looking the same as in the emulator.
    uint8_t r = 0x80, g = 0x80, bch = 0x80;
    if constexpr (hasColor) {
        const uint32_t c = p.color;
        r   = static_cast<uint8_t>((c >>  0) & 0xff);
        g   = static_cast<uint8_t>((c >>  8) & 0xff);
        bch = static_cast<uint8_t>((c >> 16) & 0xff);
    }
    int u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if constexpr (textured) {
        u0 = static_cast<int>(p.u);
        v0 = static_cast<int>(p.v);
        u1 = u0 + p.w;
        v1 = v0 + p.h;
    }
    prim.vertices.resize(4);
    auto setVtx = [&](unsigned i, int x, int y, int u, int v) {
        auto& vv = prim.vertices[i];
        vv.x = static_cast<float>(x);
        vv.y = static_cast<float>(y);
        vv.u = static_cast<float>(u);
        vv.v = static_cast<float>(v);
        vv.r = r;
        vv.g = g;
        vv.b = bch;
        vv.a = 255;
    };
    // Vertex order matches the existing Quad path in the mixer
    // (drawTextured assumes 0=top-left, 1=top-right, 2=bottom-left,
    // 3=bottom-right and stitches the two triangles 0-1-2 + 1-3-2).
    setVtx(0, x0, y0, u0, v0);
    setVtx(1, x1, y0, u1, v0);
    setVtx(2, x0, y1, u0, v1);
    setVtx(3, x1, y1, u1, v1);
    detail::intercept(prim, [](const ::vj::Primitive&) {});
    return true;
}

}  // namespace vj
}  // namespace PCSX
