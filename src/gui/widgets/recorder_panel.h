/***************************************************************************
 *   VJ primitive-stream recorder panel (imgui).
 *
 *   Captures the post-libvj primitive stream (the actual prims drawn on
 *   screen, including any glitch effects) into a vj::PrimitiveStream file.
 *   Pair with the (future) replay panel to play back recorded sessions.
 ***************************************************************************/

#pragma once

#include "imgui.h"

namespace PCSX {
namespace Widgets {

class RecorderPanel {
  public:
    RecorderPanel(bool& show) : m_show(show) {}
    void draw(const char* title);

    bool& m_show;

  private:
    char m_pathBuf[512] = "vj-record.vjr";
};

}  // namespace Widgets
}  // namespace PCSX
