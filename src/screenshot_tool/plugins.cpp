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

#include "cache.hpp"
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
#include "theme.hpp"
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
        ui::Tag(label, theme::color(AppCol::TagWarning));
        ImGui::SetItemTooltip(
            "%s\n\n%s", reason.c_str(), _("According to its author. oshot can't check or limit what plugins do."));
    }
}

void ScreenshotTool::FetchIndexAsync()
{
    {
        std::scoped_lock lock(m_index.state->mutex);
        if (m_index.state->loading)
            return;
        m_index.state->loading = true;
    }
    if (m_index.thread.joinable())
        m_index.thread.join();

    m_index.thread = std::thread([state = m_index.state]() {
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
    if (m_install.state && m_install.state->running)
        return;

    if (m_install.thread.joinable())
        m_install.thread.join();

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

    m_install.events.clear();
    m_install.state = state;
    m_show_window.Set(SubWindow::PluginInstallStatus);

    m_install.thread = std::thread([this, state, title, task = std::move(task)]() {
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

std::string* ScreenshotTool::GetTextBuffer(const std::string_view id)
{
    if (id == OSHOT_TEXT_OCR)
        return &m_inputs.ocr_results.data;
    if (id == OSHOT_TEXT_BARCODE)
        return &m_inputs.barcode_text;
    return nullptr;
}

void ScreenshotTool::StartPluginsCapture()
{
    m_plugin_selection = {};
    m_flags.Clear(CaptureFlag::ShowOcrOriginal);
    m_flags.Clear(CaptureFlag::ShowBarcodeOriginal);
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
    const bool has_selection = !m_main_sel.selection.is_empty();
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

            // Made on first use in each overlay. If it can't be, the letter is drawn
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

    constexpr float         margin  = 10.0f;
    const ImVec2            display = ImGui::GetIO().DisplaySize;
    const ui::OverlayWindow win("##plugin_reports",
                                ImVec2(display.x - margin, display.y - margin),
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav,
                                ImVec2(1.0f, 1.0f));
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
            ImGui::ProgressBar(fraction, ui::FontRelative(16, 0), r.text.empty() ? nullptr : r.text.c_str());
        }
        else
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextColored(theme::color(r.ok ? AppCol::Success : AppCol::Error), "%s", r.text.c_str());
            if (!r.copyable.empty())
                ImGui::TextDisabled("%s", r.copyable.c_str());
            ImGui::PopTextWrapPos();

            if (!r.copyable.empty())
            {
                const auto key        = std::make_pair(rt->id, r.action);
                bool       line_armed = armed && copied == key;
                if (ui::TimedButton(_("Copy"), _("Copied!"), line_armed))
                {
                    const Result<>& res = g_clipboard.CopyText(r.copyable);
                    copied              = key;
                    armed               = res.ok();
                    if (!res.ok())
                        spdlog::error("Failed to copy {}'s result: {}", rt->id, res.error_v());
                }
                else if (copied == key)
                    armed = line_armed;  // cleared by ui::TimedButton() once its time is up
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
}

bool ScreenshotTool::DrawPluginWarningModal(ui::Window& win)
{
    if (g_cache->GetValue(CacheEntry::KnowsPluginsRisk, false))
        return true;

    const char* id = _("Accept plugins risks###accept_plugin_risk_modal");
    if (!ImGui::IsPopupOpen(id))
        ImGui::OpenPopup(id);

    const float w = ui::FontRelative(32, 0).x;
    ImGui::SetNextWindowSizeConstraints({ w, 0.0f }, { w, FLT_MAX });
    if (ui::BeginModal(id))
    {
        ImGui::TextWrapped("%s", _("Plugins come with no guarantee of stability, availability or security."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s",
                           _("They run with the same permissions as oshot and are not sandboxed, so they can "
                             "access your files or anything else without asking you."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s",
                           _("oshot and its developers are NOT RESPONSIBLE FOR ANY DAMAGE CAUSED BY PLUGINS. "
                             "By continuing you accept the risks of using plugins from now on."));
        ImGui::Spacing();
        ImGui::TextUnformatted(_("Do you want to continue?"));

        enum : std::uint8_t
        {
            No,
            Yes
        };
        const int pressed = ui::ModalButtons(ui::ModalEscape(_("No")), ui::ModalDanger(_("Yes")));
        if (pressed == Yes)
            g_cache->SetValue(CacheEntry::KnowsPluginsRisk, true);
        else if (pressed == No)
            win.Close();
        ImGui::EndPopup();
    }
    return g_cache->GetValue(CacheEntry::KnowsPluginsRisk, false);
}

void ScreenshotTool::DrawManagePluginsWindow()
{
    static bool was_open = false;
    if (!m_show_window.Has(SubWindow::ManagePlugins))
    {
        was_open = false;
        return;
    }

    const bool busy = m_install.state && m_install.state->running;

    // oshotpm may have changed state.toml since oshot started
    if (!was_open && !busy)
        m_plugin_manager.GetStateManager().Reload();
    was_open = true;

    ui::Window win(
        m_show_window, m_flags, SubWindow::ManagePlugins, _("Manage plugins###manage_plugins_window"), { 48, 37 });
    if (win)
    {
        if (!DrawPluginWarningModal(win))
            return;

        if (busy)
        {
            ImGui::TextDisabled("%s", _("Working, see the Plugins status window. This list refreshes once it's done."));
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

        ui::TextWrappedColored(theme::color(AppCol::TextMuted),
                               "%s",
                               _("Changes apply the next time oshot starts. If oshot runs in the tray, quit it from "
                                 "the tray menu first."));
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

                ui::AlignRight(ui::ButtonWidth(_("Uninstall all")), false);
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
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(ImGuiCol_Text, 0.03f));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Text, 0.08f));
                    ImGui::BeginChild("card",
                                      ImVec2(0, 0),
                                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                      ImGuiWindowFlags_NoScrollbar);

                    ImGui::TextUnformatted(plugin.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", plugin.id.c_str());

                    const char* tag     = _("Disabled");
                    AppCol      tag_col = AppCol::TagWarning;
                    if (missing)
                    {
                        tag     = _("Missing");
                        tag_col = AppCol::TagError;
                    }
                    else if (plugin.enabled)
                    {
                        tag     = loaded ? _("Enabled") : _("Not loaded");
                        tag_col = loaded ? AppCol::TagSuccess : AppCol::TagNeutral;
                    }
                    ui::AlignRight(ui::TagWidth(tag));
                    ui::Tag(tag, theme::color(tag_col));

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
                            ui::Tag(license.c_str(), theme::color(AppCol::Tag));
                        }
                    }
                    draw_capability_tags(plugin.capabilities, /*same_line=*/!plugin.licenses.empty());

                    if (missing)
                        ImGui::TextColored(
                            theme::color(AppCol::Error), "%s", _("The library is missing: reinstall the plugin"));
                    else if (plugin.enabled && !loaded)
                        ImGui::TextDisabled("%s",
                                            _("Not loaded: restart oshot, or see View Logs if it failed to load"));

                    const char* toggle_label = plugin.enabled ? _("Disable") : _("Enable");
                    ui::AlignRight(ui::ButtonWidth(toggle_label) + ui::ButtonWidth(_("Uninstall")) +
                                   ImGui::GetStyle().ItemSpacing.x);
                    if (ImGui::Button(toggle_label))
                    {
                        toggle_id = plugin.id;
                        toggle_to = !plugin.enabled;
                    }
                    ImGui::SameLine();
                    if (ui::DangerButton(_("Uninstall")))
                    {
                        pending_uninstall = plugin.id;
                        pending_label     = fmt::format("'{}' ({})", plugin.name, plugin.id);
                        request_confirm   = true;
                    }

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

        if (ui::BeginModal(_("Uninstall?###plugin_uninstall_confirm")))
        {
            ImGui::Text(_("Uninstall %s?"), pending_label.c_str());
            ImGui::TextUnformatted(_("Their settings are deleted too."));
            enum : std::uint8_t
            {
                Cancel,
                Uninstall
            };
            if (ui::ModalButtons(ui::ModalEscape(_("Cancel")), ui::ModalDanger(_("Uninstall"))) == Uninstall)
                StartPluginTask(i18n::tr_fmt("Uninstalling {}", pending_uninstall),
                                [name = pending_uninstall](PluginManager& pm) { return pm.Uninstall(name); });
            ImGui::EndPopup();
        }
    }
}

void ScreenshotTool::DrawBrowsePluginsWindow()
{
    if (!m_show_window.Has(SubWindow::BrowsePlugins))
        return;

    // Downloaded the first time the window opens, then on Refresh
    bool fetch = false;
    {
        std::scoped_lock lock(m_index.state->mutex);
        fetch = !m_index.state->loaded && !m_index.state->loading;
    }
    if (fetch)
        FetchIndexAsync();

    ui::Window win(
        m_show_window, m_flags, SubWindow::BrowsePlugins, _("Browse plugins###browse_plugins_window"), { 49, 40 });
    if (win)
    {
        if (!DrawPluginWarningModal(win))
            return;

        const bool busy = m_install.state && m_install.state->running;

        std::unique_lock lock(m_index.state->mutex);
        const bool       loading = m_index.state->loading;

        ImGui::BeginDisabled(loading);
        fetch = ImGui::Button(_("Refresh"));
        ImGui::EndDisabled();
        if (loading)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", _("Downloading the plugin index..."));
        }
        ImGui::SameLine();
        ui::SearchBox(m_index.filter, "##index_filter", _("Search"));

        ui::TextWrappedColored(theme::color(AppCol::TextMuted),
                               "%s",
                               _("Plugins listed in the oshot plugin index. Listed doesn't mean audited: plugins run "
                                 "with the same permissions as oshot, so only install the ones you trust."));
        if (!m_index.state->error.empty())
            ImGui::TextColored(theme::color(AppCol::Error), "%s", m_index.state->error.c_str());
        else if (m_index.state->stale)
            ImGui::TextColored(
                theme::color(AppCol::Warning), "%s", _("Offline: this is the last downloaded copy of the index."));
        ImGui::Separator();
        ImGui::Spacing();

        std::string install_id;
        bool        install_update = false;
        size_t      shown          = 0;
        // state.toml changes under a running install, so don't read it meanwhile
        if (busy)
            ImGui::TextDisabled("%s", _("Working, see the Plugins status window."));
        for (const index_plugin_t& p : m_index.state->index.plugins)
        {
            if (busy)
                break;
            if (!ui::PassFilter(m_index.filter,
                                { p.id, p.name, p.description, fmt::format("{}", fmt::join(p.tags, " ")) }))
                continue;
            ++shown;

            std::string            why;
            const index_version_t* best      = pick_version(p, &why);
            const auto             installed = m_plugin_manager.GetStateManager().FindPlugin(p.id);
            const index_version_t* current =
                installed && !installed->version.empty() ? p.FindVersion(installed->version) : nullptr;

            ImGui::PushID(p.id.c_str());
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(ImGuiCol_Text, 0.03f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Text, 0.08f));
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
                ui::Tag(p.license.c_str(), theme::color(AppCol::Tag));
            draw_capability_tags(p.capabilities, /*same_line=*/!p.license.empty());
            for (const std::string& tag : p.tags)
            {
                ImGui::SameLine();
                ui::Tag(tag.c_str(), theme::color(AppCol::TagNeutral));
            }

            if (!p.homepage.empty())
            {
                ImGui::TextDisabled("%s", _("Homepage:"));
                ImGui::SameLine(0, 4);
                if (ImGui::TextLinkOpenURL(p.homepage.c_str(), p.homepage.c_str()))
                    minimize_window();
            }

            if (!best)
                ImGui::TextColored(theme::color(AppCol::Error), _("Can't be installed here: %s"), why.c_str());
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
                ImGui::TextColored(theme::color(AppCol::Error), "%s", withdrawn.c_str());
            }

            // Right-aligned action
            if (installed && installed->repository != INDEX_REPO_NAME)
            {
                ImGui::TextDisabled(_("Installed from %s"), installed->repository.c_str());
            }
            else if (installed && best && compare_versions(best->version, installed->version) > 0)
            {
                const std::string label = i18n::tr_fmt("Update {} -> {}", installed->version, best->version);
                ui::AlignRight(ui::ButtonWidth(label.c_str()), false);
                if (ImGui::Button(label.c_str()))
                {
                    install_id     = p.id;
                    install_update = true;
                }
            }
            else if (installed)
            {
                const std::string label = i18n::tr_fmt("Installed {}", installed->version);
                ui::AlignRight(ui::ButtonWidth(label.c_str()), false);
                ImGui::BeginDisabled();
                ImGui::Button(label.c_str());
                ImGui::EndDisabled();
            }
            else if (best)
            {
                const std::string label = i18n::tr_fmt("Install {}", best->version);
                ui::AlignRight(ui::ButtonWidth(label.c_str()), false);
                if (ImGui::Button(label.c_str()))
                    install_id = p.id;
            }

            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
            ImGui::Spacing();
            ImGui::PopID();
        }

        if (shown == 0 && !busy && !loading && m_index.state->error.empty())
            ImGui::TextDisabled("%s",
                                !m_index.filter.IsActive() ? _("The plugin index is empty.") : _("No plugin matches."));

        if (!install_id.empty())
        {
            const index_plugin_t* p = m_index.state->index.Find(install_id);
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
}

void ScreenshotTool::DrawInstallPluginsWindow()
{
    if (!m_show_window.Has(SubWindow::InstallPlugins))
        return;

    const bool is_installing = m_install.state && m_install.state->running;

    ui::Window win(
        m_show_window, m_flags, SubWindow::InstallPlugins, _("Install plugins###install_plugins_window"), { 43, 17 });
    if (win)
    {
        if (!DrawPluginWarningModal(win))
            return;

        ImGui::TextWrapped(
            "%s",
            _("A git URL, a folder with an oshot-plugin.toml, or a prebuilt plugin archive (.zip, .tar.gz, .tar.xz). "
              "From git or a folder, the plugin is built on this computer, which needs git and its build tools."));
        ImGui::Spacing();
        ui::TextWrappedColored(
            theme::color(AppCol::Error),
            "%s",
            _("Plugins run with the same permissions as oshot. Only install them from people you trust."));
        ImGui::Spacing();

        ImGui::BeginDisabled(is_installing);

        constexpr SDL_DialogFileFilter filter{ .name = N_("Archives"), .pattern = "zip;tar.gz;tgz;tar.xz;txz" };
        ui::InputFile(_("Source"), "##install_source", filter, nullptr, m_install.source);

        const bool can_install = !is_installing && !m_install.source.empty();
        ImGui::BeginDisabled(!can_install);
        if (ImGui::Button(_("Install")))
            StartPluginTask(i18n::tr_fmt("Installing {}", m_install.source),
                            [source = m_install.source](PluginManager& pm) { return pm.Install(source); });
        ImGui::EndDisabled();

        ImGui::EndDisabled();
        if (is_installing)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", _("Working, see the status window..."));
        }
    }
}

void ScreenshotTool::DrawPluginInstallStatus()
{
    if (!m_show_window.Has(SubWindow::PluginInstallStatus) || !m_install.state)
        return;

    // Move the worker's events into the tree. Nothing else changes m_install.events
    {
        std::scoped_lock lock(m_install.state->events_mutex);
        while (!m_install.state->pending_events.empty())
        {
            plugin_install_event_t ev = std::move(m_install.state->pending_events.front());
            m_install.state->pending_events.pop_front();

            // Build output belongs to the step that is running
            if (ev.kind == plugin_install_event_t::Kind::Output)
            {
                if (m_install.events.empty())
                    m_install.events.push_back(
                        { .kind = ev.kind, .text = _("Output"), .details = {}, .in_progress = false, .output = {} });
                std::string& out = m_install.events.back().output;
                out += ev.text;
                if (out.size() > 64ULL * 1024)
                    out.erase(0, out.size() - 48ULL * 1024);
                continue;
            }

            // A step followed by the next one finished fine
            if (!m_install.events.empty() && m_install.events.back().in_progress &&
                ev.kind == plugin_install_event_t::Kind::Status)
            {
                m_install.events.back().kind        = plugin_install_event_t::Kind::Success;
                m_install.events.back().in_progress = false;
            }

            if (!m_install.events.empty() && m_install.events.back().in_progress &&
                ev.kind != plugin_install_event_t::Kind::Status)
            {
                install_node_t& node = m_install.events.back();
                node.kind            = ev.kind;
                node.in_progress     = false;
                node.details.push_back(ev.text);
            }
            else
            {
                m_install.events.push_back({ .kind        = ev.kind,
                                             .text        = ev.text,
                                             .details     = {},
                                             .in_progress = ev.kind == plugin_install_event_t::Kind::Status,
                                             .output      = {} });
            }
        }
    }

    ui::Window win(m_show_window,
                   m_flags,
                   SubWindow::PluginInstallStatus,
                   _("Plugins###plugin_install_status_window"),
                   { 43, 32 });
    if (win)
    {
        if (!DrawPluginWarningModal(win))
            return;

        for (size_t i = 0; i < m_install.events.size(); i++)
        {
            const install_node_t& node = m_install.events[i];
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
                    if (m_install.state->running)
                        ImGui::SetScrollHereY(1.0f);
                    ImGui::EndChild();
                }
                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (m_install.state->running)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", _("Working..."));
        }

        // A question from the worker thread, as a modal in this window
        bool has_confirm;
        {
            std::scoped_lock lock(m_install.state->confirm_mutex);
            has_confirm = m_install.state->confirm_pending && !m_install.state->confirm_answered;
        }
        if (has_confirm)
            ImGui::OpenPopup(_("Confirm###plugin_install_confirm"));

        const float confirm_width = ui::FontRelative(42, 0).x;
        ImGui::SetNextWindowSizeConstraints({ confirm_width, 0.0f }, { confirm_width, FLT_MAX });
        if (ui::BeginModal(_("Confirm###plugin_install_confirm"), ImGuiWindowFlags_NoCollapse))
        {
            std::scoped_lock lock(m_install.state->confirm_mutex);
            ImGui::TextWrapped("%s", m_install.state->confirm_prompt.c_str());
            enum : std::uint8_t
            {
                Yes,
                No
            };
            if (const int pressed = ui::ModalButtons(_("Yes"), ui::ModalEscape(_("No"))); pressed >= 0)
            {
                m_install.state->confirm_answer   = pressed == Yes;
                m_install.state->confirm_answered = true;
                m_install.state->confirm_cv.notify_all();
            }
            ImGui::EndPopup();
        }
    }

    // Keep the window open while an install runs. Nothing would drain the worker's
    // events, and a pending confirm would wait forever with no popup to answer it
    if (m_install.state->running)
        win.KeepOpen();
}

void ScreenshotTool::DrawEventIcon(plugin_install_event_t::Kind kind)
{
    struct icon_t
    {
        const char* glyph;
        AppCol      color;
    };
    constexpr auto table = std::to_array<icon_t>({
        { .glyph = "o", .color = AppCol::TextMuted },  // Status (in progress)
        { .glyph = "v", .color = AppCol::Success },
        { .glyph = "!", .color = AppCol::Warning },
        { .glyph = "x", .color = AppCol::Error },
        { .glyph = "i", .color = AppCol::Info },
        { .glyph = ">", .color = AppCol::TextMuted },  // Output
    });
    const icon_t&  icon  = table[idx(kind)];
    ImGui::TextColored(theme::color(icon.color), "%s", icon.glyph);
}
#endif
