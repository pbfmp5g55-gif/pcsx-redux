/***************************************************************************
 *   MIDI configuration panel (imgui) for the VJ subsystem.
 *
 *   - Toggle MIDI enable
 *   - Pick a MIDI input port (list / refresh / open / close)
 *   - Edit the per-axis CC mapping (8 axes, 0..127)
 *   - "Learn": arm an axis and have the next received CC bound to it
 *   - Live ProgressBar for each axis's currently-mapped CC value
 ***************************************************************************/

#pragma once

#include <string>
#include <vector>

#include "imgui.h"

namespace PCSX {
namespace Widgets {

class MidiConfigPanel {
  public:
    MidiConfigPanel(bool& show) : m_show(show) {}
    void draw(const char* title);

    bool& m_show;

  private:
    std::vector<std::string> m_cachedPorts;
    bool m_portsCached = false;
    int m_learnAxis = -1;     // axis index currently armed; -1 = idle
    int m_learnSeenCC = -1;   // value of lastReceivedCC() at arm time

    void refreshPorts();
    void renderPortList();
    void renderMapping();
};

}  // namespace Widgets
}  // namespace PCSX
