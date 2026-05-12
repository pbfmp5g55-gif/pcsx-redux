/***************************************************************************
 *   Filter Preset Bank panel (imgui) for the VJ subsystem.
 *
 *   16 named slots of FilterParams. A single MIDI CC selects which slot
 *   drives the live filter, with optional linear interpolation between
 *   neighbouring slots. Slot contents are user-editable and persist via
 *   pcsx.json (config save path).
 ***************************************************************************/

#pragma once

#include "imgui.h"

namespace PCSX {
namespace Widgets {

class FilterPresetPanel {
  public:
    FilterPresetPanel(bool& show) : m_show(show) {}
    void draw(const char* title);

    bool& m_show;

  private:
    int  m_editingSlot = 0;  // which slot is currently shown in the editor
    bool m_learnCC = false;  // arm: next received CC becomes the preset CC
    int  m_learnSeenCC = -1; // baseline lastReceivedCC at arm time

    void renderMidiSection();
    void renderSlotList();
    void renderSlotEditor();
};

}  // namespace Widgets
}  // namespace PCSX
