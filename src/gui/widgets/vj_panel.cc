/***************************************************************************
 *   VJ subsystem control panel (imgui). See vj_panel.h.
 ***************************************************************************/

#include "gui/widgets/vj_panel.h"

#include "core/vj_bridge.h"
#include "vj/AutoMode.h"
#include "vj/Params.h"

namespace PCSX {
namespace Widgets {

void VJPanel::draw(const char* title) {
    if (!m_show) return;

    ImGui::SetNextWindowSize(ImVec2(360, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    bool enabled = ::PCSX::vj::isEnabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        ::PCSX::vj::setEnabled(enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(unchecking = bypass interceptor entirely)");

    ImGui::Separator();

    auto& p = ::PCSX::vj::params();
    ImGui::TextUnformatted("Live glitch params (0..1)");
    ImGui::SliderFloat("MASTER",   &p.master,   0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("CHANCE",   &p.chance,   0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("GEOMETRY", &p.geometry, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("TEXTURE",  &p.texture,  0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("MISSING",  &p.missing,  0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("COLOR",    &p.color,    0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("DEPTH",    &p.depth,    0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("CHAOS",    &p.chaos,    0.0f, 1.0f, "%.2f");

    ImGui::Separator();

    auto& a = ::PCSX::vj::autoParams();
    ImGui::TextUnformatted("Auto-Mode (per-axis sine LFO)");
    ImGui::Checkbox("Auto enabled", &a.enabled);
    ImGui::SliderFloat("Auto depth", &a.depth, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Auto rate",  &a.rate,  0.1f, 4.0f, "%.2f");

    ImGui::Separator();

    if (ImGui::Button("Reset all")) {
        p = ::vj::Params{};
        a = ::vj::AutoModeParams{};
    }
    ImGui::SameLine();
    if (ImGui::Button("Demo: subtle")) {
        p.master   = 0.30f;
        p.chance   = 0.50f;
        p.geometry = 0.40f;
        p.color    = 0.20f;
        p.missing  = 0.05f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Demo: chaos")) {
        p.master   = 0.80f;
        p.chance   = 0.90f;
        p.geometry = 0.80f;
        p.texture  = 0.50f;
        p.missing  = 0.30f;
        p.color    = 0.60f;
        p.chaos    = 0.70f;
    }

    ImGui::Separator();
    ImGui::Text("Last frame primitives: %llu", ::PCSX::vj::lastFramePrimitiveCount());

    ImGui::End();
}

}  // namespace Widgets
}  // namespace PCSX
