/***************************************************************************
 *   VJ live IPC panel (imgui).
 *
 *   Toggles the live primitive-stream sender to a Windows shared-memory
 *   ring. The mixer (ps1-vj-mix) attaches to the same ring and draws
 *   what it reads.
 ***************************************************************************/

#pragma once

#include "imgui.h"

namespace PCSX {
namespace Widgets {

class LivePanel {
  public:
    LivePanel(bool& show) : m_show(show) {}
    void draw(const char* title);

    bool& m_show;

  private:
    char m_nameBuf[128] = "Local\\vj-mix-prim-A";
};

}  // namespace Widgets
}  // namespace PCSX
