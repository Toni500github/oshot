/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "clipboard.hpp"
#include "config.hpp"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "helpers.hpp"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/spdlog.h"
#include "tiny-process-library/process.hpp"
#include "util.hpp"

// ------------------------------
// Windows
// ------------------------------

void ScreenshotTool::DrawAboutWindow()
{
    bool open = m_show_window.Has(SubWindow::About);
    if (!open)
        return;

    ImGui::SetNextWindowSize(font_relative(27, 19), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("About###about_window"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        const char* text_display;
        const float window_width  = ImGui::GetWindowSize().x;
        auto        centered_text = [&](const char* text) {
            float name_width = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX((window_width - name_width) / 2);
            return text;
        };

        // Centered image
        ImGui::SetCursorPosX((window_width - 32.0f) / 2);
        ImGui::Image(m_tool_textures[idx(ToolType::Logo)], ImVec2(32, 32));

        // Centered labels
        text_display = centered_text("oshot v" VERSION);
        ImGui::TextUnformatted(text_display);
        ImGui::Spacing();

        text_display = centered_text(_("Screenshot tool for extracting text on the fly"));
        ImGui::TextUnformatted(text_display);
        ImGui::Spacing();

#ifdef DISABLE_PLUGINS
        text_display = centered_text(_("!!! NO PLUGINS SUPPORT !!!"));
        ImGui::TextColored(Colors::RED, "%s", text_display);
        ImGui::Spacing();
#endif

        text_display = _("More Details:");
        if (ImGui::TreeNode(text_display))
        {
            ImGui::BeginChild("##scrollable_region",
                              ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6.0f),
                              false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(version_infos.c_str());
            CreateCopyTextButton(version_infos);
            ImGui::EndChild();
            ImGui::TreePop();
        }

        ImGui::TextUnformatted(i18n::tr_fmt("Version: v{}", VERSION).c_str());
        ImGui::TextUnformatted(i18n::tr_fmt("Created by: {}", "Toni500").c_str());
        ImGui::TextUnformatted("Copyright © 2026");
        ImGui::Spacing();

        ImGui::TextUnformatted(_("Support the project at "));
        ImGui::SameLine(0, 1);
        if (ImGui::TextLinkOpenURL("Toni500github/oshot", "https://github.com/Toni500github/oshot"))
            minimize_window();

        if (ImGui::Button(_("Close")))
            open = false;
    }
    ImGui::End();
    m_show_window.Set(SubWindow::About, open);
}

void ScreenshotTool::DrawDownloadOCRWindow()
{
    ErrorContext<OcrDownloadError>& ectx = m_download_errors;

    bool open = m_show_window.Has(SubWindow::OcrDownload);
    if (!open)
        return;

    static bool        has_downloaded = false;
    static std::string model_to_get   = "eng";
    static std::string prev_model;
    bool               refit = false;  // the result below the button appeared or changed

    // Reset success banner when the user targets a different model
    if (prev_model != model_to_get)
    {
        has_downloaded = false;
        prev_model     = model_to_get;
    }

    const bool is_downloading = m_ocr_download && m_ocr_download->running.load();

    ImGui::SetNextWindowSize(font_relative(40, 0), ImGuiCond_FirstUseEver);  // 0 = auto height
    if (ImGui::Begin(_("Download OCR Model###ocr_download_window"), &open, ImGuiWindowFlags_NoSavedSettings))
    {
        // Lock all inputs while a download is in flight
        if (is_downloading)
            ImGui::BeginDisabled();

        ImGui::SeparatorText(_("Source"));
        ImGui::TextDisabled("%s", _("GitHub repository containing Tesseract '.traineddata' models"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ocr_download_repo", &m_inputs.ocr_download_repo))
            ClearError(ectx, OcrDownloadError::FailedToDownload);

        ImGui::Spacing();

        ImGui::SeparatorText(_("Model"));
        ImGui::TextDisabled("%s", _("Language code (e.g eng, fra, deu, chi_sim)"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ocr_model_to_download", &model_to_get))
            ClearError(ectx, OcrDownloadError::FailedToDownload);

        ImGui::Spacing();

        ImGui::SeparatorText(_("Destination"));
        ImGui::SetNextItemWidth(-1);
        draw_input_text_folder(
            "",
            "##ocr_download_path",
            [&] {
                if (!fs::exists(m_inputs.ocr_model_downloaded_path))
                    SetError(ectx, OcrDownloadError::InvalidPath, _("No such directory or path"));
                else if (!fs::is_directory(m_inputs.ocr_model_downloaded_path))
                    SetError(ectx, OcrDownloadError::InvalidPath, _("Not a directory"));
                else
                    ClearError(ectx, OcrDownloadError::InvalidPath);
            },
            m_inputs.ocr_model_downloaded_path);
        ShowIfError(ectx, OcrDownloadError::InvalidPath);

        if (is_downloading)
            ImGui::EndDisabled();

        // URL preview when both fields are filled
        if (!m_inputs.ocr_download_repo.empty() && !model_to_get.empty())
        {
            ImGui::Spacing();
            ImGui::TextDisabled("https://raw.githubusercontent.com/%s/main/%s.traineddata",
                                m_inputs.ocr_download_repo.c_str(),
                                model_to_get.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Determine why the button should be disabled (if at all)
        const bool fields_filled =
            !m_inputs.ocr_download_repo.empty() && !model_to_get.empty() && !m_inputs.ocr_model_downloaded_path.empty();
        // A failed download can be retried as it is, e.g. after a network error
        const bool can_download = !m_ocr_download && fields_filled && !HasError(ectx, OcrDownloadError::InvalidPath);

        if (!can_download)
            ImGui::BeginDisabled();

        if (ImGui::Button(_("Download"), ImVec2(-1, 0)))
        {
            ClearError(ectx, OcrDownloadError::FailedToDownload);
            has_downloaded = false;
            refit          = true;
            std::vector<std::string> cmd{ "curl",
                                          "-fL",
                                          fmt::format("https://raw.githubusercontent.com/{}/main/{}.traineddata",
                                                      m_inputs.ocr_download_repo,
                                                      model_to_get),
                                          "-o",
                                          fmt::format(
                                              "{}/{}.traineddata", m_inputs.ocr_model_downloaded_path, model_to_get) };

            m_ocr_download = std::make_shared<ocr_download_t>();

            std::thread([cmd = std::move(cmd), dl = m_ocr_download]() mutable {
                TinyProcessLib::Process proc(
                    cmd,
                    "",
                    [](const char*, size_t) { /* stdout: unused */ },
                    [&dl](const char* buf, size_t n) {
                        std::scoped_lock g(dl->err_mutex);

                        dl->line_buf.append(buf, n);

                        size_t pos = 0;
                        while (pos < dl->line_buf.size())
                        {
                            // curl progress uses \r to overwrite the same line.
                            // Format: "  25  1234k   25   308k  ..."
                            // First integer on a data line is the overall percentage
                            size_t end = dl->line_buf.find_first_of("\r\n", pos);
                            if (end == std::string::npos)
                                break;  // incomplete line, waiting for more data

                            if (end > pos)
                            {
                                const std::string_view line(dl->line_buf.c_str() + pos, end - pos);
                                int                    pct = -1;
                                if (sscanf(line.data(), " %d", &pct) == 1 && pct >= 0 && pct <= 100)  // NOLINT
                                {
                                    dl->progress.store(static_cast<float>(pct));
                                }
                                // Skip curl's two progress-table header lines
                                else if (line.find("% Total") == line.npos && line.find("Dload") == line.npos)
                                {
                                    dl->err.append(line);
                                    dl->err.push_back('\n');
                                }
                            }
                            pos = end + 1;
                        }

                        // Keep only the unprocessed tail
                        dl->line_buf.erase(0, pos);
                    });

                dl->exit_code.store(proc.get_exit_status());
                dl->running.store(false);
            }).detach();
        }

        if (!can_download)
            ImGui::EndDisabled();

        // Tooltip on the disabled button explaining why
        if (!can_download && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            if (m_ocr_download)  // also right after the click, before the thread marks itself running
                ImGui::TextUnformatted(_("Download already in progress"));
            else if (!fields_filled)
                ImGui::TextUnformatted(_("Fill all fields first"));
            else
                ImGui::TextUnformatted(_("Fix the destination path error first"));
            ImGui::EndTooltip();
        }

        // Progress bar
        if (m_ocr_download)
        {
            if (m_ocr_download->running.load())
            {
                ImGui::Spacing();
                const float pct = m_ocr_download->progress.load();
                if (pct < 0.f)
                {
                    // Size unknown yet, so show a marquee bar
                    const float t = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.8f, 1.0f);
                    ImGui::ProgressBar(-1.f * t, ImVec2(-1.f, 0.f), _("Downloading..."));
                }
                else if (g_config->theme_overrides.smooth_animations)
                {
                    float& dp = m_ocr_download->display_progress;
                    dp += (pct - dp) * std::min(1.f, 6.f * ImGui::GetIO().DeltaTime);
                    ImGui::ProgressBar(dp / 100.f, ImVec2(-1.f, 0.f), fmt::format("{:.2f}%", dp).c_str());
                }
                else
                {
                    ImGui::ProgressBar(pct / 100.f, ImVec2(-1.f, 0.f), fmt::format("{:.0f}%", pct).c_str());
                }
            }
            else
            {
                const int code = m_ocr_download->exit_code.load();
                if (code != 0)
                {
                    std::string err;
                    {
                        std::scoped_lock g(m_ocr_download->err_mutex);
                        err = std::move(m_ocr_download->err);
                    }
                    SetError(ectx,
                             OcrDownloadError::FailedToDownload,
                             i18n::tr_fmt("Failed to download '{}' to '{}':\n{}",
                                          model_to_get,
                                          m_inputs.ocr_model_downloaded_path,
                                          err));
                }
                else
                {
                    ClearError(ectx, OcrDownloadError::FailedToDownload);
                    has_downloaded = true;
                    // Rescan, so the new model is in the Model list
                    m_last_scanned_ocr_path.clear();
                    RefreshOcrModels();
                }
                m_ocr_download.reset();
                refit = true;
            }
        }

        ImGui::Spacing();
        ShowIfError(ectx, OcrDownloadError::FailedToDownload);
        if (has_downloaded && !HasError(ectx, OcrDownloadError::FailedToDownload))
        {
            ImGui::TextColored((0x33D933FF_rgba), "%s", _("Downloaded successfully!"));

            // The Model list only shows the OCR path's models, often a system folder oshot can't write to
            std::error_code ec;
            if (!fs::equivalent(m_inputs.ocr_path, m_inputs.ocr_model_downloaded_path, ec))
            {
                ImGui::TextWrapped("%s",
                                   _("The OCR path is another folder, so the model isn't in the Model list yet."));
                if (ImGui::Button(_("Use this folder")))
                {
                    m_inputs.ocr_path  = m_inputs.ocr_model_downloaded_path;
                    m_inputs.ocr_model = model_to_get;
                    RefreshOcrModels();
                    refit = true;
                }
            }
        }

        // Its height was fitted when it opened, so grow it to show the error or the progress bar
        if (refit)
            ImGui::SetWindowSize(ImVec2(ImGui::GetWindowWidth(), 0.0f));
    }
    ImGui::End();

    m_show_window.Set(SubWindow::OcrDownload, open);
}

void ScreenshotTool::DrawLogsWindow()
{
    bool open = m_show_window.Has(SubWindow::Logs);
    if (!open || !g_imgui_log_sink)
        return;

    auto level_color = [](spdlog::level::level_enum lvl) {
        switch (lvl)
        {
            case spdlog::level::trace:    return 0x888888FF_rgba;  // gray
            case spdlog::level::debug:    return 0x9999FFFF_rgba;  // periwinkle
            case spdlog::level::info:     return 0x00AEFFFF_rgba;  // blueish
            case spdlog::level::warn:     return 0xFFCC33FF_rgba;  // amber
            case spdlog::level::err:      return 0xFF4D4DFF_rgba;  // red
            case spdlog::level::critical: return Colors::RED;      // pure red

            default: return Colors::WHITE;
        }
    };

    auto level_tag = [](spdlog::level::level_enum lvl) {
        switch (lvl)
        {
            case spdlog::level::trace:    return "TRACE";
            case spdlog::level::debug:    return "DEBUG";
            case spdlog::level::info:     return "INFO";
            case spdlog::level::warn:     return "WARN";
            case spdlog::level::err:      return "ERROR";
            case spdlog::level::critical: return "CRITICAL";
            default:                      return "OFF";
        }
    };

    // Persistent UI state for this window
    static ImGuiTextFilter               text_filter;
    static spdlog::level::level_enum     min_level  = spdlog::level::debug;
    static bool                          autoscroll = true;
    static spdlog::log_clock::time_point cleared_before{};  // "soft clear" marker

    ImGui::SetNextWindowSize(font_relative(43, 31), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Logs###logs_window"), &open, ImGuiWindowFlags_NoSavedSettings))
    {
        // --- Toolbar ---
        // The log lines and their level tags stay English, like the log file
        if (ImGui::Button(_("Clear")))
            cleared_before = spdlog::log_clock::now();

        ImGui::SameLine();
        bool copy_all = ImGui::Button(_("Copy All"));
        ImGui::SameLine();
        ImGui::Checkbox(_("Auto-scroll"), &autoscroll);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(button_width({ level_tag(spdlog::level::critical) }) + ImGui::GetFrameHeight());
        if (ImGui::BeginCombo("##min_level", level_tag(min_level)))
        {
            for (int i = spdlog::level::trace; i < spdlog::level::off; ++i)
            {
                auto lvl      = toe<spdlog::level::level_enum>(i);
                bool selected = (lvl == min_level);
                if (ImGui::Selectable(level_tag(lvl), selected))
                    min_level = lvl;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        text_filter.Draw(_("Filter"), 200);

        ImGui::Separator();

        // --- Build filtered view ---
        const std::vector<spdlog::details::log_msg_buffer>& all = g_imgui_log_sink->last_raw();
        std::vector<const spdlog::details::log_msg_buffer*> shown;
        shown.reserve(all.size());
        for (const auto& msg : all)
        {
            if (msg.level < min_level || msg.time <= cleared_before)
                continue;
            if (!text_filter.PassFilter(msg.payload.data(), msg.payload.data() + msg.payload.size()))
                continue;
            shown.push_back(&msg);
        }

        // --- Scrolling log region ---
        ImGui::BeginChild("##log_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto&        msg      = *shown[i];
                auto               time_ms  = std::chrono::time_point_cast<std::chrono::milliseconds>(msg.time);
                const std::string& time_fmt = fmt::format("{:%H:%M:%S}", time_ms);

                ImGui::TextDisabled("%s", time_fmt.c_str());
                ImGui::SameLine();
                ImGui::TextColored(level_color(msg.level), "[%s]", level_tag(msg.level));
                ImGui::SameLine();
                ImGui::TextUnformatted(msg.payload.data(), msg.payload.data() + msg.payload.size());

                if (ImGui::BeginPopupContextItem(fmt::format("ctx##{}", i).c_str()))
                {
                    if (ImGui::MenuItem(_("Copy line")))
                        MUST_OK(g_clipboard.CopyText(std::string(msg.payload.data(), msg.payload.size())),
                                error("Failed to copy line log: {}", _r.error_v()));
                    ImGui::EndPopup();
                }
            }
        }

        if (copy_all)
        {
            std::string all_text;
            for (const auto* msg : shown)
                all_text += fmt::format("[{}] {}\n", level_tag(msg->level), msg->payload);
            MUST_OK(g_clipboard.CopyText(all_text), error("Failed to copy logs: {}", _r.error_v()));
        }

        if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
    }
    ImGui::End();

    m_show_window.Set(SubWindow::Logs, open);
}

void ScreenshotTool::DrawOutputMenuSelection()
{
#if OSHOT_FREEDESKTOP
    if (!m_show_window.Has(SubWindow::OutputMenuSelection))
        return;

    static int output_sel = 0;
    static int trans_sel  = 0;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 10));

    static ImVec2 menu_display;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), center_cond(menu_display), ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(font_relative(25, 0), font_relative(40, 37));
    ImGui::Begin(_("Choose an output to capture###select_output_crop"),
                 nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    std::deque<region_t> layout;

    for (size_t i = 0; i < m_wayland_monitors.size(); ++i)
    {
        const monitor_t& m = m_wayland_monitors[i];
        layout.push_back(m.geo);

        ImGui::PushID(int(i));
        ImGui::RadioButton(
            fmt::format("{} ({}x{})", m.name[0] ? m.name.data() : _("Unknown"), m.geo.w, m.geo.h).c_str(),
            &output_sel,
            int(i));
        ImGui::PopID();
    }

    output_sel              = layout.empty() ? 0 : std::clamp(output_sel, 0, static_cast<int>(layout.size()) - 1);
    const monitor_t& target = m_wayland_monitors[output_sel];

    ImGui::Spacing();
    ImGui::TextDisabled(_("Detected transform: %d"), target.transform);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const auto rotations =
        std::to_array({ _("No Rotation"), _("Left Rotation"), _("Down Rotation"), _("Right Rotation") });
    ImGui::Combo("##rotate_combo", &trans_sel, rotations.data(), int(rotations.size()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          _("Overrides the auto-detected output rotation for this crop.\n"
                            "Should normally stay \"No Rotation\"."));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float button_width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(layout.empty());
    if (ImGui::Button(_("Crop"), ImVec2(button_width, 0)))
    {
        m_show_window.Clear(SubWindow::OutputMenuSelection);
        MUST_OK(g_ss_tool.CropToOutput(layout, target, trans_sel),
                error("Crop to focused monitor failed: {}", _r.error_v()));
    }
    ImGui::EndDisabled();

    ImGui::End();
    ImGui::PopStyleVar(2);
#endif
}
