/***************************************************************************
 *   Live IPC panel implementation. See live_panel.h.
 ***************************************************************************/

#include "gui/widgets/live_panel.h"

#include <string>

#include "core/vj_bridge.h"

namespace PCSX {
namespace Widgets {

void LivePanel::draw(const char* title) {
    if (!m_show) return;

    ImGui::SetNextWindowSize(ImVec2(420, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    const bool active = ::PCSX::vj::live::isActive();

    ImGui::TextUnformatted("Shared-memory ring name (Windows):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", m_nameBuf, sizeof(m_nameBuf),
                     active ? ImGuiInputTextFlags_ReadOnly : 0);

    ImGui::Spacing();
    if (active) {
        if (ImGui::Button("Stop live", ImVec2(180, 0))) {
            ::PCSX::vj::live::stop();
        }
    } else {
        if (ImGui::Button("Start live", ImVec2(180, 0))) {
            const std::string name = m_nameBuf[0] ? m_nameBuf : "Local\\vj-mix-prim-A";
            ::PCSX::vj::live::start(name);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (active) {
        ImGui::Text("Status: ACTIVE");
        const std::string n = ::PCSX::vj::live::currentName();
        ImGui::Text("Ring:   %s", n.c_str());
        ImGui::TextDisabled("(mixer side reads the same name.");
        ImGui::TextDisabled(" Records: primitives + VRAM uploads + FrameEnd)");
    } else {
        ImGui::TextDisabled("Status: idle");
        ImGui::TextDisabled("(start to expose the primitive stream over IPC)");
    }

    ImGui::End();
}

}  // namespace Widgets
}  // namespace PCSX
