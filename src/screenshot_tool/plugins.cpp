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
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "clipboard.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/spdlog.h"
#include "text_extraction.hpp"
#include "util.hpp"
#include "util/color.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/window.hpp"
#ifndef DISABLE_PLUGINS
#  include "manifest.hh"
#  include "plugin.hpp"
#  include "state_manager.hh"
#endif
#include "helpers.hpp"

using namespace std::chrono_literals;

// ------------------------------
// Plugins
// ------------------------------

#ifndef DISABLE_PLUGINS

// A small rounded label, e.g. a license or a status
static void draw_tag(const char* label, const rgba_t col)
{
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 pad(6.0f, 2.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(text_size.x + pad.x * 2, text_size.y + pad.y * 2);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), col.to_abgr(), 4.0f);
    draw_list->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), Colors::WHITE.to_abgr(), label);

    ImGui::Dummy(size);
}

// What a plugin declares it does on its own, as tags with the reason in their tooltip.
// `same_line` continues the current line
static void draw_capability_tags(const std::map<std::string, std::string>& capabilities, bool same_line)
{
    for (const auto& [capability, reason] : capabilities)
    {
        if (same_line)
            ImGui::SameLine();
        same_line = true;

        const char* label = capability == "network"     ? _("Uses the internet")
                            : capability == "files"     ? _("Uses your files")
                            : capability == "processes" ? _("Runs programs")
                                                        : capability.c_str();
        draw_tag(label, 0x725919FF_rgba);
        ImGui::SetItemTooltip(
            "%s\n\n%s", reason.c_str(), _("According to its author. oshot can't check or limit what plugins do."));
    }
}

// Right-aligns the next item of width `w` on the current line
static void align_right(const float w)
{
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - w + ImGui::GetCursorPosX());
}

static float button_width(const char* label)
{
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
}

void ScreenshotTool::FetchIndexAsync()
{
    {
        std::scoped_lock lock(m_index_state->mutex);
        if (m_index_state->loading)
            return;
        m_index_state->loading = true;
    }
    if (m_index_thread.joinable())
        m_index_thread.join();

    m_index_thread = std::thread([state = m_index_state]() {
        bool                   stale = false;
        Result<plugin_index_t> index = PluginManager::FetchIndex(/*allow_cached=*/true, &stale);

        std::scoped_lock lock(state->mutex);
        state->loading = false;
        state->loaded  = true;
        state->stale   = stale;
        if (index.ok())
        {
            state->index = std::move(index.get());
            state->error.clear();
        }
        else
        {
            state->error = index.error_v();
        }
    });
}

void ScreenshotTool::StartPluginTask(const std::string& title, std::function<Result<>(PluginManager&)> task)
{
    // The buttons that start a task are disabled while one runs, this is just a safety net
    if (m_install_state && m_install_state->running)
        return;

    if (m_install_thread.joinable())
        m_install_thread.join();

    auto state = std::make_shared<plugin_install_state_t>();

    // The callbacks only append to the shared queue and never touch ImGui,
    // so the worker thread can call them
    auto push = [state](plugin_install_event_t::Kind kind) {
        return [state, kind](const std::string_view msg) {
            std::scoped_lock lock(state->events_mutex);
            state->pending_events.push_back({ .kind = kind, .text = std::string(msg) });
        };
    };

    PluginCallbacks cb;
    cb.on_status  = push(plugin_install_event_t::Kind::Status);
    cb.on_success = push(plugin_install_event_t::Kind::Success);
    cb.on_warning = push(plugin_install_event_t::Kind::Warning);
    cb.on_error   = push(plugin_install_event_t::Kind::Error);
    cb.on_info    = push(plugin_install_event_t::Kind::Info);
    cb.on_output  = push(plugin_install_event_t::Kind::Output);

    // Blocks the worker thread until DrawPluginInstallStatus() answers the
    // popup on the render thread and notifies confirm_cv.
    cb.confirm = [state](const std::string_view prompt, bool) -> bool {
        std::unique_lock lock(state->confirm_mutex);
        state->confirm_prompt   = std::string(prompt);
        state->confirm_pending  = true;
        state->confirm_answered = false;
        state->confirm_cv.wait(lock, [&] { return state->confirm_answered; });
        state->confirm_pending = false;
        return state->confirm_answer;
    };
    m_plugin_manager.SetCallbacks(cb);

    m_install_events.clear();
    m_install_state = state;
    m_show_window.Set(SubWindow::PluginInstallStatus);

    m_install_thread = std::thread([this, state, title, task = std::move(task)]() {
        {
            std::scoped_lock lock(state->events_mutex);
            state->pending_events.push_back({ .kind = plugin_install_event_t::Kind::Info, .text = title });
        }

        Result<> r = task(m_plugin_manager);
        {
            std::scoped_lock lock(state->events_mutex);
            if (r.ok())
                state->pending_events.push_back({ .kind = plugin_install_event_t::Kind::Success,
                                                  .text = _("Done. Changes apply the next time oshot starts.") });
            else
                state->pending_events.push_back({ .kind = plugin_install_event_t::Kind::Error, .text = r.error_v() });
        }
        state->running = false;
    });
}

void ScreenshotTool::StartPluginsCapture()
{
    m_plugin_selection      = {};
    m_show_ocr_original     = false;
    m_show_barcode_original = false;
    set_plugin_overlay_open(true);
    for (auto& [id, rt] : g_plugins)
        rt.CaptureStart();
}

void ScreenshotTool::EndPluginsCapture()
{
    // Results reported from here on, or by threads still running, become notifications
    set_plugin_overlay_open(false);
    for (auto& [id, rt] : g_plugins)
        rt.CaptureEnd();
}

void ScreenshotTool::NotifyPluginsOfOcr(const ocr_result_t* res)
{
    // Copied: a plugin may call set_text(OSHOT_TEXT_OCR) from on_ocr_done()
    const std::string             text = res ? res->data : std::string();
    std::vector<oshot_ocr_word_t> words;
    if (res)
    {
        words.reserve(res->words.size());
        for (const ocr_word_t& w : res->words)
            words.push_back({ .text       = { .p = w.text.c_str(), .len = w.text.length() },
                              .x          = w.x,
                              .y          = w.y,
                              .w          = w.w,
                              .h          = w.h,
                              .confidence = w.confidence,
                              .line       = w.line,
                              .block      = w.block });
    }

    const oshot_ocr_result_t ocr{
        .text       = { .p = text.c_str(), .len = text.length() },
        .confidence = res ? res->confidence : -1,
        .psm        = res ? res->psm : -1,
        .words      = words.data(),
        .n_words    = words.size(),
    };
    for (auto& [id, rt] : g_plugins)
    {
        // Plugins built before on_barcodes_done expect on_ocr_done() only with text
        if (!rt.plugin->on_ocr_done || (!res && !OSHOT_PLUGIN_HAS(rt.plugin, on_barcodes_done)))
            continue;
        rt.plugin->on_ocr_done(rt.state, &ocr);
    }
}

void ScreenshotTool::NotifyPluginsOfSelection()
{
    // Selected and not being dragged (a drag owns the input until the mouse is released)
    const bool has_selection = m_main_sel.selection.get_width() > 0 && m_main_sel.selection.get_height() > 0;
    if (m_state != ToolState::Selected || m_input_owner == InputOwner::Selection || !has_selection)
        return;

    const region_t sel = GetActiveRegion();
    if (sel == m_plugin_selection)
        return;
    m_plugin_selection = sel;

    for (auto& [id, rt] : g_plugins)
        if (OSHOT_PLUGIN_HAS(rt.plugin, on_selection_changed) && rt.plugin->on_selection_changed)
            rt.plugin->on_selection_changed(rt.state, float(sel.x), float(sel.y), float(sel.w), float(sel.h));
}

void ScreenshotTool::TransformText(std::string& text, std::string& original, const char* buffer_id)
{
    original = text;
    text     = run_text_transformers(text, buffer_id);
    // Nothing to show under the Original toggle
    if (text == original)
        original.clear();
}

void ScreenshotTool::RunPluginAction(plugin_runtime_t& rt, int32_t action)
{
    plugin_runtime_t* p = &rt;
    std::scoped_lock  lock(rt.tasks_mutex);
    rt.tasks.emplace_back([p, action] {
        // It may have been unregistered since the click
        const plugin_action_t* a = p->FindAction(action);
        if (!a)
            return;
        const auto fn   = a->fn;
        void*      user = a->user;
        fn(user, action);
    });
}

// The label's first character, upper-cased if it's ASCII, for a button without an icon
static std::string first_letter(const std::string& label)
{
    size_t len = 1;
    while (len < label.size() && (static_cast<unsigned char>(label[len]) & 0xC0) == 0x80)
        ++len;
    std::string ret = label.substr(0, len);
    if (ret.size() == 1)
        ret[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ret[0])));
    return ret;
}

void ScreenshotTool::DrawPluginToolbarButtons(bool same_line)
{
    for (auto& [id, rt] : g_plugins)
    {
        for (const plugin_action_t& action : rt.actions)
        {
            if (action.place != OSHOT_PLACE_TOOLBAR)
                continue;

            // Made on first use in each overlay. On macOS CreateTexture() makes none, so the letter is drawn
            const auto key = std::make_pair(id, action.id);
            auto       it  = m_plugin_icons.find(key);
            if (it == m_plugin_icons.end() && !action.icon.empty())
            {
                const Result<ImTextureRef>& tex = CreateTexture(0, action.icon, action.icon_w, action.icon_h);
                it = m_plugin_icons.emplace(key, tex.ok() ? tex.get() : ImTextureRef{}).first;
            }
            const ImTextureID tex_id = it != m_plugin_icons.end() ? it->second.GetTexID() : ImTextureID{};

            ImGui::PushID(id.c_str());
            ImGui::PushID(action.id);
            const bool clicked = tex_id ? ImGui::ImageButton("##btn", tex_id, TOOL_BUTTON_SIZE2)
                                        : ImGui::Button(first_letter(action.label).c_str(), GetToolButtonSize());
            ImGui::SetItemTooltip("%s", action.label.c_str());
            ImGui::PopID();
            ImGui::PopID();

            if (clicked)
                RunPluginAction(rt, action.id);
            if (same_line)
                ImGui::SameLine();
        }
    }
}

void ScreenshotTool::DrawPluginActionsMenu()
{
    size_t n_plugins = 0;
    for (const auto& [id, rt] : g_plugins)
        if (std::ranges::count(rt.actions, OSHOT_PLACE_TEXT_TOOLS, &plugin_action_t::place) > 0)
            ++n_plugins;
    if (n_plugins == 0 || !ImGui::BeginMenu(_("Plugins")))
        return;

    for (auto& [id, rt] : g_plugins)
    {
        bool named = false;
        for (const plugin_action_t& action : rt.actions)
        {
            if (action.place != OSHOT_PLACE_TEXT_TOOLS)
                continue;
            // With several plugins, each one's entries go under its name
            if (n_plugins > 1 && !std::exchange(named, true))
                ImGui::SeparatorText(rt.plugin->name);

            ImGui::PushID(id.c_str());
            ImGui::PushID(action.id);
            if (ImGui::MenuItem(action.label.c_str()))
                RunPluginAction(rt, action.id);
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    ImGui::EndMenu();
}

void ScreenshotTool::DrawPluginReports()
{
    // A result without anything to copy goes away after this; one with something stays until closed
    constexpr auto RESULT_SHOWN = 10s;

    struct line_t
    {
        plugin_runtime_t* rt;
        plugin_report_t   report;
    };

    // Copied, as plugin threads may report while it's drawn
    const auto          now = std::chrono::steady_clock::now();
    std::vector<line_t> lines;
    for (auto& [id, rt] : g_plugins)
    {
        std::scoped_lock lock(rt.reports_mutex);
        std::erase_if(rt.reports, [&](const plugin_report_t& r) {
            return r.done && r.copyable.empty() && now - r.finished > RESULT_SHOWN;
        });
        for (const plugin_report_t& r : rt.reports)
            lines.push_back({ .rt = &rt, .report = r });
    }
    if (lines.empty())
        return;

    constexpr float margin  = 10.0f;
    const ImVec2    display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - margin, display.y - margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::Begin("##plugin_reports",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    // Above the text tools when they reach the corner, without taking the focus from them
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // The line whose Copy was pressed, which says "Copied!" for a moment
    static std::pair<std::string, int32_t> copied;
    static bool                            armed = false;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto& [rt, r] = lines[i];
        if (i > 0)
            ImGui::Separator();

        ImGui::PushID(rt->id.c_str());
        ImGui::PushID(r.action);

        const plugin_action_t* action = rt->FindAction(r.action);
        if (action)
            ImGui::Text("%s: %s", rt->plugin->name, action->label.c_str());
        else
            ImGui::TextUnformatted(rt->plugin->name);

        if (!r.done)
        {
            // A negative fraction makes ImGui animate it as unknown
            const float fraction = r.fraction < 0.0f ? -1.0f * float(ImGui::GetTime()) : r.fraction;
            ImGui::ProgressBar(fraction, font_relative(16, 0), r.text.empty() ? nullptr : r.text.c_str());
        }
        else
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextColored(r.ok ? Colors::GREEN : Colors::RED, "%s", r.text.c_str());
            if (!r.copyable.empty())
                ImGui::TextDisabled("%s", r.copyable.c_str());
            ImGui::PopTextWrapPos();

            if (!r.copyable.empty())
            {
                const auto key        = std::make_pair(rt->id, r.action);
                bool       line_armed = armed && copied == key;
                if (create_timed_button(_("Copy"), _("Copied!"), line_armed))
                {
                    const Result<>& res = g_clipboard.CopyText(r.copyable);
                    copied              = key;
                    armed               = res.ok();
                    if (!res.ok())
                        spdlog::error("Failed to copy {}'s result: {}", rt->id, res.error_v());
                }
                else if (copied == key)
                    armed = line_armed;  // cleared by create_timed_button() once its time is up
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(_("Close")))
            {
                std::scoped_lock lock(rt->reports_mutex);
                std::erase_if(rt->reports, [&](const plugin_report_t& rep) { return rep.action == r.action; });
            }
        }

        ImGui::PopID();
        ImGui::PopID();
    }
    ImGui::End();
}

void ScreenshotTool::DrawOriginalToggle(const std::string& original, bool& show)
{
    if (original.empty())
    {
        show = false;
        return;
    }
    ImGui::Checkbox(_("Original"), &show);
    ImGui::SameLine();
    HelpMarker(_("Show the text as it was read, before the plugins' text transformers changed it"));
}

void ScreenshotTool::DrawManagePluginsWindow()
{
    static bool was_open = false;
    bool        open     = m_show_window.Has(SubWindow::ManagePlugins);
    if (!open)
    {
        was_open = false;
        return;
    }

    const bool busy = m_install_state && m_install_state->running;

    // oshotpm may have changed state.toml since oshot started
    if (!was_open && !busy)
        m_plugin_manager.GetStateManager().Reload();
    was_open = true;

    ImGui::SetNextWindowSize(font_relative(48, 37), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Manage plugins###manage_plugins_window"), &open, ImGuiWindowFlags_NoSavedSettings))
    {
        if (busy)
        {
            ImGui::TextDisabled("%s", _("Working, see the Plugins status window. This list refreshes once it's done."));
            ImGui::End();
            m_show_window.Set(SubWindow::ManagePlugins, open);
            return;
        }

        if (ImGui::Button(_("Browse plugins...")))
            m_show_window.Set(SubWindow::BrowsePlugins);
        ImGui::SameLine();
        if (ImGui::Button(_("Install from a URL or file...")))
            m_show_window.Set(SubWindow::InstallPlugins);
        ImGui::SameLine();
        if (ImGui::Button(_("Check for updates")))
            StartPluginTask(_("Updating plugins"), [](PluginManager& pm) { return pm.UpdateAll(); });

        ImGui::PushStyleColor(ImGuiCol_Text, (0x999999FF_rgba));
        ImGui::TextWrapped("%s",
                           _("Changes apply the next time oshot starts. If oshot runs in the tray, quit it from the "
                             "tray menu first."));
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        // Deferred until after the loops, which iterate the state they'd change
        static std::string pending_uninstall;  // plugin ID or repository name
        static std::string pending_label;
        std::string        toggle_id;
        bool               toggle_to       = false;
        bool               request_confirm = false;

        const std::vector<manifest_t>& repos = m_plugin_manager.GetStateManager().GetAllRepos();
        if (repos.empty())
            ImGui::TextDisabled("%s", _("No plugins installed."));

        for (const manifest_t& repo : repos)
        {
            ImGui::PushID(repo.name.c_str());

            if (ImGui::CollapsingHeader(
                    repo.name.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                ImGui::Indent();

                if (!repo.url.empty())
                {
                    ImGui::TextDisabled("%s", _("Homepage:"));
                    ImGui::SameLine(0, 4);
                    if (ImGui::TextLinkOpenURL(repo.url.c_str(), repo.url.c_str()))
                        minimize_window();
                }
                if (repo.source == RepoSource::LocalPlugin)
                    ImGui::TextDisabled("%s",
                                        _("Installed from a folder or an archive: install it again to update it"));
                else if (!repo.git_hash.empty())
                    ImGui::TextDisabled(_("Commit %.7s"), repo.git_hash.c_str());

                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                     button_width(_("Uninstall all")));
                if (ImGui::Button(_("Uninstall all")))
                {
                    const auto count  = long(repo.plugins.size());
                    pending_uninstall = repo.name;
                    pending_label =
                        i18n::tr_n_fmt("'{}' and its {} plugin", "'{}' and its {} plugins", count, repo.name, count);
                    request_confirm = true;
                }
                ImGui::Spacing();

                for (const plugin_t& plugin : repo.plugins)
                {
                    ImGui::PushID(plugin.id.c_str());

                    std::error_code ec;
                    const bool      missing = plugin.library.empty() || !fs::exists(plugin.library, ec);
                    const bool      loaded  = g_plugins.contains(plugin.id);

                    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, (0xffffff07_rgba));
                    ImGui::PushStyleColor(ImGuiCol_Border, (0xffffff14_rgba));
                    ImGui::BeginChild("card",
                                      ImVec2(0, 0),
                                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                      ImGuiWindowFlags_NoScrollbar);

                    ImGui::TextUnformatted(plugin.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", plugin.id.c_str());

                    const char* tag     = _("Disabled");
                    rgba_t      tag_col = 0x725919FF_rgba;
                    if (missing)
                    {
                        tag     = _("Missing");
                        tag_col = 0x8c2626FF_rgba;
                    }
                    else if (plugin.enabled)
                    {
                        tag     = loaded ? _("Enabled") : _("Not loaded");
                        tag_col = loaded ? 0x197233FF_rgba : 0x3f3f3fFF_rgba;
                    }
                    align_right(ImGui::CalcTextSize(tag).x + 12.0f);
                    draw_tag(tag, tag_col);

                    if (!plugin.description.empty())
                        ImGui::TextWrapped("%s", plugin.description.c_str());
                    if (!plugin.authors.empty())
                        ImGui::TextDisabled(_("By %s"), fmt::format("{}", fmt::join(plugin.authors, ", ")).c_str());

                    if (!plugin.licenses.empty())
                    {
                        ImGui::TextDisabled("%s", _("License:"));
                        for (const std::string& license : plugin.licenses)
                        {
                            ImGui::SameLine();
                            draw_tag(license.c_str(), 0x334c7fFF_rgba);
                        }
                    }
                    draw_capability_tags(plugin.capabilities, /*same_line=*/!plugin.licenses.empty());

                    if (missing)
                        ImGui::TextColored(Colors::RED, "%s", _("The library is missing: reinstall the plugin"));
                    else if (plugin.enabled && !loaded)
                        ImGui::TextDisabled("%s",
                                            _("Not loaded: restart oshot, or see View Logs if it failed to load"));

                    const char* toggle_label = plugin.enabled ? _("Disable") : _("Enable");
                    align_right(button_width(toggle_label) + button_width(_("Uninstall")) +
                                ImGui::GetStyle().ItemSpacing.x);
                    if (ImGui::Button(toggle_label))
                    {
                        toggle_id = plugin.id;
                        toggle_to = !plugin.enabled;
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, (0x8C1F1FFF_rgba));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (0xB23333FF_rgba));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, (0x730F0FFF_rgba));
                    if (ImGui::Button(_("Uninstall")))
                    {
                        pending_uninstall = plugin.id;
                        pending_label     = fmt::format("'{}' ({})", plugin.name, plugin.id);
                        request_confirm   = true;
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::EndChild();
                    ImGui::PopStyleColor(2);
                    ImGui::PopStyleVar();
                    ImGui::Spacing();
                    ImGui::PopID();
                }

                ImGui::Unindent();
            }

            ImGui::PopID();
            ImGui::Spacing();
        }

        if (!toggle_id.empty())
            MUST_OK(m_plugin_manager.SetEnabled(toggle_id, toggle_to), spdlog::error("{}", _r.error_v()));

        // Outside every PushID, so it matches BeginPopupModal below
        if (request_confirm)
            ImGui::OpenPopup(_("Uninstall?###plugin_uninstall_confirm"));

        if (ImGui::BeginPopupModal(
                _("Uninstall?###plugin_uninstall_confirm"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text(_("Uninstall %s?"), pending_label.c_str());
            ImGui::TextUnformatted(_("Their settings are deleted too."));
            ImGui::Spacing();

            if (ImGui::Button(_("Cancel")))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, (0x8C1F1FFF_rgba));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (0xB23333FF_rgba));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, (0x730F0FFF_rgba));
            if (ImGui::Button(_("Uninstall")))
            {
                StartPluginTask(i18n::tr_fmt("Uninstalling {}", pending_uninstall),
                                [name = pending_uninstall](PluginManager& pm) { return pm.Uninstall(name); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    m_show_window.Set(SubWindow::ManagePlugins, open);
}

void ScreenshotTool::DrawBrowsePluginsWindow()
{
    bool open = m_show_window.Has(SubWindow::BrowsePlugins);
    if (!open)
        return;

    // Downloaded the first time the window opens, then on Refresh
    bool fetch = false;
    {
        std::scoped_lock lock(m_index_state->mutex);
        fetch = !m_index_state->loaded && !m_index_state->loading;
    }
    if (fetch)
        FetchIndexAsync();

    ImGui::SetNextWindowSize(font_relative(49, 40), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Browse plugins###browse_plugins_window"), &open, ImGuiWindowFlags_NoSavedSettings))
    {
        const bool busy = m_install_state && m_install_state->running;

        std::unique_lock lock(m_index_state->mutex);
        const bool       loading = m_index_state->loading;

        if (loading)
            ImGui::BeginDisabled();
        fetch = ImGui::Button(_("Refresh"));
        if (loading)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", _("Downloading the plugin index..."));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##index_filter", _("Search"), &m_index_filter);

        ImGui::PushStyleColor(ImGuiCol_Text, (0x999999FF_rgba));
        ImGui::TextWrapped("%s",
                           _("Plugins listed in the oshot plugin index. Listed doesn't mean audited: plugins run with "
                             "the same permissions as oshot, so only install the ones you trust."));
        ImGui::PopStyleColor();
        if (!m_index_state->error.empty())
            ImGui::TextColored(Colors::RED, "%s", m_index_state->error.c_str());
        else if (m_index_state->stale)
            ImGui::TextColored(0xFFCC33FF_rgba, "%s", _("Offline: this is the last downloaded copy of the index."));
        ImGui::Separator();
        ImGui::Spacing();

        std::string filter = m_index_filter;
        std::ranges::transform(filter, filter.begin(), [](unsigned char c) { return std::tolower(c); });

        std::string install_id;
        bool        install_update = false;
        size_t      shown          = 0;
        // state.toml changes under a running install, so don't read it meanwhile
        if (busy)
            ImGui::TextDisabled("%s", _("Working, see the Plugins status window."));
        for (const index_plugin_t& p : m_index_state->index.plugins)
        {
            if (busy)
                break;
            if (!filter.empty())
            {
                std::string haystack = fmt::format("{} {} {} {}", p.id, p.name, p.description, fmt::join(p.tags, " "));
                std::ranges::transform(haystack, haystack.begin(), [](unsigned char c) { return std::tolower(c); });
                if (haystack.find(filter) == haystack.npos)
                    continue;
            }
            ++shown;

            std::string            why;
            const index_version_t* best      = pick_version(p, &why);
            const auto             installed = m_plugin_manager.GetStateManager().FindPlugin(p.id);
            const index_version_t* current =
                installed && !installed->version.empty() ? p.FindVersion(installed->version) : nullptr;

            ImGui::PushID(p.id.c_str());
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, (0xffffff07_rgba));
            ImGui::PushStyleColor(ImGuiCol_Border, (0xffffff14_rgba));
            ImGui::BeginChild("card",
                              ImVec2(0, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar);

            ImGui::TextUnformatted(p.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", p.id.c_str());
            if (best)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", best->version.c_str());
            }

            if (!p.description.empty())
                ImGui::TextWrapped("%s", p.description.c_str());
            if (!p.owners.empty())
                ImGui::TextDisabled(
                    _("By %s"), fmt::format("{}", fmt::join(p.authors.empty() ? p.owners : p.authors, ", ")).c_str());

            if (!p.license.empty())
                draw_tag(p.license.c_str(), 0x334c7fFF_rgba);
            draw_capability_tags(p.capabilities, /*same_line=*/!p.license.empty());
            for (const std::string& tag : p.tags)
            {
                ImGui::SameLine();
                draw_tag(tag.c_str(), 0x3f3f3fFF_rgba);
            }

            if (!p.homepage.empty())
            {
                ImGui::TextDisabled("%s", _("Homepage:"));
                ImGui::SameLine(0, 4);
                if (ImGui::TextLinkOpenURL(p.homepage.c_str(), p.homepage.c_str()))
                    minimize_window();
            }

            if (!best)
                ImGui::TextColored(Colors::RED, _("Can't be installed here: %s"), why.c_str());
            else if (best->artifacts.contains(OSHOT_TARGET))
                ImGui::TextDisabled(_("Prebuilt for %s"), OSHOT_TARGET);
            else
                ImGui::TextDisabled("%s", _("Built from source when installed: needs git, CMake and a C compiler"));

            if (current && current->yanked)
            {
                const std::string withdrawn =
                    current->yanked_reason.empty()
                        ? i18n::tr_fmt("Your version ({}) was withdrawn", current->version)
                        : i18n::tr_fmt("Your version ({}) was withdrawn: {}", current->version, current->yanked_reason);
                ImGui::TextColored(Colors::RED, "%s", withdrawn.c_str());
            }

            // Right-aligned action
            if (installed && installed->repository != INDEX_REPO_NAME)
            {
                ImGui::TextDisabled(_("Installed from %s"), installed->repository.c_str());
            }
            else if (installed && best && compare_versions(best->version, installed->version) > 0)
            {
                const std::string label = i18n::tr_fmt("Update {} -> {}", installed->version, best->version);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                     button_width(label.c_str()));
                if (ImGui::Button(label.c_str()))
                {
                    install_id     = p.id;
                    install_update = true;
                }
            }
            else if (installed)
            {
                const std::string label = i18n::tr_fmt("Installed {}", installed->version);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                     button_width(label.c_str()));
                ImGui::BeginDisabled();
                ImGui::Button(label.c_str());
                ImGui::EndDisabled();
            }
            else if (best)
            {
                const std::string label = i18n::tr_fmt("Install {}", best->version);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                     button_width(label.c_str()));
                if (ImGui::Button(label.c_str()))
                    install_id = p.id;
            }

            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
            ImGui::Spacing();
            ImGui::PopID();
        }

        if (shown == 0 && !busy && !loading && m_index_state->error.empty())
            ImGui::TextDisabled("%s",
                                m_index_filter.empty() ? _("The plugin index is empty.") : _("No plugin matches."));

        if (!install_id.empty())
        {
            const index_plugin_t* p = m_index_state->index.Find(install_id);
            const std::string     title =
                install_update ? i18n::tr_fmt("Updating {}", p->name) : i18n::tr_fmt("Installing {}", p->name);
            StartPluginTask(title, [install_id, install_update](PluginManager& pm) {
                return pm.InstallFromIndex(install_id, install_update);
            });
        }

        lock.unlock();
        if (fetch)
            FetchIndexAsync();
    }
    ImGui::End();

    m_show_window.Set(SubWindow::BrowsePlugins, open);
}

void ScreenshotTool::DrawInstallPluginsWindow()
{
    bool open = m_show_window.Has(SubWindow::InstallPlugins);
    if (!open)
        return;

    const bool is_installing = m_install_state && m_install_state->running;

    ImGui::SetNextWindowSize(font_relative(43, 17), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Install plugins###install_plugins_window"), &open, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextWrapped(
            "%s",
            _("A git URL, a folder with an oshot-plugin.toml, or a prebuilt plugin archive (.zip, .tar.gz, .tar.xz). "
              "From git or a folder, the plugin is built on this computer, which needs git and its build tools."));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::RED);
        ImGui::TextWrapped(
            "%s", _("Plugins run with the same permissions as oshot. Only install them from people you trust."));
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (is_installing)
            ImGui::BeginDisabled();

        static constexpr auto filters = std::to_array({ "*.zip", "*.tar.gz", "*.tgz", "*.tar.xz", "*.txz" });
        draw_input_text_file(_("Source"), "##install_source", filters, nullptr, m_install_source);

        const bool can_install = !is_installing && !m_install_source.empty();
        if (!can_install)
            ImGui::BeginDisabled();
        if (ImGui::Button(_("Install")))
            StartPluginTask(i18n::tr_fmt("Installing {}", m_install_source),
                            [source = m_install_source](PluginManager& pm) { return pm.Install(source); });
        if (!can_install)
            ImGui::EndDisabled();

        if (is_installing)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", _("Working, see the status window..."));
        }
    }
    ImGui::End();

    m_show_window.Set(SubWindow::InstallPlugins, open);
}

void ScreenshotTool::DrawPluginInstallStatus()
{
    bool open = m_show_window.Has(SubWindow::PluginInstallStatus);
    if (!open || !m_install_state)
        return;

    // Move the worker's events into the tree. Nothing else changes m_install_events
    {
        std::scoped_lock lock(m_install_state->events_mutex);
        while (!m_install_state->pending_events.empty())
        {
            plugin_install_event_t ev = std::move(m_install_state->pending_events.front());
            m_install_state->pending_events.pop_front();

            // Build output belongs to the step that is running
            if (ev.kind == plugin_install_event_t::Kind::Output)
            {
                if (m_install_events.empty())
                    m_install_events.push_back(
                        { .kind = ev.kind, .text = _("Output"), .details = {}, .in_progress = false, .output = {} });
                std::string& out = m_install_events.back().output;
                out += ev.text;
                if (out.size() > 64ULL * 1024)
                    out.erase(0, out.size() - 48ULL * 1024);
                continue;
            }

            // A step followed by the next one finished fine
            if (!m_install_events.empty() && m_install_events.back().in_progress &&
                ev.kind == plugin_install_event_t::Kind::Status)
            {
                m_install_events.back().kind        = plugin_install_event_t::Kind::Success;
                m_install_events.back().in_progress = false;
            }

            if (!m_install_events.empty() && m_install_events.back().in_progress &&
                ev.kind != plugin_install_event_t::Kind::Status)
            {
                install_node_t& node = m_install_events.back();
                node.kind            = ev.kind;
                node.in_progress     = false;
                node.details.push_back(ev.text);
            }
            else
            {
                m_install_events.push_back({ .kind        = ev.kind,
                                             .text        = ev.text,
                                             .details     = {},
                                             .in_progress = ev.kind == plugin_install_event_t::Kind::Status,
                                             .output      = {} });
            }
        }
    }

    ImGui::SetNextWindowSize(font_relative(43, 32), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(_("Plugins###plugin_install_status_window"), &open, ImGuiWindowFlags_NoSavedSettings))
    {
        for (size_t i = 0; i < m_install_events.size(); i++)
        {
            const install_node_t& node = m_install_events[i];
            ImGui::PushID(int(i));

            DrawEventIcon(node.kind);
            ImGui::SameLine();
            const bool has_children = !node.details.empty() || !node.output.empty();
            // Errors open by themselves, since they're what the user needs to read
            if (node.kind == plugin_install_event_t::Kind::Error && has_children)
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNodeEx("##node",
                                  has_children ? ImGuiTreeNodeFlags_None
                                               : ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen,
                                  "%s",
                                  node.text.c_str()) &&
                has_children)
            {
                for (const std::string& detail : node.details)
                    ImGui::TextWrapped("%s", detail.c_str());
                if (!node.output.empty())
                {
                    ImGui::BeginChild("##output",
                                      ImVec2(0, ImGui::GetTextLineHeight() * 12),
                                      ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    ImGui::TextUnformatted(node.output.c_str(), node.output.c_str() + node.output.size());
                    if (m_install_state->running)
                        ImGui::SetScrollHereY(1.0f);
                    ImGui::EndChild();
                }
                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (m_install_state->running)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", _("Working..."));
        }

        // A question from the worker thread, as a modal in this window
        bool has_confirm;
        {
            std::scoped_lock lock(m_install_state->confirm_mutex);
            has_confirm = m_install_state->confirm_pending && !m_install_state->confirm_answered;
        }
        if (has_confirm)
            ImGui::OpenPopup(_("Confirm###plugin_install_confirm"));

        ImGui::SetNextWindowSize(font_relative(42, 17));
        if (ImGui::BeginPopupModal(_("Confirm###plugin_install_confirm"), nullptr, ImGuiWindowFlags_NoCollapse))
        {
            std::scoped_lock lock(m_install_state->confirm_mutex);
            ImGui::TextWrapped("%s", m_install_state->confirm_prompt.c_str());
            if (ImGui::Button(_("Yes")))
            {
                m_install_state->confirm_answer   = true;
                m_install_state->confirm_answered = true;
                m_install_state->confirm_cv.notify_all();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(_("No")))
            {
                m_install_state->confirm_answer   = false;
                m_install_state->confirm_answered = true;
                m_install_state->confirm_cv.notify_all();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    // Keep the window open while an install runs. Nothing would drain the worker's
    // events, and a pending confirm would wait forever with no popup to answer it
    if (!open && m_install_state->running)
        open = true;

    m_show_window.Set(SubWindow::PluginInstallStatus, open);
}

void ScreenshotTool::DrawEventIcon(plugin_install_event_t::Kind kind)
{
    struct icon_t
    {
        const char* glyph;
        rgba_t      color;
    };
    constexpr auto table = std::to_array<icon_t>({
        { .glyph = "o", .color = 0x808080FF_rgba },  // Status (in progress)
        { .glyph = "v", .color = 0x2ecc71FF_rgba },  // Success
        { .glyph = "!", .color = 0xf1c40fFF_rgba },  // Warning
        { .glyph = "x", .color = 0xe74c3cFF_rgba },  // Error
        { .glyph = "i", .color = 0x3498dbFF_rgba },  // Info
        { .glyph = ">", .color = 0x808080FF_rgba },  // Output
    });
    const icon_t&  icon  = table[idx(kind)];
    ImGui::TextColored(icon.color, "%s", icon.glyph);
}
#endif
