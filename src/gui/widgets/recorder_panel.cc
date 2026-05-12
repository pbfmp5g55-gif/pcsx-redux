/***************************************************************************
 *   Recorder panel implementation. See recorder_panel.h.
 ***************************************************************************/

#include "gui/widgets/recorder_panel.h"

#include <cstdio>
#include <string>

#include "core/vj_bridge.h"

namespace PCSX {
namespace Widgets {

void RecorderPanel::draw(const char* title) {
    if (!m_show) return;

    ImGui::SetNextWindowSize(ImVec2(440, 220), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    const bool recording = ::PCSX::vj::record::isRecording();

    ImGui::TextUnformatted("Output file:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##path", m_pathBuf, sizeof(m_pathBuf),
                     recording ? ImGuiInputTextFlags_ReadOnly : 0);

    ImGui::Spacing();

    if (recording) {
        if (ImGui::Button("Stop recording", ImVec2(180, 0))) {
            ::PCSX::vj::record::stop();
        }
    } else {
        if (ImGui::Button("Start recording", ImVec2(180, 0))) {
            const std::string path = m_pathBuf[0] ? m_pathBuf : "vj-record.vjr";
            ::PCSX::vj::record::start(path);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (recording) {
        ImGui::Text("Status: RECORDING");
        const std::string p = ::PCSX::vj::record::currentPath();
        ImGui::Text("File:   %s", p.c_str());
        ImGui::Text("Frames: %llu",
                    static_cast<unsigned long long>(::PCSX::vj::record::recordedFrames()));
    } else {
        ImGui::TextDisabled("Status: idle");
        ImGui::TextDisabled("(captures the post-libvj primitive stream, frame by frame)");
    }

    ImGui::End();
}

}  // namespace Widgets
}  // namespace PCSX
