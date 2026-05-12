/***************************************************************************
 *   Filter Preset Bank panel implementation. See filter_preset_panel.h.
 ***************************************************************************/

#include "gui/widgets/filter_preset_panel.h"

#include <cstdio>
#include <cstring>

#include "core/vj_bridge.h"
#include "vj/FilterPresetBank.h"
#include "vj/MidiController.h"

namespace PCSX {
namespace Widgets {

void FilterPresetPanel::renderMidiSection() {
    bool enabled = ::PCSX::vj::filter::isMidiEnabled();
    if (ImGui::Checkbox("Filter MIDI enabled", &enabled)) {
        ::PCSX::vj::filter::setMidiEnabled(enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(MIDI must also be on; uses a separate CC)");

    bool interp = ::PCSX::vj::filter::interpolation();
    if (ImGui::Checkbox("Interpolation (blend between slots)", &interp)) {
        ::PCSX::vj::filter::setInterpolation(interp);
    }

    // Preset-select CC + Learn.
    if (m_learnCC) {
        const int latest = ::PCSX::vj::midi::lastReceivedCC();
        if (latest >= 0 && latest != m_learnSeenCC) {
            ::PCSX::vj::filter::setPresetCC(latest);
            m_learnCC = false;
        }
    }

    int cc = ::PCSX::vj::filter::presetCC();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("Preset CC", &cc, 1, 8)) {
        ::PCSX::vj::filter::setPresetCC(cc);
    }
    ImGui::SameLine();
    if (m_learnCC) {
        if (ImGui::Button("Cancel##learncc")) m_learnCC = false;
    } else {
        if (ImGui::Button("Learn##learncc")) {
            m_learnCC = true;
            m_learnSeenCC = ::PCSX::vj::midi::lastReceivedCC();
        }
    }
    if (m_learnCC) {
        ImGui::TextDisabled("Move a knob on your MIDI device to assign it.");
    }

    // Live readout: current CC value + which slot it lands on.
    const int v = ::PCSX::vj::filter::currentCC();
    const float vf = v < 0 ? 0.0f : static_cast<float>(v) / 127.0f;
    char vbuf[32];
    if (v < 0) std::snprintf(vbuf, sizeof(vbuf), "--");
    else       std::snprintf(vbuf, sizeof(vbuf), "%d", v);
    ImGui::Text("Live CC:");
    ImGui::SameLine();
    ImGui::ProgressBar(vf, ImVec2(180, 0), vbuf);
    ImGui::SameLine();
    if (v >= 0) {
        const int slotIdx = ::vj::FilterPresetBank::slotForCC(v);
        const std::string& name = ::PCSX::vj::filter::presetBank().slot(slotIdx).name;
        ImGui::Text("-> slot %d \"%s\"", slotIdx, name.c_str());
    } else {
        ImGui::TextDisabled("-> (no CC received)");
    }
}

void FilterPresetPanel::renderSlotList() {
    auto& bank = ::PCSX::vj::filter::presetBank();
    ImGui::TextUnformatted("Slots (click to edit):");
    if (ImGui::BeginChild("##slots", ImVec2(180, 0), true)) {
        for (int i = 0; i < ::vj::FilterPresetBank::slotCount(); ++i) {
            char label[64];
            std::snprintf(label, sizeof(label), "%2d  %s", i,
                          bank.slot(i).name.c_str());
            const bool sel = m_editingSlot == i;
            if (ImGui::Selectable(label, sel)) m_editingSlot = i;
        }
    }
    ImGui::EndChild();
}

void FilterPresetPanel::renderSlotEditor() {
    auto& bank = ::PCSX::vj::filter::presetBank();
    auto& slot = bank.slot(m_editingSlot);

    ImGui::BeginGroup();
    ImGui::Text("Editing slot %d", m_editingSlot);

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", slot.name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        slot.name = nameBuf;
    }

    ImGui::Checkbox("Textured only", &slot.params.texturedOnly);
    ImGui::SliderFloat("Min area", &slot.params.minArea, 0.0f, 50000.0f, "%.0f");
    ImGui::SliderFloat("Max area", &slot.params.maxArea, 0.0f, 50000.0f, "%.0f");
    float region[4] = {
        slot.params.regionX0, slot.params.regionY0,
        slot.params.regionX1, slot.params.regionY1,
    };
    if (ImGui::SliderFloat4("Region X0/Y0/X1/Y1", region, 0.0f, 1024.0f, "%.0f")) {
        slot.params.regionX0 = region[0]; slot.params.regionY0 = region[1];
        slot.params.regionX1 = region[2]; slot.params.regionY1 = region[3];
    }
    ImGui::SliderInt("Every N", &slot.params.everyN, 0, 16);

    if (ImGui::Button("Capture live filter into this slot")) {
        slot.params = ::PCSX::vj::params().filter;
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply this slot to live filter")) {
        ::PCSX::vj::params().filter = slot.params;
    }
    if (ImGui::Button("Clear this slot")) {
        slot.params = ::vj::FilterParams{};
    }

    ImGui::EndGroup();
}

void FilterPresetPanel::draw(const char* title) {
    if (!m_show) return;

    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    // Save / Load row.
    {
        static char status[64] = "";
        const std::string defaultPath = ::PCSX::vj::filter::defaultBankPath();
        if (ImGui::Button("Save bank")) {
            const bool ok = ::PCSX::vj::filter::saveBank(defaultPath);
            std::snprintf(status, sizeof(status), "Save %s: %s",
                          defaultPath.c_str(), ok ? "OK" : "FAILED");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload bank")) {
            const bool ok = ::PCSX::vj::filter::loadBank(defaultPath);
            std::snprintf(status, sizeof(status), "Load %s: %s",
                          defaultPath.c_str(), ok ? "OK" : "FAILED");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("File: %s", defaultPath.c_str());
        if (status[0]) ImGui::TextDisabled("%s", status);
    }

    ImGui::Separator();
    renderMidiSection();
    ImGui::Separator();
    renderSlotList();
    ImGui::SameLine();
    renderSlotEditor();

    ImGui::End();
}

}  // namespace Widgets
}  // namespace PCSX
