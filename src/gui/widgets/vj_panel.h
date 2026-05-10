/***************************************************************************
 *   VJ subsystem control panel (imgui).
 *
 *   8 sliders for the live glitch params + an enable/disable toggle. Modifies
 *   the process-wide ::vj::Params held in core/vj_bridge.cc. Effects take
 *   hold on the next VSync (libvj's beginFrame() copies the snapshot into
 *   its local Params and re-derives the safety-clamped master).
 ***************************************************************************/

#pragma once

#include "imgui.h"

namespace PCSX {
namespace Widgets {

class VJPanel {
  public:
    VJPanel(bool& show) : m_show(show) {}
    void draw(const char* title);

    bool& m_show;
};

}  // namespace Widgets
}  // namespace PCSX
