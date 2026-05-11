/***************************************************************************
 *   MIDI configuration panel implementation. See midi_config_panel.h.
 ***************************************************************************/

#include "gui/widgets/midi_config_panel.h"

#include <cstdio>

#include "core/vj_bridge.h"
#include "vj/MidiController.h"

namespace PCSX {
namespace Widgets {

void MidiConfigPanel::refreshPorts() {
    m_cachedPorts = ::PCSX::vj::midi::listPorts();
    m_portsCached = true;
}

void MidiConfigPanel::renderPortList() {
    if (!m_portsCached) refreshPorts();

    ImGui::TextUnformatted("Available MIDI input ports:");
    const int currentlyOpen = ::PCSX::vj::midi::openedPort();
    if (m_cachedPorts.empty()) {
        ImGui::TextDisabled("  (no MIDI input ports detected)");
    } else {
        for (size_t i = 0; i < m_cachedPorts.size(); ++i) {
            const int idx = static_cast<int>(i);
            const bool isOpen = currentlyOpen == idx;
            char label[256];
            std::snprintf(label, sizeof(label), "%s [%d] %s",
                          isOpen ? "[OPEN]" : "      ", idx, m_cachedPorts[i].c_str());
            if (ImGui::Selectable(label, isOpen)) {
                ::PCSX::vj::midi::openPort(isOpen ? -1 : idx);
            }
        }
    }

    if (ImGui::Button("Refresh")) refreshPorts();
    ImGui::SameLine();
    if (ImGui::Button("Close port")) {
        ::PCSX::vj::midi::openPort(-1);
    }
    ImGui::SameLine();
    const std::string opened = ::PCSX::vj::midi::openedPortName();
    if (opened.empty()) {
        ImGui::TextDisabled("(no port open)");
    } else {
        ImGui::Text("Open: %s", opened.c_str());
    }
}

void MidiConfigPanel::renderMapping() {
    // Poll learn target. When the most-recently-received CC changes from what
    // it was at arm-time, commit it to the armed axis.
    if (m_learnAxis >= 0) {
        const int latest = ::PCSX::vj::midi::lastReceivedCC();
        if (latest >= 0 && latest != m_learnSeenCC) {
            ::PCSX::vj::midi::setAxisCC(static_cast<::vj::Axis>(m_learnAxis), latest);
            m_learnAxis = -1;
        }
    }

    ImGui::TextUnformatted("CC mapping & live values:");
    for (int axisIdx = 0; axisIdx < ::vj::kAxisCount; ++axisIdx) {
        ImGui::PushID(axisIdx);
        const ::vj::Axis axis = static_cast<::vj::Axis>(axisIdx);

        ImGui::Text("%-9s", ::vj::axisName(axis));
        ImGui::SameLine();

        int cc = ::PCSX::vj::midi::getAxisCC(axis);
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("CC", &cc, 1, 8)) {
            if (cc < 0) cc = 0;
            if (cc > 127) cc = 127;
            ::PCSX::vj::midi::setAxisCC(axis, cc);
        }
        ImGui::SameLine();

        const bool armed = m_learnAxis == axisIdx;
        if (armed) {
            if (ImGui::Button("Cancel")) m_learnAxis = -1;
        } else {
            if (ImGui::Button("Learn")) {
                m_learnAxis = axisIdx;
                m_learnSeenCC = ::PCSX::vj::midi::lastReceivedCC();
            }
        }
        ImGui::SameLine();

        const int v = ::PCSX::vj::midi::getCC(cc);
        const float vf = v < 0 ? 0.0f : static_cast<float>(v) / 127.0f;
        char vbuf[16];
        if (v < 0) std::snprintf(vbuf, sizeof(vbuf), "--");
        else       std::snprintf(vbuf, sizeof(vbuf), "%d", v);
        ImGui::ProgressBar(vf, ImVec2(140, 0), vbuf);

        ImGui::PopID();
    }

    if (m_learnAxis >= 0) {
        ImGui::TextDisabled("Move a control on your MIDI device to assign it to %s.",
                            ::vj::axisName(static_cast<::vj::Axis>(m_learnAxis)));
    }
}

void MidiConfigPanel::draw(const char* title) {
    if (!m_show) return;

    ImGui::SetNextWindowSize(ImVec2(540, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    bool enabled = ::PCSX::vj::midi::isEnabled();
    if (ImGui::Checkbox("MIDI enabled", &enabled)) {
        ::PCSX::vj::midi::setEnabled(enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(when on, MIDI overrides the 8 effect sliders every VSync)");

    ImGui::Separator();
    renderPortList();
    ImGui::Separator();
    renderMapping();

    ImGui::End();
}

}  // namespace Widgets
}  // namespace PCSX
