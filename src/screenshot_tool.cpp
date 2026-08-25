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

#include "screenshot_tool.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cache.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3_loader.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#ifndef DISABLE_PLUGINS
#  include "plugin.hpp"
#  include "plugins/oshot_plugin.h"
#else
#  define OCR_OUTPUT  "ocr_output"
#  define ZBAR_OUTPUT "barcode_output"
#endif
#include "screen_capture.hpp"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "tiny-process-library/process.hpp"
#include "tinyfiledialogs.h"
#include "tool_icons.h"
#include "util.hpp"

#ifndef GL_NO_ERROR
#  define GL_NO_ERROR 0
#endif

std::deque<monitor_t> wl_get_monitors();

using namespace std::chrono_literals;

constexpr rgba_t::rgba_t(ImVec4 vec)
    : r(static_cast<uint8_t>(vec.x * 255.0f)),
      g(static_cast<uint8_t>(vec.y * 255.0f)),
      b(static_cast<uint8_t>(vec.z * 255.0f)),
      a(static_cast<uint8_t>(vec.w * 255.0f))
{}

constexpr ImVec4 rgba_t::to_imvec4() const
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

inline rgba_t blend(rgba_t src, rgba_t dst)
{
    float a  = src.a / 255.0f;
    float ia = 1.0f - a;

    return rgba_t{ uint8_t(src.r * a + dst.r * ia),
                   uint8_t(src.g * a + dst.g * ia),
                   uint8_t(src.b * a + dst.b * ia),
                   uint8_t(src.a + dst.a * ia) };
}

static void get_filtered_filenames(const std::string&                          dir,
                                   std::vector<std::string>&                   list,
                                   std::function<bool(const fs::path&)>        filter,
                                   std::function<std::string(const fs::path&)> value_push)
{
    if (!fs::exists(dir))
        return;

    for (const auto& entry : fs::recursive_directory_iterator(
             dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied))
    {
        if (filter(entry.path()))
            list.emplace_back(value_push(entry.path()));
    }
}

// https://github.com/pthom/imgui/blob/808272622f52d2f36124629c29994d2a5a7eb2f2/imgui_demo.cpp#L273
// Helper to display a little (?) mark which shows a tooltip when hovered.
// In your own code you may want to display an actual icon if you are using a merged icon fonts (see docs/FONTS.md)
static void HelpMarker(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void draw_input_text_path(const char*                  label,
                                 const char*                  input_id,
                                 const bool                   is_file,
                                 const char*                  filters[],
                                 int                          filter_count,
                                 const std::function<void()>& if_edited,
                                 std::string&                 path,
                                 ImGuiInputFlags              flags)
{
    auto handle_drop = [&]() {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !g_dropped_paths.empty())
        {
            path = g_dropped_paths.back();
            g_dropped_paths.clear();
            if (if_edited)
                if_edited();
        }
    };

    const float button_size = ImGui::GetFrameHeight();
    ImGui::PushItemWidth(ImGui::CalcItemWidth() - button_size);
    if (ImGui::InputText(input_id, &path, flags) && if_edited)
        if_edited();
    ImGui::PopItemWidth();
    handle_drop();

    ImGui::PushID(input_id);
    ImGui::SameLine(0, 0);
    if (ImGui::Button("...", ImVec2(button_size, button_size)))
    {
        minimize_window();

        const fs::path default_path = path;

        std::string start_path;
        if (is_file && default_path.has_parent_path())
        {
            start_path = default_path.parent_path().string();
            start_path += '/';  // tinyfd treats a trailing separator as "open here"
        }

        const char* dialog_path =
            !is_file ? tinyfd_selectFolderDialog("Open folder", nullptr)
                     : tinyfd_openFileDialog("Open file", start_path.c_str(), filter_count, filters, nullptr, 0);

        maximize_window();

        if (dialog_path)
        {
            path.assign(dialog_path);
            if (if_edited)
                if_edited();
        }
    }
    handle_drop();
    ImGui::PopID();

    ImGui::SameLine(0, 3);
    ImGui::TextUnformatted(label);
}

static void draw_input_text_file(const char*                  label,
                                 const char*                  input_id,
                                 const char*                  filters[],
                                 int                          filter_count,
                                 const std::function<void()>& if_edited,
                                 std::string&                 path,
                                 ImGuiInputFlags              flags = 0)
{
    draw_input_text_path(label, input_id, true, filters, filter_count, if_edited, path, flags);
}

static void draw_input_text_folder(const char*                  label,
                                   const char*                  input_id,
                                   const std::function<void()>& if_edited,
                                   std::string&                 path,
                                   ImGuiInputFlags              flags = 0)
{
    draw_input_text_path(label, input_id, false, nullptr, 0, if_edited, path, flags);
}

static HandleHovered flip_handle_x(HandleHovered handle)
{
    switch (handle)
    {
        case HandleHovered::TopLeft:     return HandleHovered::TopRight;
        case HandleHovered::TopRight:    return HandleHovered::TopLeft;
        case HandleHovered::BottomLeft:  return HandleHovered::BottomRight;
        case HandleHovered::BottomRight: return HandleHovered::BottomLeft;
        case HandleHovered::Left:        return HandleHovered::Right;
        case HandleHovered::Right:       return HandleHovered::Left;
        default:                         return handle;
    }
}

static HandleHovered flip_handle_y(HandleHovered handle)
{
    switch (handle)
    {
        case HandleHovered::TopLeft:     return HandleHovered::BottomLeft;
        case HandleHovered::TopRight:    return HandleHovered::BottomRight;
        case HandleHovered::BottomLeft:  return HandleHovered::TopLeft;
        case HandleHovered::BottomRight: return HandleHovered::TopRight;
        case HandleHovered::Top:         return HandleHovered::Bottom;
        case HandleHovered::Bottom:      return HandleHovered::Top;
        default:                         return handle;
    }
}

static bool ui_blocks_selection()
{
    static ImGuiWindow* overlay_window = nullptr;

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return false;

    if (!overlay_window)
        overlay_window = ImGui::FindWindowByName("Screenshot Tool");

    ImGuiWindow* hovered = ctx->HoveredWindow;
    if (!hovered || !overlay_window)
        return false;

    // Allow selection when hovering the overlay window itself
    if (hovered->RootWindow == overlay_window)
        return false;

    // Anything else (Text tools, menu popups, etc.) blocks starting selection
    return true;
}

static bool create_timed_button(const std::string_view label1,
                                const std::string_view label2,
                                bool&                  armed,  // caller controls arming
                                const float            delay_secs = 1.5f)
{
    static float press_time = 0.0f;
    const double now        = ImGui::GetTime();
    if (armed && now - press_time > delay_secs)
        armed = false;

    if (ImGui::Button(!armed ? label1.data() : label2.data()))
    {
        press_time = now;
        return true;  // caller decides whether to set armed=true
    }
    return false;
}

static std::unordered_map<std::string, int>& color_name_map()
{
    static std::unordered_map<std::string, int> map;
    if (map.empty())
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
            map[ImGui::GetStyleColorName(i)] = i;

    return map;
}

void apply_imgui_theme()
{
    is_system_dark_mode() ? ScreenshotTool::StyleDefaultColor() : ImGui::StyleColorsLight();

    ImGuiStyle& style = ImGui::GetStyle();
    const auto& ov    = g_config->theme_overrides;
    const auto& cmap  = color_name_map();

    // Apply color overrides
    for (const auto& [name, hex] : ov.colors)
    {
        auto it = cmap.find(name);
        if (it == cmap.end())
            continue;
        ImVec4 col;
        if (hexstr_to_imvec4(hex.data(), col))
            style.Colors[it->second] = col;
    }

    // Apply style var overrides (-1 means "not set")
    if (ov.window_rounding >= 0.f)
        style.WindowRounding = ov.window_rounding;
    if (ov.frame_rounding >= 0.f)
        style.FrameRounding = ov.frame_rounding;
    if (ov.grab_rounding >= 0.f)
        style.GrabRounding = ov.grab_rounding;
    if (ov.tab_rounding >= 0.f)
        style.TabRounding = ov.tab_rounding;
    if (ov.window_border >= 0.f)
        style.WindowBorderSize = ov.window_border;
    if (ov.frame_border >= 0.f)
        style.FrameBorderSize = ov.frame_border;
}

Result<> ScreenshotTool::Start()
{
    Result<capture_result_t> result{ Err() };
    m_session = get_session_type();

    if (!g_config->Runtime.source_file.empty())
    {
        result = load_image_rgba(g_config->Runtime.source_file);
        TRY_MSG(result, "Failed to load image: {}");
    }
    else
    {
        if (g_config->File.delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(g_config->File.delay));

        switch (m_session)
        {
            case SessionType::X11:     result = capture_full_screen_x11(); break;
            case SessionType::Wayland: result = capture_full_screen_wayland(); break;
            case SessionType::KDE:     result = capture_full_screen_spectacle(); break;
            case SessionType::Windows: result = capture_full_screen_windows(); break;
            case SessionType::MacOS:   result = capture_full_screen_macos(); break;
            default:                   return Err("Unknown platform");
        }
        TRY_MSG(result, "Failed to capture screenshot: {}");

#if OSHOT_LINUX
        if (m_session == SessionType::Wayland)
        {
            m_wayland_monitors = wl_get_monitors();
            if (m_wayland_monitors.size() > 1)
                m_show_window.Set(SubWindow::OutputMenuSelection);
        }
#endif
    }

    m_screenshot = std::move(result.get());
    m_tool_thickness.fill(3.0f);
    m_tool_thickness[idx(ToolType::Text)] = 16.0f;

    spdlog::debug("captured screenshots: {}x{}, size: {}", m_screenshot.w, m_screenshot.h, m_screenshot.view().size());
    return Ok();
}

Result<> ScreenshotTool::StartWindow()
{
    // Do not load anything about the text tools window,
    // just the screenshot one
    if (g_config->Runtime.instant_copy_save != SavingOp::kNone)
    {
        m_state = ToolState::Selecting;
#if OSHOT_MACOS
        m_texture_id = ImTextureRef{};  // will be set by backend
#else
        const Result<ImTextureRef>& res = CreateTexture(nullptr, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
        TRY_MSG(res, "Failed to create openGL texture: {}");

        m_texture_id = res.get();
#endif
        return Ok();
    }

#ifndef DISABLE_PLUGINS
    static std::once_flag plugins_loaded;
    std::call_once(plugins_loaded, [&] { load_plugins(m_plugin_manager.GetStateManager().GetAllRepos()); });
#endif

    m_inputs = { g_config->File.ocr_path,
                 g_config->File.ocr_model,
                 g_config->File.ocr_get_repo,
#if defined(__unix__) && !defined(__APPLE__)
                 g_config->GetConfigDirPath() + "/models",
#else
                 "./models",
#endif
                 {},
                 "",
                 {},
                 "",
                 "" };
    m_current_color = (rgba_t(g_cache->GetValue(CacheEntry::AnnColor, 0xFF0000FF)));

    m_imgui_id_texts.insert_or_assign(OCR_OUTPUT, &m_inputs.ocr_results.data);
    m_imgui_id_texts.insert_or_assign(ZBAR_OUTPUT, &m_inputs.barcode_text);

    m_state = ToolState::Selecting;

    m_show_window.Set(SubWindow::MainTextTools, g_config->File.show_text_tools);

    fit_to_screen(m_screenshot);
    SyncRuntimeFromConfig();

#if OSHOT_MACOS
    m_texture_id = ImTextureRef{};  // will be set by backend
#else
    const Result<ImTextureRef>& res = CreateTexture(nullptr, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    TRY_MSG(res, "Failed to create openGL texture: {}");

    m_texture_id = res.get();

    // Since the creation of the screenshot texture was fine, suppose the other too
    m_tool_textures[idx(ToolType::Rectangle)] =
        CreateTexture(nullptr, ICON_SQUARE_RGBA, ICON_SQUARE_W, ICON_SQUARE_H).get();
    m_tool_textures[idx(ToolType::RectangleFilled)] =
        CreateTexture(nullptr, ICON_RECT_FILLED_RGBA, ICON_RECT_FILLED_W, ICON_RECT_FILLED_H).get();
    m_tool_textures[idx(ToolType::CircleFilled)] =
        CreateTexture(nullptr, ICON_CIRCLE_FILLED_RGBA, ICON_CIRCLE_FILLED_W, ICON_CIRCLE_FILLED_H).get();
    m_tool_textures[idx(ToolType::ToggleTextTools)] =
        CreateTexture(nullptr, ICON_TEXT_TOOLS_RGBA, ICON_TEXT_TOOLS_W, ICON_TEXT_TOOLS_H).get();
    m_tool_textures[idx(ToolType::Circle)] =
        CreateTexture(nullptr, ICON_CIRCLE_RGBA, ICON_CIRCLE_W, ICON_CIRCLE_H).get();
    m_tool_textures[idx(ToolType::CounterBubble)] =
        CreateTexture(nullptr, ICON_COUNTER_BUBBLE_RGBA, ICON_COUNTER_BUBBLE_W, ICON_COUNTER_BUBBLE_H).get();
    m_tool_textures[idx(ToolType::Pencil)] =
        CreateTexture(nullptr, ICON_PENCIL_RGBA, ICON_PENCIL_W, ICON_PENCIL_H).get();

    m_tool_textures[idx(ToolType::Arrow)] = CreateTexture(nullptr, ICON_ARROW_RGBA, ICON_ARROW_W, ICON_ARROW_H).get();
    m_tool_textures[idx(ToolType::Text)]  = CreateTexture(nullptr, ICON_TEXT_RGBA, ICON_TEXT_W, ICON_TEXT_H).get();
    m_tool_textures[idx(ToolType::CopyImage)] = CreateTexture(nullptr, ICON_COPY_RGBA, ICON_COPY_W, ICON_COPY_H).get();
    m_tool_textures[idx(ToolType::SaveImage)] = CreateTexture(nullptr, ICON_SAVE_RGBA, ICON_SAVE_W, ICON_SAVE_H).get();
    m_tool_textures[idx(ToolType::Line)]      = CreateTexture(nullptr, ICON_LINE_RGBA, ICON_LINE_W, ICON_LINE_H).get();
    m_tool_textures[idx(ToolType::Logo)] = CreateTexture(nullptr, OSHOT_LOGO_RGBA, OSHOT_LOGO_W, OSHOT_LOGO_H).get();
#endif

#ifndef DISABLE_PLUGINS
    // Placeholders for not crashing when used but not needed
    PluginCallbacks cb;
    cb.on_status  = [](const std::string_view) {};
    cb.on_success = [](const std::string_view) {};
    cb.on_warning = [](const std::string_view) {};
    cb.on_error   = [](const std::string_view) {};
    cb.on_info    = [](const std::string_view) {};
    cb.confirm    = [](const std::string_view, bool) -> bool { return false; };
    m_plugin_manager.SetCallbacks(cb);
#endif

    if (!fs::exists(m_inputs.ocr_model_downloaded_path))
        SetError(m_download_errors, OcrDownloadError::InvalidPath, "No such directory or path");
    else if (!fs::is_directory(m_inputs.ocr_model_downloaded_path))
        SetError(m_download_errors, OcrDownloadError::InvalidPath, "Not a directory");

    return Ok();
}

void ScreenshotTool::RenderOverlay()
{
    const bool disable_esc = (m_current_actions.HasAny(CurrentAction::IsTextPlacing, CurrentAction::IsColorPicking) ||
                              m_show_window.HasAny(SubWindow::OcrDownload, SubWindow::Preferences)) &&
                             !g_config->File.show_text_tools;

    static constexpr int minimal_win_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoBackground;
    // Overlay window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Screenshot Tool",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoInputs);

    // Screenshot as a centered bg image
    UpdateWindowBg();

    if (ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest)
        ImGui::GetBackgroundDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);

    ImGui::GetBackgroundDrawList()->AddImage(m_texture_id, m_image_origin, m_image_end);

    if (ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear)
        ImGui::GetBackgroundDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

    if (m_session == SessionType::Wayland && m_show_window.Has(SubWindow::OutputMenuSelection))
    {
        DrawOutputMenuSelection();
        DrawDarkOverlay();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !disable_esc)
            Cancel();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    if (m_selection.get_width() == 0 || m_selection.get_height() == 0)
    {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::Begin("##select_area", nullptr, minimal_win_flags);
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Select an area");
        ImGui::TextColored(ImVec4(0, 1, 1, 1), "Press Save/Copy for the full screenshot");
        ImGui::TextColored(ImVec4(0, 1, 0.5f, 1), "CTRL+A for whole image selection");
        ImGui::End();
    }

    if (m_state == ToolState::Selecting || m_state == ToolState::Selected || m_state == ToolState::Resizing)
    {
        DrawAnnotations();
        DrawDarkOverlay();
        DrawSelectionBorder();
        HandleSelectionInput();
    }

    if (m_state == ToolState::Selected)
    {
        DrawAnnotationToolbar();

        if (m_current_actions.Has(CurrentAction::IsColorPicking))
            HandleColorPickerInput();
        else
            HandleAnnotationInput();

        if (g_config->Runtime.instant_copy_save != SavingOp::kNone)
            m_on_complete(g_config->Runtime.instant_copy_save, GetFinalImage(), g_config->File.image_out_type.second);
    }

    ImGui::End();
    ImGui::PopStyleVar();

    bool open = m_show_window.Has(SubWindow::MainTextTools);
    if (m_state == ToolState::Selected && open)
    {
        ImGui::Begin("Text tools", &open, ImGuiWindowFlags_MenuBar);
        DrawMenuItems();
        DrawPreferencesWindow();
        DrawDownloadOCRWindow();
        DrawLogsWindow();
        DrawOcrTools();
        DrawAboutWindow();
        DrawBarDecodeTools();
#ifndef DISABLE_PLUGINS
        DrawManagePluginsWindow();
        DrawInstallPluginsWindow();
        DrawPluginInstallStatus();
        DrawUninstallPluginsWindow();
        for (auto& [id, rt] : g_plugins)
        {
            if (!rt.enabled)
                continue;

            oshot_plugin_t* plugin = rt.plugin;
            if (!plugin->render || !plugin->id || !plugin->name || plugin->name[0] == '\0')
                continue;

            if (ImGui::CollapsingHeader(plugin->name))
            {
                ImGui::PushID(plugin->id);

                // auto-height
                ImGui::BeginChild(plugin->name, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);

                ScopedActivePlugin _(&rt);
                plugin->render(rt.state);

                ImGui::EndChild();
                ImGui::PopID();
            }
        }
#endif
        ImGui::End();
    }
    m_show_window.Set(SubWindow::MainTextTools, open);

    HandleShortcutsInput();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !disable_esc)
        Cancel();
}

void ScreenshotTool::NormalizeSelection()
{
    if (m_selection.start.x > m_selection.end.x)
        std::swap(m_selection.start.x, m_selection.end.x);
    if (m_selection.start.y > m_selection.end.y)
        std::swap(m_selection.start.y, m_selection.end.y);
}

void ScreenshotTool::HandleShortcutsInput()
{
    if (ImGui::Shortcut(ImGuiKey_E | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
    {
        g_config->File.allow_out_edit = !g_config->File.allow_out_edit;
        ImGui::ClearActiveID();
    }

    if (ImGui::Shortcut(ImGuiKey_A | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
    {
        m_selection.start = point_t{ m_image_origin.x, m_image_origin.y };
        m_selection.end   = point_t{ m_image_origin.x + static_cast<float>(m_screenshot.w),
                                     m_image_origin.y + static_cast<float>(m_screenshot.h) };
        m_state           = ToolState::Selected;
    }

    if (ImGui::Shortcut(ImGuiKey_Z | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal) && !m_annotations.empty())
        m_annotations.pop_back();

    if (ImGui::Shortcut(ImGuiKey_G | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
        g_config->Runtime.enable_handles = !g_config->Runtime.enable_handles;

    if (ImGui::Shortcut(ImGuiKey_S | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
        if (m_on_complete)
            m_on_complete(SavingOp::File, GetFinalImage(), g_config->File.image_out_type.second);

    if (ImGui::Shortcut(ImGuiKey_C | ImGuiMod_Ctrl | (g_config->File.ctrl_c_copy_img ? 0 : ImGuiMod_Shift),
                        ImGuiInputFlags_RouteGlobal) &&
        !ui_blocks_selection())
        if (m_on_complete)
            m_on_complete(SavingOp::Clipboard, GetFinalImage(), g_config->File.image_out_type.second);
}

void ScreenshotTool::HandleSelectionInput()
{
    // Only block new interactions. Never block an ongoing drag/resize.
    if ((m_input_owner != InputOwner::Selection && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
         ui_blocks_selection()) ||
        m_current_tool != ToolType::kNone)
    {
        m_input_owner = InputOwner::Tools;
        return;
    }

    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const float   sel_x     = m_selection.get_x();
    const float   sel_y     = m_selection.get_y();
    const float   sel_w     = m_selection.get_width();
    const float   sel_h     = m_selection.get_height();
    const ImRect  selection_rect(ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h));

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_input_owner != InputOwner::Selection)
    {
        m_input_owner = InputOwner::Selection;

        // Check if we're starting to resize from a handle
        if (m_handle_hover != HandleHovered::kNone)
        {
            // Normalize before storing the drag anchor so HandleResizeInput
            // always starts from a canonical (start <= end) selection.
            NormalizeSelection();
            m_dragging_handle      = m_handle_hover;
            m_drag_start_mouse     = mouse_pos;
            m_drag_start_selection = m_selection;
            m_state                = ToolState::Resizing;
        }
        // Check if we're clicking inside the selection to move it
        else if (selection_rect.Contains(mouse_pos))
        {
            NormalizeSelection();
            m_dragging_handle      = HandleHovered::Move;
            m_drag_start_mouse     = mouse_pos;
            m_drag_start_selection = m_selection;
            m_state                = ToolState::Resizing;
        }
        // Start new selection
        else
        {
            m_selection.start = { mouse_pos.x, mouse_pos.y };
            m_selection.end   = m_selection.start;
            m_state           = ToolState::Selecting;
        }
    }

    if (m_input_owner == InputOwner::Selection && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (m_state == ToolState::Resizing)
            HandleResizeInput();
        else  // ToolState::Selecting
            m_selection.end = { mouse_pos.x, mouse_pos.y };
    }

    if (m_input_owner == InputOwner::Selection && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_dragging_handle = HandleHovered::kNone;
        m_input_owner     = InputOwner::kNone;

        if (m_selection.get_width() > 10 && m_selection.get_height() > 10)
            m_state = ToolState::Selected;

        NormalizeSelection();
    }
}

void ScreenshotTool::HandleResizeInput()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    ImVec2        delta(mouse_pos.x - m_drag_start_mouse.x, mouse_pos.y - m_drag_start_mouse.y);

    switch (m_dragging_handle)
    {
        case HandleHovered::TopLeft:
            m_selection.start.x = m_drag_start_selection.start.x + delta.x;
            m_selection.start.y = m_drag_start_selection.start.y + delta.y;
            break;
        case HandleHovered::TopRight:
            m_selection.end.x   = m_drag_start_selection.end.x + delta.x;
            m_selection.start.y = m_drag_start_selection.start.y + delta.y;
            break;
        case HandleHovered::BottomLeft:
            m_selection.start.x = m_drag_start_selection.start.x + delta.x;
            m_selection.end.y   = m_drag_start_selection.end.y + delta.y;
            break;
        case HandleHovered::BottomRight:
            m_selection.end.x = m_drag_start_selection.end.x + delta.x;
            m_selection.end.y = m_drag_start_selection.end.y + delta.y;
            break;
        case HandleHovered::Top:    m_selection.start.y = m_drag_start_selection.start.y + delta.y; break;
        case HandleHovered::Bottom: m_selection.end.y = m_drag_start_selection.end.y + delta.y; break;
        case HandleHovered::Left:   m_selection.start.x = m_drag_start_selection.start.x + delta.x; break;
        case HandleHovered::Right:  m_selection.end.x = m_drag_start_selection.end.x + delta.x; break;
        case HandleHovered::Move:
            m_selection.start.x = m_drag_start_selection.start.x + delta.x;
            m_selection.start.y = m_drag_start_selection.start.y + delta.y;
            m_selection.end.x   = m_drag_start_selection.end.x + delta.x;
            m_selection.end.y   = m_drag_start_selection.end.y + delta.y;
            break;
        default: break;
    }

    // When a handle is dragged past the opposite edge, the selection inverts.
    // Normalize it by swapping coordinates, flipping the active handle, and
    // resetting the drag anchor so the delta stays correct next frame.
    if (m_selection.start.x > m_selection.end.x)
    {
        std::swap(m_selection.start.x, m_selection.end.x);
        m_dragging_handle      = flip_handle_x(m_dragging_handle);
        m_drag_start_mouse     = mouse_pos;
        m_drag_start_selection = m_selection;
    }

    if (m_selection.start.y > m_selection.end.y)
    {
        std::swap(m_selection.start.y, m_selection.end.y);
        m_dragging_handle      = flip_handle_y(m_dragging_handle);
        m_drag_start_mouse     = mouse_pos;
        m_drag_start_selection = m_selection;
    }
}

void ScreenshotTool::HandleAnnotationInput()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();

    // If the user switched away from the Text tool while a text annotation was
    // in progress (but not yet committed), cancel it so the placement position
    // is not left stale.
    // Without this, the generic drawing code can later
    // overwrite m_current_annotation (including start.x/y = 0) while
    // m_is_text_placing stays true, causing the input window to reappear at
    // position (0, 0) the next time Text is selected.
    if (m_current_actions.Has(CurrentAction::IsTextPlacing) && m_current_tool != ToolType::Text)
    {
        m_current_annotation = {};
        m_current_actions.Clear(CurrentAction::IsTextPlacing);
    }

    if (m_current_tool == ToolType::Text)
    {
        if (!m_current_actions.Has(CurrentAction::IsTextPlacing) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ui_blocks_selection())
        {
            m_current_actions.Set(CurrentAction::IsTextPlacing);
            m_current_annotation.type      = ToolType::Text;
            m_current_annotation.start     = { mouse_pos.x, mouse_pos.y };
            m_current_annotation.end       = m_current_annotation.start;
            m_current_annotation.color     = m_current_color;
            m_current_annotation.thickness = m_tool_thickness[idx(ToolType::Text)];
            m_current_annotation.text.clear();
        }

        if (m_current_actions.Has(CurrentAction::IsTextPlacing))
        {
            const float padding_y =
                std::max(2.0f, (m_current_annotation.thickness - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::SetNextWindowPos(ImVec2(m_current_annotation.start.x, m_current_annotation.start.y));
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, padding_y));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));

            ImGui::Begin("##text_ann_input_win",
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar);

            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            ImFont* ann_font = CacheAndGetFont(m_inputs.resolved_ann_font_path, m_current_annotation.thickness);
            ImGui::PushFont(ann_font, ann_font->LegacySize);

            ImGui::PushStyleColor(ImGuiCol_Text, m_current_annotation.color.to_abgr());
            if (ImGui::InputText(
                    "##text_ann_input_text", &m_current_annotation.text, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (!m_current_annotation.text.empty())
                {
                    // Shift start to where the text actually appears visually,
                    // accounting for the FramePadding we pushed above
                    const ImVec2& fp = ImGui::GetStyle().FramePadding;
                    m_current_annotation.start.x += fp.x;
                    m_current_annotation.start.y += fp.y;

                    m_annotations.push_back(m_current_annotation);
                }
                m_current_annotation = {};
                m_current_actions.Clear(CurrentAction::IsTextPlacing);
            }
            ImGui::PopFont();

            ImGui::Text("Enter: Place | ESC: Cancel");

            ImGui::PopStyleColor();

            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_current_annotation = {};
                m_current_actions.Clear(CurrentAction::IsTextPlacing);
            }

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
        }
        return;  // never fall through to the generic drag path
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ui_blocks_selection())
    {
        m_current_actions.Set(CurrentAction::IsDrawing);
        m_current_annotation.type      = m_current_tool;
        m_current_annotation.start     = { mouse_pos.x, mouse_pos.y };
        m_current_annotation.end       = m_current_annotation.start;
        m_current_annotation.color     = m_current_color;
        m_current_annotation.thickness = m_tool_thickness[idx(m_current_tool)];
        m_current_annotation.points.clear();

        if (m_current_tool == ToolType::Pencil)
        {
            m_current_annotation.points.push_back(m_current_annotation.start);
        }
        else if (m_current_tool == ToolType::CounterBubble)
        {
            uint8_t next = 1;
            for (const auto& ann : m_annotations)
                if (ann.type == ToolType::CounterBubble && ann.count >= next)
                    next = ann.count + 1;
            m_current_annotation.count = next;
        }
    }

    if (m_current_actions.Has(CurrentAction::IsDrawing) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        m_current_annotation.end = { mouse_pos.x, mouse_pos.y };

        if (m_current_tool == ToolType::Pencil)
        {
            // Add point if it's far enough from the last one
            if (!m_current_annotation.points.empty())
            {
                const point_t& last = m_current_annotation.points.back();
                float          dx   = mouse_pos.x - last.x;
                float          dy   = mouse_pos.y - last.y;
                if (dx * dx + dy * dy > 4.0f)  // Minimum distance squared
                    m_current_annotation.points.push_back({ mouse_pos.x, mouse_pos.y });
            }
        }
    }

    if (m_current_actions.Has(CurrentAction::IsDrawing) && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_current_actions.Clear(CurrentAction::IsDrawing);

        // Only add annotation if it has meaningful size or points
        bool should_add = false;
        if (m_current_tool == ToolType::Pencil)
        {
            should_add = (m_current_annotation.points.size() > 1);
        }
        else
        {
            float dx   = m_current_annotation.end.x - m_current_annotation.start.x;
            float dy   = m_current_annotation.end.y - m_current_annotation.start.y;
            should_add = (dx * dx + dy * dy) > 25.0f;  // Minimum 5px distance
        }

        if (should_add)
            m_annotations.push_back(m_current_annotation);

        m_current_annotation = annotation_t{};
    }
}

void ScreenshotTool::HandleColorPickerInput()
{
    // The loupe acts as our cursor
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const int     px        = int(mouse_pos.x - m_image_origin.x);
    const int     py        = int(mouse_pos.y - m_image_origin.y);

    const bool in_image = px >= 0 && px < m_screenshot.w && py >= 0 && py < m_screenshot.h;

    constexpr float k_loupe_px = 140.0f;  // loupe display size (square, pixels)
    constexpr float k_zoom     = 8.0f;    // magnification factor
    constexpr float k_padding  = 10.0f;   // inner window padding
    constexpr float k_offset   = 15.0f;   // distance from cursor to loupe corner
    constexpr float k_win_size = k_loupe_px + k_padding * 2.0f;

    constexpr uint32_t shadow_color      = rgba_t(0x000000B4).to_abgr();
    constexpr uint32_t white_lines_color = rgba_t(0xffffffE6).to_abgr();

    // Position loupe window: prefer bottom-right, flip to stay on screen
    const ImVec2& display = ImGui::GetIO().DisplaySize;
    float         win_x   = mouse_pos.x + k_offset;
    float         win_y   = mouse_pos.y + k_offset;

    // For the horizontal flip we know the exact width; for vertical, use loupe
    // height + a comfortable margin since AlwaysAutoResize determines final height.
    constexpr float k_approx_info_h = 50.0f;  // swatch row + spacing
    if (win_x + k_win_size > display.x)
        win_x = mouse_pos.x - k_offset - k_win_size;
    if (win_y + k_win_size + k_approx_info_h > display.y)
        win_y = mouse_pos.y - k_offset - k_win_size - k_approx_info_h;

    ImGui::SetNextWindowPos(ImVec2(win_x, win_y));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(k_padding, k_padding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::Begin("##color_loupe",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    if (!in_image)
    {
        ImGui::TextDisabled("Outside of the image");
    }
    else
    {
        // Sample the pixel under the cursor
        const size_t off = (size_t(py) * m_screenshot.w + px) * 4;
        const rgba_t c   = load_rgba(m_screenshot.data.data() + off);

        // Compute UV window for the zoomed region
        const float half_src_px_x = (k_loupe_px / k_zoom) * 0.5f / float(m_screenshot.w);
        const float half_src_px_y = (k_loupe_px / k_zoom) * 0.5f / float(m_screenshot.h);
        const float uv_cx         = (float(px) + 0.5f) / float(m_screenshot.w);
        const float uv_cy         = (float(py) + 0.5f) / float(m_screenshot.h);

        const ImVec2 uv_min(uv_cx - half_src_px_x, uv_cy - half_src_px_y);
        const ImVec2 uv_max(uv_cx + half_src_px_x, uv_cy + half_src_px_y);

        ImGuiPlatformIO& platform_io  = ImGui::GetPlatformIO();
        ImDrawList*      dl           = ImGui::GetWindowDrawList();
        const ImVec2&    loupe_origin = ImGui::GetCursorScreenPos();

        if (platform_io.DrawCallback_SetSamplerNearest)
            dl->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);

        // Draw the magnified image
        ImGui::Image(m_texture_id, ImVec2(k_loupe_px, k_loupe_px), uv_min, uv_max);

        if (platform_io.DrawCallback_SetSamplerLinear)
            dl->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);

        // Draw crosshair over the loupe
        const ImVec2    ctr = ImVec2(loupe_origin.x + k_loupe_px * 0.5f, loupe_origin.y + k_loupe_px * 0.5f);
        constexpr float arm = 10.0f;
        constexpr float gap = 3.0f;  // gap around the centre dot

        // Shadow lines for contrast on any background
        dl->AddLine(ImVec2(ctr.x - arm, ctr.y), ImVec2(ctr.x - gap, ctr.y), shadow_color, 1.5f);
        dl->AddLine(ImVec2(ctr.x + gap, ctr.y), ImVec2(ctr.x + arm, ctr.y), shadow_color, 1.5f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - arm), ImVec2(ctr.x, ctr.y - gap), shadow_color, 1.5f);
        dl->AddLine(ImVec2(ctr.x, ctr.y + gap), ImVec2(ctr.x, ctr.y + arm), shadow_color, 1.5f);
        // White lines on top
        dl->AddLine(ImVec2(ctr.x - arm, ctr.y), ImVec2(ctr.x - gap, ctr.y), white_lines_color, 1.0f);
        dl->AddLine(ImVec2(ctr.x + gap, ctr.y), ImVec2(ctr.x + arm, ctr.y), white_lines_color, 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - arm), ImVec2(ctr.x, ctr.y - gap), white_lines_color, 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y + gap), ImVec2(ctr.x, ctr.y + arm), white_lines_color, 1.0f);
        // Centre dot, filled with the hovered colour so it's always visible
        dl->AddCircleFilled(ctr, gap - 0.5f, c.to_abgr());
        dl->AddCircle(ctr, gap - 0.5f, rgba_t(0xffffffC8).to_abgr(), 12, 1.0f);

        // Outline around the entire loupe image
        dl->AddRect(loupe_origin,
                    ImVec2(loupe_origin.x + k_loupe_px, loupe_origin.y + k_loupe_px),
                    rgba_t(0x505050DC).to_abgr(),
                    2.0f,
                    1.5f,
                    ImDrawFlags_None);

        ImGui::Spacing();
        ImGui::ColorButton("##loupe_swatch", c.to_imvec4(), ImGuiColorEditFlags_NoPicker, ImVec2(32, 32));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("#%02X%02X%02X", c.r, c.g, c.b);
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "%-3d ", c.r);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "%-3d ", c.g);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 0, 1, 1), "%-3d", c.b);
        ImGui::EndGroup();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_current_color = c;
            m_current_actions.Clear(CurrentAction::IsColorPicking);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();

    // Keeps a precise reference point visible even outside the loupe region.
    {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddCircle(mouse_pos, 5.0f, shadow_color, 12, 2.0f);
        fg->AddCircleFilled(mouse_pos, 2.0f, rgba_t(0xffffffFF).to_abgr());
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_current_actions.Clear(CurrentAction::IsColorPicking);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
}

void ScreenshotTool::UpdateHandleHoverState()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    m_handle_hover          = HandleHovered::kNone;

    if (m_state != ToolState::Selected && m_state != ToolState::Resizing)
        return;

    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_w = m_selection.get_width();
    const float sel_h = m_selection.get_height();

    const float hover_half = HANDLE_HOVER_SIZE / 2.0f;

    const std::array<handle_info_t, 8> handles = {
        { { .type = HandleHovered::TopLeft,
            .pos  = ImVec2(sel_x, sel_y),
            .rect = ImRect(ImVec2(sel_x - hover_half, sel_y - hover_half),
                           ImVec2(sel_x + hover_half, sel_y + hover_half)) },

          { .type = HandleHovered::TopRight,
            .pos  = ImVec2(sel_x + sel_w, sel_y),
            .rect = ImRect(ImVec2(sel_x + sel_w - hover_half, sel_y - hover_half),
                           ImVec2(sel_x + sel_w + hover_half, sel_y + hover_half)) },

          { .type = HandleHovered::BottomLeft,
            .pos  = ImVec2(sel_x, sel_y + sel_h),
            .rect = ImRect(ImVec2(sel_x - hover_half, sel_y + sel_h - hover_half),
                           ImVec2(sel_x + hover_half, sel_y + sel_h + hover_half)) },

          { .type = HandleHovered::BottomRight,
            .pos  = ImVec2(sel_x + sel_w, sel_y + sel_h),
            .rect = ImRect(ImVec2(sel_x + sel_w - hover_half, sel_y + sel_h - hover_half),
                           ImVec2(sel_x + sel_w + hover_half, sel_y + sel_h + hover_half)) },

          { .type = HandleHovered::Top,
            .pos  = ImVec2(sel_x + sel_w / 2, sel_y),
            .rect = ImRect(ImVec2(sel_x + sel_w / 2 - hover_half, sel_y - hover_half),
                           ImVec2(sel_x + sel_w / 2 + hover_half, sel_y + hover_half)) },

          { .type = HandleHovered::Bottom,
            .pos  = ImVec2(sel_x + sel_w / 2, sel_y + sel_h),
            .rect = ImRect(ImVec2(sel_x + sel_w / 2 - hover_half, sel_y + sel_h - hover_half),
                           ImVec2(sel_x + sel_w / 2 + hover_half, sel_y + sel_h + hover_half)) },

          { .type = HandleHovered::Left,
            .pos  = ImVec2(sel_x, sel_y + sel_h / 2),
            .rect = ImRect(ImVec2(sel_x - hover_half, sel_y + sel_h / 2 - hover_half),
                           ImVec2(sel_x + hover_half, sel_y + sel_h / 2 + hover_half)) },

          { .type = HandleHovered::Right,
            .pos  = ImVec2(sel_x + sel_w, sel_y + sel_h / 2),
            .rect = ImRect(ImVec2(sel_x + sel_w - hover_half, sel_y + sel_h / 2 - hover_half),
                           ImVec2(sel_x + sel_w + hover_half, sel_y + sel_h / 2 + hover_half)) } }
    };

    for (const handle_info_t& handle : handles)
    {
        if (handle.rect.Contains(mouse_pos))
        {
            m_handle_hover = handle.type;
            break;
        }
    }
}

void ScreenshotTool::UpdateCursor()
{
    if (m_current_tool != ToolType::kNone)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
    else if (m_handle_hover != HandleHovered::kNone || m_dragging_handle != HandleHovered::kNone)
    {
        HandleHovered handle = (m_dragging_handle != HandleHovered::kNone) ? m_dragging_handle : m_handle_hover;

        switch (handle)
        {
            case HandleHovered::Move: ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); break;

            case HandleHovered::TopLeft:
            case HandleHovered::BottomRight: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;

            case HandleHovered::TopRight:
            case HandleHovered::BottomLeft: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;

            case HandleHovered::Top:
            case HandleHovered::Bottom: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;

            case HandleHovered::Left:
            case HandleHovered::Right: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;

            default: ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); break;
        }
    }
    else if (m_state == ToolState::Selected || m_state == ToolState::Resizing)
    {
        // Check if mouse is inside the selection (for moving)
        const float sel_x = m_selection.get_x();
        const float sel_y = m_selection.get_y();
        const float sel_w = m_selection.get_width();
        const float sel_h = m_selection.get_height();

        ImRect       selection_rect(ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h));
        const ImVec2 mouse_pos = ImGui::GetMousePos();

        if (selection_rect.Contains(mouse_pos))
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
    else
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
}

void ScreenshotTool::DrawDarkOverlay()
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_w = m_selection.get_width();
    const float sel_h = m_selection.get_height();

    constexpr ImU32 dark_color = rgba_t(0x00000078).to_abgr();

    // Top rectangle
    draw_list->AddRectFilled(m_image_origin, ImVec2(m_image_end.x, sel_y), dark_color);

    // Bottom rectangle
    draw_list->AddRectFilled(ImVec2(m_image_origin.x, sel_y + sel_h), m_image_end, dark_color);

    // Left rectangle
    draw_list->AddRectFilled(ImVec2(m_image_origin.x, sel_y), ImVec2(sel_x, sel_y + sel_h), dark_color);

    // Right rectangle
    draw_list->AddRectFilled(ImVec2(sel_x + sel_w, sel_y), ImVec2(m_image_end.x, sel_y + sel_h), dark_color);
}

void ScreenshotTool::DrawSelectionBorder()
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_w = m_selection.get_width();
    const float sel_h = m_selection.get_height();

    UpdateHandleHoverState();
    UpdateCursor();

    // Draw selection border
    draw_list->AddRect(ImVec2(sel_x, sel_y),
                       ImVec2(sel_x + sel_w, sel_y + sel_h),
                       rgba_t(0x0096ffFF).to_abgr(),
                       0.0f,
                       1.0f,
                       ImDrawFlags_None);

    if (!g_config->Runtime.enable_handles)
        return;

    // Draw handles
    const float handle_draw_half = HANDLE_DRAW_SIZE / 2.0f;
    auto        draw_handle      = [&](ImVec2 pos, HandleHovered type) {
        ImVec2 min = ImVec2(pos.x - handle_draw_half, pos.y - handle_draw_half);
        ImVec2 max = ImVec2(pos.x + handle_draw_half, pos.y + handle_draw_half);

        rgba_t color = rgba_t(0xffffffFF);
        if (m_handle_hover == type || m_dragging_handle == type)
            color.b = 0;  // Yellow

        draw_list->AddRectFilled(min, max, color.to_abgr());
        draw_list->AddRect(min, max, rgba_t(0xffffffFF).to_abgr(), 0.0f, 2.0f, ImDrawFlags_None);
    };

    // Corner handles
    draw_handle(ImVec2(sel_x, sel_y), HandleHovered::TopLeft);
    draw_handle(ImVec2(sel_x + sel_w, sel_y), HandleHovered::TopRight);
    draw_handle(ImVec2(sel_x, sel_y + sel_h), HandleHovered::BottomLeft);
    draw_handle(ImVec2(sel_x + sel_w, sel_y + sel_h), HandleHovered::BottomRight);

    // Edge handles
    draw_handle(ImVec2(sel_x + sel_w / 2, sel_y), HandleHovered::Top);
    draw_handle(ImVec2(sel_x + sel_w / 2, sel_y + sel_h), HandleHovered::Bottom);
    draw_handle(ImVec2(sel_x, sel_y + sel_h / 2), HandleHovered::Left);
    draw_handle(ImVec2(sel_x + sel_w, sel_y + sel_h / 2), HandleHovered::Right);

    // Selection size window
    // Show it when interacting with selections and if less
    // than 2 seconds are passed
    static double last_change_time = ImGui::GetTime();
    if (m_input_owner == InputOwner::Selection)
        last_change_time = ImGui::GetTime();

    if ((ImGui::GetTime() - last_change_time) < 2.f)
    {
        std::string str(fmt::format("{:.0f}x{:.0f}+{:.0f}+{:.0f}", sel_w, sel_h, sel_x, sel_y));
        if (g_config->File.image_out_size_fmt != "off")
        {
            static std::atomic<size_t> estimated_size{ 0 };
            static std::atomic<bool>   size_computing{ false };
            static selection_rect_t    tracked_selection{};    // last geometry observed, any source
            static selection_rect_t    committed_selection{};  // geometry estimated_size currently reflects
            static std::chrono::steady_clock::time_point last_change_ts{};

            auto same_geo = [](const selection_rect_t& a, const selection_rect_t& b) {
                return a.get_x() == b.get_x() && a.get_y() == b.get_y() && a.get_width() == b.get_width() &&
                       a.get_height() == b.get_height();
            };

            if (!same_geo(m_selection, tracked_selection))
            {
                tracked_selection = m_selection;
                last_change_ts    = std::chrono::steady_clock::now();
            }

            if (!size_computing && !same_geo(tracked_selection, committed_selection) &&
                std::chrono::steady_clock::now() - last_change_ts > 150ms)
            {
                committed_selection = tracked_selection;
                size_computing      = true;

                capture_result_t snapshot = GetFinalImage();  // render thread only, cheap crop
                ImageExt         ext      = g_config->File.image_out_type.second;

                std::thread([snapshot = std::move(snapshot), ext]() mutable {
                    std::vector<uint8_t> img = encode_to_image(snapshot, ext);
                    estimated_size.store(img.size(), std::memory_order_relaxed);
                    size_computing.store(false, std::memory_order_relaxed);
                }).detach();
            }
            byte_units_t byte_units =
                (g_config->File.image_out_size_fmt == "auto")
                    ? auto_divide_bytes(double(estimated_size.load()), 1024)
                    : divide_bytes(double(estimated_size.load()), g_config->File.image_out_size_fmt);

            str += fmt::format(
                "\n{}: {:.2f}{}", g_config->File.image_out_type.first, byte_units.num_bytes, byte_units.unit);
        }
        const ImVec2 str_size = ImGui::CalcTextSize(str.c_str());

        constexpr float kPaddingX = 6.0f;
        constexpr float kPaddingY = 4.5f;
        constexpr float kMargin   = 3.0f;  // gap between selection corner and the label

        const ImVec2 win_size(str_size.x + kPaddingX * 2.0f, str_size.y + kPaddingY * 2.0f);
        const ImVec2 win_pos((sel_x + sel_w) - win_size.x - kMargin, (sel_y + sel_h) - win_size.y - kMargin);

        ImGui::SetNextWindowPos(win_pos);
        ImGui::SetNextWindowSize(win_size);
        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPaddingX, kPaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

        if (ImGui::Begin("##selection_size_window",
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(str.c_str());
            ImGui::End();
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
    }
}

void ScreenshotTool::DrawMenuItems()
{
    if (ImGui::BeginMenuBar())
    {
        // Now draw the menus
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open Image..."))
            {
                minimize_window();

                const char* filter[]  = { "*.png", "*.jpeg", "*.jpg", "*.bmp" };
                const char* open_path = tinyfd_openFileDialog("Open Image",
                                                              "",        // default path
                                                              4,         // number of filter patterns
                                                              filter,    // file filters
                                                              "Images",  // filter description
                                                              false      // allow multiple selections
                );

                maximize_window();

                if (open_path)
                    OpenImage(open_path);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Save Image", "CTRL+S"))
                if (m_on_complete)
                    m_on_complete(SavingOp::File, GetFinalImage(), g_config->File.image_out_type.second);

            if (ImGui::MenuItem("Copy Image", g_config->File.ctrl_c_copy_img ? "CTRL+C" : "CTRL+SHIFT+C"))
                if (m_on_complete)
                    m_on_complete(SavingOp::Clipboard, GetFinalImage(), g_config->File.image_out_type.second);

            ImGui::Separator();

            if (ImGui::MenuItem("Quit", "ESC"))
                Cancel();

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::BeginMenu("Optimize OCR for..."))
            {
                if (ImGui::RadioButton("Automatic", g_config->Runtime.preferred_psm == 0))
                    g_config->Runtime.preferred_psm = 0;
                ImGui::RadioButton("Single Word", &g_config->Runtime.preferred_psm, tesseract::PSM_SINGLE_WORD);
                ImGui::RadioButton("Single Line", &g_config->Runtime.preferred_psm, tesseract::PSM_SINGLE_LINE);
                ImGui::RadioButton("Paragraph", &g_config->Runtime.preferred_psm, tesseract::PSM_SINGLE_BLOCK);
                ImGui::RadioButton("Automatic (Layout)", &g_config->Runtime.preferred_psm, tesseract::PSM_AUTO);
                ImGui::EndMenu();
            }

            ImGui::Separator();
            ImGui::MenuItem("View Handles", "CTRL+G", &g_config->Runtime.enable_handles);
            ImGui::MenuItem("Anns. in image scans", "", &g_config->File.render_anns);
            if (ImGui::MenuItem("Enable vsync", "", &g_config->File.enable_vsync))
                extern_glfwSwapInterval(int(g_config->File.enable_vsync));
            if (ImGui::MenuItem("Allow text edit", "CTRL+E", &g_config->File.allow_out_edit))
                ImGui::ClearActiveID();

            ImGui::Separator();
            if (ImGui::MenuItem("Preferences..."))
                m_show_window.Set(SubWindow::Preferences);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools"))
        {
            if (ImGui::MenuItem("Download OCR model"))
                m_show_window.Set(SubWindow::OcrDownload);
            ImGui::Separator();

#ifndef DISABLE_PLUGINS
            if (ImGui::MenuItem("Manage Plugins"))
                m_show_window.Set(SubWindow::ManagePlugins);
            if (ImGui::MenuItem("Install Plugins..."))
                m_show_window.Set(SubWindow::InstallPlugins);
            if (ImGui::MenuItem("Uninstall Plugins"))
                m_show_window.Set(SubWindow::UninstallPlugins);
            ImGui::Separator();
#endif

            if (ImGui::MenuItem("View Logs"))
                m_show_window.Set(SubWindow::Logs);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About"))
                m_show_window.Set(SubWindow::About);
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }
}

void ScreenshotTool::DrawAboutWindow()
{
    bool open = m_show_window.Has(SubWindow::About);
    if (!open)
        return;

    ImGui::SetNextWindowSize(ImVec2(350, 250), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        std::string_view text_display;
        const float      window_width  = ImGui::GetWindowSize().x;
        auto             centered_text = [&](const std::string_view text) {
            float name_width = ImGui::CalcTextSize(text.data()).x;
            ImGui::SetCursorPosX((window_width - name_width) / 2);
            return text;
        };

        // Centered image
        ImGui::SetCursorPosX((window_width - 24.0f) / 2);
        ImGui::Image(m_tool_textures[idx(ToolType::Logo)], ImVec2(32, 32));

        // Centered labels
        text_display = centered_text("oshot v" VERSION);
        ImGui::TextUnformatted(text_display.data());
        ImGui::Spacing();

        text_display = centered_text("Screenshot tool for extracting text on the fly");
        ImGui::TextUnformatted(text_display.data());
        ImGui::Spacing();

#ifdef DISABLE_PLUGINS
        text_display = centered_text("!!! NO PLUGINS SUPPORT !!!");
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", text_display.data());
        ImGui::Spacing();
#endif

        // More version details
        text_display = "More Details:";
        if (ImGui::TreeNode(text_display.data()))
        {
            ImGui::BeginChild("##scrollable_region", ImVec2(0, 100), false, ImGuiWindowFlags_HorizontalScrollbar);

            ImGui::TextUnformatted(version_infos.c_str());

            if (ImGui::Button("Copy text"))
                MUST_OK(g_clipboard.CopyText(version_infos), error("Failed to copy text: {}", _r.error_v()));
            ImGui::EndChild();
            ImGui::TreePop();
        }

        ImGui::TextUnformatted("Version: v" VERSION);
        ImGui::TextUnformatted("Created by: Toni500");
        ImGui::TextUnformatted("Copyright © 2026");
        ImGui::Spacing();

        ImGui::TextUnformatted("Support the project at ");
        ImGui::SameLine(0, 1);
        if (ImGui::TextLinkOpenURL("Toni500github/oshot", "https://github.com/Toni500github/oshot"))
            minimize_window();

        if (ImGui::Button("Close"))
            open = false;
        ImGui::End();
    }
    m_show_window.Set(SubWindow::About, open);
}

void ScreenshotTool::DrawOcrTools()
{
    ErrorContext<OcrError>& ectx = m_ocr_errors;

    std::string& ocr_path  = m_inputs.ocr_path;
    std::string& ocr_model = m_inputs.ocr_model;

    static size_t item_selected_idx = 0;

    auto push_error_style = [](bool cond) {
        if (cond)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
    };
    auto pop_error_label = [](bool cond, const char* label) {
        if (cond)
        {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", label);
        }
    };

    ImGui::PushID("OcrTools");
    ImGui::SeparatorText("OCR");

    const bool need_to_scan  = HasError(ectx, OcrError::NeedToScanDir);
    const bool invalid_path  = HasError(ectx, OcrError::InvalidPath);
    const bool invalid_model = HasError(ectx, OcrError::InvalidModel);

    // --- Path input ---
    push_error_style(invalid_path);
    push_error_style(need_to_scan);
    draw_input_text_folder("Path", "##ocr_path", [&] { SetError(ectx, OcrError::NeedToScanDir); }, ocr_path);
    if (need_to_scan && ImGui::Button("Scan"))
        RefreshOcrModels();
    ImGui::SameLine();
    const auto& it    = std::find(m_ocr_models_list.begin(), m_ocr_models_list.end(), ocr_model);
    item_selected_idx = (it != m_ocr_models_list.end()) ? std::distance(m_ocr_models_list.begin(), it) : 0;
    pop_error_label(invalid_path, "Invalid!");
    pop_error_label(need_to_scan, "Need to scan new directory");
    ImGui::SameLine();
    HelpMarker("Full path to the OCR models (.traineddata). Supports drag-and-drop");

    // --- Model combo (only shown when path is valid and isn't changed) ---
    if (!invalid_path && !need_to_scan)
    {
        push_error_style(invalid_model);

        const bool combo_open = ImGui::BeginCombo("Model", ocr_model.c_str(), ImGuiComboFlags_HeightLarge);

        // Don't infect the whole list of red
        if (invalid_model)
            ImGui::PopStyleColor();

        if (combo_open)
        {
            static ImGuiTextFilter filter;
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
                filter.Clear();
            }

            ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F);
            filter.Draw("##Filter", -FLT_MIN);

            for (size_t i = 0; i < m_ocr_models_list.size(); ++i)
            {
                if (filter.PassFilter(m_ocr_models_list[i].c_str()))
                {
                    const bool is_selected = (item_selected_idx == i);
                    if (ImGui::Selectable(m_ocr_models_list[i].c_str(), is_selected))
                    {
                        item_selected_idx = i;
                        ocr_model         = m_ocr_models_list[i];
                        ClearError(ectx, OcrError::InvalidModel);
                    }
                }
            }
            ImGui::EndCombo();
        }

        if (invalid_model)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Invalid!");
        }
    }

    // --- Extract button + result details ---
    if (!invalid_path && !invalid_model && !need_to_scan)
    {
        if (ImGui::Button("Extract Text"))
        {
            const Result<>& configure_res = m_ocr_api.Configure(ocr_path.c_str(), ocr_model.c_str());
            if (!configure_res.ok())
            {
                SetError(ectx, OcrError::FailedToOCR, configure_res.error_v());
            }
            else
            {
                Result<ocr_result_t> result = m_ocr_api.ExtractTextCapture(GetFinalImage(true));
                if (result.ok())
                {
                    ClearError(ectx, OcrError::FailedToOCR);
                    m_inputs.ocr_results = std::move(result.get());
#ifndef DISABLE_PLUGINS
                    if (!g_plugins.empty())
                    {
                        oshot_ocr_result_t ocr{
                            .text =
                                oshot_str_new(m_inputs.ocr_results.data.c_str(), m_inputs.ocr_results.data.length()),
                            .confidence = m_inputs.ocr_results.confidence,
                            .psm        = m_inputs.ocr_results.psm,
                        };
                        for (auto& [id, rt] : g_plugins)
                        {
                            if (!rt.enabled || !rt.plugin->on_ocr_done)
                                continue;
                            ScopedActivePlugin _(&rt);
                            rt.plugin->on_ocr_done(rt.state, &ocr);
                        }
                        oshot_str_free(&ocr.text);
                    }
#endif
                }
                else
                {
                    SetError(ectx, OcrError::FailedToOCR, result.error_v());
                }
            }
        }

        ImGui::SameLine();

        if (HasError(ectx, OcrError::FailedToOCR))
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                               "Failed to initialize OCR: %s",
                               GetError(ectx, OcrError::FailedToOCR).c_str());
        }
        else
        {
            ImGui::SameLine();
            HelpMarker("If results seem off, try Edit > Optimize OCR for...");
        }

        if (m_inputs.ocr_results.confidence > 0 && ImGui::TreeNode("Details"))
        {
            ImGui::BulletText("Confidence:");
            ImGui::SameLine();
            ImVec4 confidence_color(0.0f, 1.0f, 0.0f, 1.0f);  // green
            if (m_inputs.ocr_results.confidence <= 45)
                confidence_color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // red
            else if (m_inputs.ocr_results.confidence <= 70)
                confidence_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // yellow

            ImGui::TextColored(confidence_color, "%d%%", m_inputs.ocr_results.confidence);

            ImGui::BulletText("PSM: %s", m_inputs.ocr_results.psm_str.c_str());
            ImGui::TreePop();
        }
    }

    ImGui::InputTextMultiline("##" OCR_OUTPUT,
                              &m_inputs.ocr_results.data,
                              ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                              g_config->File.allow_out_edit ? 0 : ImGuiInputTextFlags_ReadOnly);

    if (!m_inputs.ocr_results.data.empty())
        CreateCopyTextButton(m_inputs.ocr_results.data);

    ImGui::PopID();
}

void ScreenshotTool::DrawBarDecodeTools()
{
    ErrorContext<ZbarError>& ectx = m_zbar_errors;

    ImGui::PushID("BarDecodeTools");
    ImGui::SeparatorText("QR/Bar Decode");

    if (ImGui::Button("Extract Text"))
    {
        const Result<zbar_result_t>& scan = m_zbar_api.ExtractTextsCapture(GetFinalImage(true));
        if (!scan.ok())
        {
            SetError(ectx, ZbarError::FailedToScan, scan.error_v());
        }
        else
        {
            ClearError(ectx, ZbarError::FailedToScan);
            m_inputs.zbar_scan_result = std::move(scan.get());
            m_inputs.barcode_text.clear();
            for (const std::string& data : m_inputs.zbar_scan_result.datas)
                m_inputs.barcode_text += data + "\n\n";
        }
    }

    if (HasError(ectx, ZbarError::FailedToScan))
    {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Failed to decode: %s", GetError(ectx, ZbarError::FailedToScan).c_str());
    }
    else if (!m_inputs.zbar_scan_result.datas.empty() && ImGui::TreeNode("Details"))
    {
        ImGui::Text("Detected barcodes:");
        for (const auto& [sym, count] : m_inputs.zbar_scan_result.symbologies)
            ImGui::BulletText("%s (x%d)", sym.c_str(), count);
        ImGui::TreePop();
    }

    ImGui::InputTextMultiline("##" ZBAR_OUTPUT,
                              &m_inputs.barcode_text,
                              ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                              g_config->File.allow_out_edit ? 0 : ImGuiInputTextFlags_ReadOnly);

    if (!m_inputs.barcode_text.empty())
        CreateCopyTextButton(m_inputs.barcode_text);

    ImGui::PopID();
}

void ScreenshotTool::DrawAnnotationToolbar()
{
    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_h = m_selection.get_height();

    constexpr float k_toolbar_offset   = 10.0f;
    constexpr float k_approx_toolbar_h = 40.0f;

    const float display_h = ImGui::GetIO().DisplaySize.y;

    const float below_y = sel_y + sel_h + k_toolbar_offset;
    const float above_y = sel_y - k_toolbar_offset - k_approx_toolbar_h;

    float toolbar_y = below_y;

    // Prefer below
    if (below_y + k_approx_toolbar_h > display_h)
    {
        // Try above
        toolbar_y = above_y;

        // If above also invalid, clamp
        if (toolbar_y < 0.0f)
            toolbar_y = std::clamp(below_y, 0.0f, display_h - k_approx_toolbar_h);
    }

    const ImVec2 toolbar_pos(sel_x, toolbar_y);
    ImGui::SetNextWindowPos(toolbar_pos);

    ImGui::Begin("##annotation_toolbar",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);

    // Tool selection buttons
    auto draw_and_set_button = [&](ToolType tool, const char* id, ImTextureRef texture) {
        const bool selected = (m_current_tool == tool);

        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));

        if (ImGui::ImageButton(id, texture.GetTexID(), ImVec2(24, 24)))
            m_current_tool = selected ? ToolType::kNone : tool;

        if (selected)
            ImGui::PopStyleColor();

        // Right-click popup on this item
        if (selected && ImGui::BeginPopupContextItem())
        {
            if (m_current_tool == ToolType::Text)
                m_tool_thickness[idx(m_current_tool)] = std::clamp(m_tool_thickness[idx(m_current_tool)], 8.0f, 144.0f);
            else
                m_tool_thickness[idx(m_current_tool)] = std::clamp(m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f);

            ImGuiColorEditFlags flags = g_config->File.color_picker == 0 ? ImGuiColorEditFlags_PickerHueBar
                                                                         : ImGuiColorEditFlags_PickerHueWheel;

            switch (ColorPickerAlpha(g_config->File.cpa_mode))
            {
                case ColorPickerAlpha::Disabled: flags |= ImGuiColorEditFlags_NoAlpha; break;
                case ColorPickerAlpha::Bar:      flags |= ImGuiColorEditFlags_AlphaBar; break;
                case ColorPickerAlpha::Inline:   /* no extra flag needed */ break;
            }

            ImGui::TextUnformatted("Annotation Settings");
            ImGui::Separator();
            ImGui::SetNextItemWidth(100);

            if (m_current_tool == ToolType::Text)
            {
                ImGui::InputFloat("##fontsize", &m_tool_thickness[idx(m_current_tool)], 2.0f, 2.0f, "%.0f px");
                ImGui::SameLine();
                ImGui::TextUnformatted("Font Size");
                static const char* font_filters[] = { "*.ttf", "*.otf", "*.woff", "*.woff2" };
                draw_input_text_file(
                    "Font name/path",
                    "##font_path_ann_settings",
                    font_filters,
                    4,
                    [&] { m_inputs.resolved_ann_font_path = get_font_path(m_inputs.ann_font).string(); },
                    m_inputs.ann_font);
            }
            else
            {
                ImGui::SliderFloat("##thickness", &m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f, "%.2f");
                ImGui::SameLine();
                ImGui::TextUnformatted("Thickness");
            }

            if (ImGui::Button("Pick color"))
            {
                m_current_actions.Set(CurrentAction::IsColorPicking);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            HelpMarker("Click anywhere on the image to pick a color");

            ImVec4 picker = m_current_color.to_imvec4();
            ImGui::ColorPicker4("Color", reinterpret_cast<float*>(&picker), flags);

            m_current_color = rgba_t(picker);
            g_cache->SetValue(CacheEntry::AnnColor, m_current_color.to_rgba());
            ImGui::EndPopup();
        }

        ImGui::SameLine();
    };

    draw_and_set_button(ToolType::Arrow, "##Arrow", m_tool_textures[idx(ToolType::Arrow)]);
    draw_and_set_button(ToolType::Line, "##Line", m_tool_textures[idx(ToolType::Line)]);
    draw_and_set_button(ToolType::Rectangle, "##Rectangle", m_tool_textures[idx(ToolType::Rectangle)]);
    draw_and_set_button(
        ToolType::RectangleFilled, "##Rectangle_filled", m_tool_textures[idx(ToolType::RectangleFilled)]);
    draw_and_set_button(ToolType::Circle, "##Circle", m_tool_textures[idx(ToolType::Circle)]);
    draw_and_set_button(ToolType::CircleFilled, "##Circle_filled", m_tool_textures[idx(ToolType::CircleFilled)]);
    draw_and_set_button(ToolType::CounterBubble, "##Counter_bubble", m_tool_textures[idx(ToolType::CounterBubble)]);
    draw_and_set_button(ToolType::Text, "##icon_Text", m_tool_textures[idx(ToolType::Text)]);
    draw_and_set_button(ToolType::Pencil, "##Pencil", m_tool_textures[idx(ToolType::Pencil)]);

    ImGui::SameLine(0, 16.0f);

    if (!m_show_window.Has(SubWindow::MainTextTools))
    {
        if (ImGui::ImageButton("##ShowTextTools", m_tool_textures[idx(ToolType::ToggleTextTools)], ImVec2(24, 24)))
            m_show_window.Set(SubWindow::MainTextTools);
        ImGui::SameLine();
    }

    if (ImGui::ImageButton("##CopyImageButton", m_tool_textures[idx(ToolType::CopyImage)], ImVec2(24, 24)) &&
        m_on_complete)
        m_on_complete(SavingOp::Clipboard, GetFinalImage(), g_config->File.image_out_type.second);

    ImGui::SameLine();

    if (ImGui::ImageButton("##SaveImageButton", m_tool_textures[idx(ToolType::SaveImage)], ImVec2(24, 24)) &&
        m_on_complete)
        m_on_complete(SavingOp::File, GetFinalImage(), g_config->File.image_out_type.second);

    ImGui::SameLine();
    ImGui::Separator();

    ImGui::SameLine();
    if (ImGui::Button("Undo") && !m_annotations.empty())
        m_annotations.pop_back();

    ImGui::End();
}

static void draw_preference_edit_config(const std::function<void()>& refresh_models_func, bool window_just_opened)
{
    static const char* image_prev_units[] = { "off", "auto", "B", "KiB", "MiB", "KB", "MB" };
    static const char* font_filters[]     = { "*.ttf", "*.otf", "*.ttc", "*.woff", "*.woff2" };
    static const char* toml_filters[]     = { "*.toml" };

    static int                   image_ext_sel    = 0;
    static int                   image_preuni_sel = 0;
    static std::string           new_font;
    static Result<std::string>   r = get_config_image_out_fmt();
    static std::vector<fs::path> resolved_font_paths;
    static bool                  should_refocus = false;

    // Rebuild cache on open or when the list changes
    auto rebuild_font_cache = [&]() {
        resolved_font_paths.clear();
        for (const std::string& f : g_config->File.fonts)
            resolved_font_paths.push_back(get_font_path(f));
    };

    if (window_just_opened)
        rebuild_font_cache();

    ImGui::SeparatorText("Edit default config");
    ImGui::Spacing();

    // --- OCR path ---
    ImGui::Text("Default OCR path");
    ImGui::SameLine();
    HelpMarker("Full path to the OCR models (.traineddata). Supports drag-and-drop.");
    draw_input_text_folder("", "##config_ocr_path", refresh_models_func, g_config->File.ocr_path);
    ImGui::Spacing();

    // --- OCR model ---
    ImGui::Text("Default OCR model");
    ImGui::InputText("##config_ocr_model", &g_config->File.ocr_model);
    ImGui::Spacing();

    // --- Capture delay ---
    ImGui::Text("Capture delay");
    ImGui::SameLine();
    HelpMarker(
        "Delay before acquiring a screenshot (milliseconds).\n"
        "Has no effect when opening an external image (e.g. -f flag).");
    ImGui::InputInt("##config_delay", &g_config->File.delay, 5, 10);
    ImGui::Spacing();

    // --- Theme settings ---
    ImGui::Text("Default Theme file path");
    ImGui::SameLine();
    HelpMarker(
        "Full path to the theme file (aka. theme.toml), or relative to the config directory. Supports drag-and-drop.");
    draw_input_text_file(
        "",
        "##config_theme_file_path",
        toml_filters,
        1,
        [&] {
            if (fs::path(g_config->File.theme_file_path).is_relative())
                g_config->File.theme_file_path.insert(0, g_config->GetConfigDirPath());
        },
        g_config->File.theme_file_path);
    if (!fs::exists(g_config->File.theme_file_path))
        ImGui::TextColored(rgba_t(0xff7444FF).to_imvec4(), "File doesn't exist, fallback to default hardcoded theme");
    ImGui::Spacing();

    ImGui::Text("Color picker style");
    ImGui::Combo("##config_color_picker", &g_config->File.color_picker, "Bar - Square\0Wheel - Triangle\0\0");
    ImGui::Spacing();

    ImGui::Text("Annotation alpha editing");
    ImGui::Combo(
        "##config_color_picker_alpha_mode", &g_config->File.cpa_mode, "Disabled\0Inline slider\0Dedicated bar\0\0");
    ImGui::Spacing();

    // --- Checkboxes ---
    ImGui::Checkbox("Exclusive fullscreen##config_real_full_screen", &g_config->File.real_full_screen);
    ImGui::SameLine();
    HelpMarker(
        "On some desktop environments (e.g. MATE) the compositor may make "
        "the capture window look grainy. Enabling this uses exclusive fullscreen "
        "to bypass the compositor.\n"
        "Downside: the window may briefly take over the display on some setups.");

    ImGui::Checkbox("Vertical sync (VSync)##config_vsync", &g_config->File.enable_vsync);
    ImGui::SameLine();
    HelpMarker(
        "Renders in sync with your monitor's refresh rate for a smoother overlay, "
        "at the cost of slightly more CPU/GPU usage.\n"
        "Disable if the overlay feels sluggish.");

    ImGui::Checkbox("Allow output edits##config_allow_out_edit", &g_config->File.allow_out_edit);

    ImGui::Checkbox("Show text tools at startup##config_show_text_tools", &g_config->File.show_text_tools);

    ImGui::Checkbox("Prefer config variables over environment variables##config_pref_conf_to_env",
                    &g_config->File.pref_conf_to_env);
    ImGui::SameLine();
    HelpMarker(
        "In some OSes such as NixOS, in order to get the OCR models installed through the package manager, we need to "
        "rely on environment variables such as $TESSDATA_PREFIX."
        "Enabling this option overrides those environment variables with the paths set in the config file.");

    ImGui::Checkbox("Consider annotations when scanning##config_render_anns", &g_config->File.render_anns);
    ImGui::SameLine();
    HelpMarker("When enabled, annotations are included in the region passed to the text extractor.");

    ImGui::Checkbox("Use CTRL+C to copy image##config_ctrl_c_copy_img", &g_config->File.ctrl_c_copy_img);
    ImGui::SameLine();
    HelpMarker(
        "Shortcut to use when copying the image selection.\n"
        "If disabled, the shortcut will be CTRL+SHIFT+C.");

    // --- Image filename output format section ---
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Output filename extension");
    if (ImGui::BeginCombo("##config_image_out_ext", g_config->File.image_out_type.first.c_str()))
    {
        for (size_t i = 0; i < idx(ImageExt::COUNT); ++i)
        {
            bool selected = (image_ext_sel == int(i));

            if (ImGui::Selectable(IMAGE_EXTS_STR[i].second, image_ext_sel))
            {
                image_ext_sel                        = i;
                g_config->File.image_out_type.first  = IMAGE_EXTS_STR[i].second;
                g_config->File.image_out_type.second = toe<ImageExt>(i);
            }

            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    ImGui::Text("Size preview unit");
    ImGui::SameLine();
    HelpMarker("Unit used to show the selection's estimated file size next to the selection border");
    if (ImGui::BeginCombo("##config_image_out_size_fmt", g_config->File.image_out_size_fmt.c_str()))
    {
        for (int i = 0; i < IM_ARRAYSIZE(image_prev_units); ++i)
        {
            bool selected = image_preuni_sel == int(i);

            if (ImGui::Selectable(image_prev_units[i], image_preuni_sel))
            {
                image_preuni_sel                  = i;
                g_config->File.image_out_size_fmt = image_prev_units[i];
            }

            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    ImGui::Text("Output filename format");
    ImGui::SameLine();
    HelpMarker("The image extension is appended automatically. Uses {fmt} chrono specifiers.");
    ImGui::Spacing();

    if (ImGui::InputText("##config_image_out_fmt", &g_config->File.image_out_fmt))
        r = get_config_image_out_fmt();

    if (!r.ok())
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", r.error_v().c_str());
    else
        ImGui::TextDisabled("%s", r.get().c_str());

    ImGui::Spacing();

    if (ImGui::TreeNode("Format help"))
    {
        auto row = [](const char* spec, const char* desc) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", spec);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(desc);
        };

        ImGui::Spacing();
        ImGui::TextDisabled("Specifiers:");
        ImGui::Indent();
        ImGui::TextDisabled("Date:");
        ImGui::TableSetupColumn("##spec", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("##desc", ImGuiTableColumnFlags_WidthStretch);
        if (ImGui::BeginTable("##fmt_date", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%Y", "full year                (e.g. 2025)");
            row("%y", "2-digit year             (e.g. 25)");
            row("%C", "century                  (e.g. 20)");
            row("%m", "month (01-12)");
            row("%d", "day of month (01-31)");
            row("%j", "day of year (001-366)");
            row("%F", "short for %Y-%m-%d       (e.g. 2025-04-19)");
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("Time:");
        if (ImGui::BeginTable("##fmt_time", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%H", "hour 24h      (00-23)");
            row("%I", "hour 12h      (01-12)");
            row("%M", "minute        (00-59)");
            row("%S", "second        (00-60)");
            row("%p", "AM / PM");
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("Week / Weekday:");
        if (ImGui::BeginTable("##fmt_week", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%u", "weekday ISO                    (1=Mon ... 7=Sun)");
            row("%w", "weekday                        (0=Sun ... 6=Sat)");
            row("%a", "abbreviated weekday            (e.g. Sat)");
            row("%A", "full weekday name              (e.g. Saturday)");
            row("%U", "week of year, Sun-start        (00-53)");
            row("%W", "week of year, Mon-start        (00-53)");
            row("%V", "ISO week number                (01-53)");
            row("%G", "ISO week-based year            (e.g. 2025)");
            row("%g", "ISO week-based year, 2-digit   (e.g. 25)");
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("Month name:");
        if (ImGui::BeginTable("##fmt_month", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%b", "abbreviated month           (e.g. Apr)");
            row("%B", "full month name             (e.g. April)");
            ImGui::EndTable();
        }
        ImGui::Spacing();

        ImGui::TextDisabled("Other:");
        if (ImGui::BeginTable("##fmt_other", 2, ImGuiTableFlags_SizingFixedFit))
        {
            row("%%", "literal %");
            ImGui::EndTable();
        }
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::TextDisabled("Examples:");
        ImGui::Indent();
        ImGui::BulletText("oshot_{:%%F_%%H-%%M}-test ->  oshot_2025-04-19_14-30-test.%s",
                          g_config->File.image_out_type.first.c_str());
        ImGui::BulletText("oshot_{:%%F_%%H-%%M-%%S}   ->  oshot_2025-04-19_14-30-05.%s",
                          g_config->File.image_out_type.first.c_str());
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::TextDisabled("Warnings:");
        ImGui::Indent();
        ImGui::BulletText("The colon inside {} is required: {:%%F} correct, {%%F} will error.");
        ImGui::BulletText("Avoid %%T -- it expands to HH:MM:SS (colons break paths on Windows).");
        ImGui::BulletText("%%H-%%M is 1-minute precise; add %%S if you take rapid shots.");
        ImGui::Unindent();
        ImGui::Spacing();
        ImGui::TreePop();
    }

    // --- Fonts section ---
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped(
        "Fonts for the application. Accepts an absolute path or a font name.\n"
        "Combine multiple entries for multi-language support, e.g. "
        "Roboto-Regular.ttf and NotoSerifCJK-Regular.ttc for CJK alongside Latin.");
    ImGui::Spacing();

    // Scrollable font list with remove buttons
    std::vector<std::string>& fonts       = g_config->File.fonts;
    const float               list_height = ImGui::GetTextLineHeightWithSpacing() * 4.5f;
    ImGui::BeginChild("##font_list", ImVec2(0, list_height), true);
    for (size_t i = 0; i < fonts.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("x"))
        {
            fonts.erase(fonts.begin() + i);
            rebuild_font_cache();  // invalidate
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(fonts[i].c_str());
        ImGui::SameLine();
        const fs::path& fp = (i < resolved_font_paths.size()) ? resolved_font_paths[i] : fs::path{};
        if (fp.empty())
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Font not found");
        else if (!fs::path(fonts[i]).is_absolute())
            ImGui::TextDisabled("Found in %s", fp.string().c_str());
        ImGui::PopID();
    }

    if (fonts.empty())
        ImGui::TextDisabled("(no fonts configured - application default will be used)");

    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextUnformatted("Font path: ");
    ImGui::SameLine(0, 0);
    if (should_refocus)
    {
        ImGui::SetKeyboardFocusHere(0);
        should_refocus = false;
    }
    draw_input_text_file("", "##font_path", font_filters, 5, [&] {}, new_font, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add font"))
    {
        if (!new_font.empty() && std::find(fonts.begin(), fonts.end(), new_font) == fonts.end())
        {
            fonts.push_back(new_font);
            rebuild_font_cache();  // invalidate
        }
        new_font.clear();
        should_refocus = true;
    }
}

#ifndef DISABLE_PLUGINS
static void draw_preference_plugin(auto& plugin_dirty, bool& prefs_modified)
{
    ImGui::Text("Edit settings per plugin");
    ImGui::Spacing();

    for (auto& [id, rt] : g_plugins)
    {
        if (!rt.enabled)
            continue;

        oshot_plugin_t* pl = rt.plugin;
        if (!pl->render_preferences)
        {
            spdlog::warn("Plugin '{}' doesn't have a render_preferences() function. Skipping", pl->id);
            continue;
        }
        ScopedActivePlugin _(&rt);

        ImGui::PushID(pl->id);
        ImGui::SeparatorText(fmt::format("{} ({})", pl->name, pl->id).c_str());
        ImGui::Spacing();

        ImGui::BeginChild(pl->name, ImVec2(0.0f, 0.0f), ImGuiWindowFlags_AlwaysAutoResize);
        if (pl->render_preferences(rt.state))
        {
            plugin_dirty[id] = true;
            prefs_modified   = true;
        }
        ImGui::EndChild();

        ImGui::PopID();

        ImGui::Spacing();
    }
}
#endif

static void draw_theme_editor()
{
    Config::theme_overrides_t& ov = g_config->theme_overrides;

    ImGui::SeparatorText("Custom theme options");

    ImGui::Checkbox("Enable smooth animations", &ov.smooth_animations);

    ImGui::SeparatorText("Style overrides");

    // Rounding / border sliders
    auto style_row = [&](const char* label, float& val, float lo, float hi) {
        float v = (val < 0.f) ? 0.f : val;
        if (ImGui::SliderFloat(label, &v, lo, hi, "%.1f"))
            val = v;
        ImGui::SameLine();
        if (ImGui::SmallButton(("reset##" + std::string(label)).c_str()))
            val = -1.f;
    };

    style_row("Window rounding", ov.window_rounding, 0, 12);
    style_row("Frame rounding", ov.frame_rounding, 0, 12);
    style_row("Grab rounding", ov.grab_rounding, 0, 12);
    style_row("Tab rounding", ov.tab_rounding, 0, 12);
    style_row("Window border", ov.window_border, 0, 2);
    style_row("Frame border", ov.frame_border, 0, 2);

    ImGui::Spacing();
    ImGui::SeparatorText("Color overrides");
    ImGui::TextDisabled("Click a swatch to edit. Changes apply on Save.");
    ImGui::Spacing();

    // We only show the most-used slots to keep the list manageable.
    // Full list is still editable via the config file directly.
    static constexpr const char* important[] = {
        "Text",          "TextDisabled",  "WindowBg",       "ChildBg",       "PopupBg",       "Border",
        "PlotHistogram", "FrameBg",       "FrameBgHovered", "FrameBgActive", "TitleBg",       "TitleBgActive",
        "MenuBarBg",     "ScrollbarBg",   "ScrollbarGrab",  "CheckMark",     "SliderGrab",    "SliderGrabActive",
        "Button",        "ButtonHovered", "ButtonActive",   "Header",        "HeaderHovered", "HeaderActive",
        "Tab",           "TabHovered",    "TabSelected",
    };

    const auto& cmap = color_name_map();

    ImGui::BeginChild("##color_list", ImVec2(0, 300), true);
    for (const char* name : important)
    {
        auto it = cmap.find(name);
        if (it == cmap.end())
            continue;

        // Resolve current color: override first, then live style
        ImVec4 col = ImGui::GetStyle().Colors[it->second];

        auto oit = ov.colors.find(name);
        if (oit != ov.colors.end())
        {
            const std::string& h = oit->second;
            if (!h.empty() && h[0] == '#')
                hexstr_to_imvec4(h, col);
        }
        ImGui::PushID(name);
        if (ImGui::ColorEdit4(
                "##c", reinterpret_cast<float*>(&col), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            ov.colors[name] = col_to_hexstr(col);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(name);

        // Allow clearing individual override
        if (oit != ov.colors.end())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                ov.colors.erase(name);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button("Clear all color overrides"))
        ov.colors.clear();
    ImGui::SameLine();
    HelpMarker("Reverts to the base theme colors on next Save.");
}

void ScreenshotTool::DrawPreferencesWindow()
{
#ifndef DISABLE_PLUGINS
    static constexpr const char* items[] = { "Defaults", "Plugins", "Theme" };
#else
    static constexpr const char* items[] = { "Defaults", "Theme" };
#endif

    static bool    prefs_modified     = false;
    static bool    prev_window_open   = false;
    static bool    armed              = false;
    const bool     window_just_opened = !prev_window_open;
    static PrefTab selected_tab       = PrefTab::Defaults;

    static Config::config_file_t                 config_snapshot;  // config state at the moment the window opened
    static Config::theme_overrides_t             theme_snapshot;   // theme state at the moment the window opened
    static std::unordered_map<std::string, bool> plugin_dirty;

    if (!m_show_window.Has(SubWindow::Preferences))
    {
        prev_window_open = false;
        return;
    }

    prev_window_open = true;

    auto save_current_tab = [&]() {
        if (config_snapshot != g_config->File)
        {
            g_config->GenerateConfig(g_config->GetConfigPath(), true);
            g_config->LoadConfigFile(g_config->GetConfigPath());
            SyncRuntimeFromConfig();
            if (!g_config->File.theme_file_path.empty())
                g_config->LoadThemeFile(g_config->File.theme_file_path);
            apply_imgui_theme();
            config_snapshot = g_config->File;
            theme_snapshot  = g_config->theme_overrides;
        }

        if (theme_snapshot != g_config->theme_overrides)
        {
            if (!g_config->File.theme_file_path.empty())
            {
                g_config->GenerateTheme(g_config->File.theme_file_path, true);
                g_config->LoadThemeFile(g_config->File.theme_file_path);
                color_name_map().clear();
            }
            else
            {
                ImGui::OpenPopup("Theme file path is empty##theme_filepath_empty");
            }

            apply_imgui_theme();
            theme_snapshot = g_config->theme_overrides;
        }

#ifndef DISABLE_PLUGINS
        for (auto& [id, rt] : g_plugins)
        {
            if (!plugin_dirty[id] || !rt.enabled || !rt.plugin->on_save_preferences)
                continue;

            ScopedActivePlugin _(&rt);
            rt.plugin->on_save_preferences(rt.state);  // plugin calls oshot_config_set_* here, populating rt.config
            rt.config.SaveFile(rt.config_path.string());
            plugin_dirty[id] = false;
        }
#endif
    };

    auto discard_current_tab = [&]() {
        g_config->File            = config_snapshot;
        g_config->theme_overrides = theme_snapshot;
#ifndef DISABLE_PLUGINS
        for (auto& [id, rt] : g_plugins)
        {
            if (!plugin_dirty[id] || !rt.enabled || !rt.plugin->on_discard_preferences)
                continue;
            ScopedActivePlugin _(&rt);
            rt.plugin->on_discard_preferences(rt.state);
            plugin_dirty[id] = false;
        }
#endif
    };

    // Snapshot the config when the window first appears so Discard can restore it.
    if (window_just_opened)
    {
        config_snapshot = g_config->File;
        theme_snapshot  = g_config->theme_overrides;
    }

    if (config_snapshot != g_config->File || theme_snapshot != g_config->theme_overrides)
        prefs_modified = true;

    bool window_open = true;
    ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Preferences", &window_open, ImGuiWindowFlags_NoSavedSettings))
    {
        // [x] or Esc was clicked this frame: either close cleanly or ask first.
        // OpenPopup must be called inside Begin/End, so we handle it here
        // before rendering any content.
        if (!window_open || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (prefs_modified)
                ImGui::OpenPopup("Unsaved changes##pref");
            else
                m_show_window.Clear(SubWindow::Preferences);
            // Either way, don't propagate to show_preferences_window yet,
            // the window_open local resets to true next frame automatically.
        }

        // Left
        ImGui::BeginChild("##left_panel", ImVec2(150, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        for (int i = 0; i < IM_ARRAYSIZE(items); i++)
            if (ImGui::Selectable(items[i], selected_tab == toe<PrefTab>(i)))
                selected_tab = toe<PrefTab>(i);
        ImGui::EndChild();

        ImGui::SameLine();

        // Right
        ImGui::BeginGroup();
        ImGui::BeginChild("##right_panel", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
        switch (selected_tab)
        {
            case PrefTab::kNone:    break;
            case PrefTab::Defaults: draw_preference_edit_config([&] { RefreshOcrModels(); }, window_just_opened); break;
#ifndef DISABLE_PLUGINS
            case PrefTab::Plugins: draw_preference_plugin(plugin_dirty, prefs_modified); break;
#endif
            case PrefTab::Theme: draw_theme_editor(); break;
        }
        ImGui::EndChild();

        if (prefs_modified && create_timed_button("Save##save_press", "Saved!", armed))
        {
            save_current_tab();
            prefs_modified = false;
            armed          = true;
        }
        else if (prefs_modified && selected_tab == PrefTab::Defaults)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Restart oshot for the new settings to take effect");
        }

        ImGui::EndGroup();

        // Confirmation modal
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Unsaved changes##pref", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("You have unsaved changes. What would you like to do?");
            ImGui::Spacing();

            if (ImGui::Button("Save & Close", ImVec2(110, 0)))
            {
                save_current_tab();
                m_show_window.Clear(SubWindow::Preferences);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(80, 0)))
            {
                discard_current_tab();
                m_show_window.Clear(SubWindow::Preferences);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0)))
            {
                // Do nothing
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
                "Theme file path is empty##theme_filepath_empty", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(
                "Please set a theme file path like 'theme.toml' before saving theme changes.\n"
                "Changes will be set temporarily for the current session.");
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(100, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    // Clean up tracking state after a confirmed close so the next open is fresh.
    if (!m_show_window.Has(SubWindow::Preferences))
    {
        prev_window_open = false;
        prefs_modified   = false;
    }
}

#ifndef DISABLE_PLUGINS
void ScreenshotTool::DrawManagePluginsWindow()
{
    bool open = m_show_window.Has(SubWindow::ManagePlugins);
    if (!open)
        return;

    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Manage plugins##manage_plugins_window", &open, ImGuiWindowFlags_NoSavedSettings))
    {
        if (m_install_state && m_install_state->running)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                               "An install is in progress, this list will refresh once it's done.");
            ImGui::End();
            m_show_window.Set(SubWindow::ManagePlugins, open);
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, rgba_t(0x999999FF).to_imvec4());
        ImGui::TextWrapped("Changes take effect after restarting oshot.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // small helper: draws a compact rounded tag, returns width used
        auto draw_tag = [](const char* label, const rgba_t col) {
            ImVec2 text_size = ImGui::CalcTextSize(label);
            ImVec2 pad(6.0f, 2.0f);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 size(text_size.x + pad.x * 2, text_size.y + pad.y * 2);

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), col.to_abgr(), 4.0f);
            draw_list->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), 0xFFffffff, label);

            ImGui::Dummy(size);
        };

        for (const manifest_t& repo : m_plugin_manager.GetStateManager().GetAllRepos())
        {
            ImGui::PushID(repo.name.c_str());

            if (ImGui::CollapsingHeader(
                    repo.name.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                ImGui::Indent();

                ImGui::TextDisabled("Homepage:");
                ImGui::SameLine(0, 4);
                if (ImGui::TextLinkOpenURL(repo.url.c_str(), repo.url.c_str()))
                    minimize_window();

                ImGui::SameLine();
                ImGui::TextDisabled("  |  commit %.7s", repo.git_hash.c_str());
                ImGui::Spacing();

                for (const plugin_t& plugin : repo.plugins)
                {
                    std::error_code ec;
                    ImGui::PushID(plugin.id.c_str());

                    const fs::path enabled_path  = plugin.library;
                    const fs::path disabled_path = fs::path(plugin.library).concat(".disabled");
                    const bool     is_enabled    = fs::exists(enabled_path);
                    const bool     is_disabled   = fs::exists(disabled_path);
                    const bool     is_missing    = !is_enabled && !is_disabled;

                    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba_t(0xffffff07).to_imvec4());
                    ImGui::PushStyleColor(ImGuiCol_Border, rgba_t(0xffffff14).to_imvec4());

                    ImGui::BeginChild("card",
                                      ImVec2(0, 0),
                                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                      ImGuiWindowFlags_NoScrollbar);

                    // Header row: name + id on the left, status badge on the right
                    ImGui::TextUnformatted(plugin.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", plugin.id.c_str());

                    ImVec2 tag_size = ImGui::CalcTextSize(is_disabled ? "Disabled" : "Enabled");
                    tag_size.x += 12.0f;
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - tag_size.x + ImGui::GetCursorPosX());
                    if (is_missing)
                        draw_tag("Missing", rgba_t(0x8c2626FF));
                    else if (is_enabled)
                        draw_tag("Enabled", rgba_t(0x197233FF));
                    else
                        draw_tag("Disabled", rgba_t(0x725919FF));

                    // Description
                    if (!plugin.description.empty())
                    {
                        ImGui::Spacing();
                        ImGui::TextWrapped("%s", plugin.description.c_str());
                    }

                    ImGui::Spacing();

                    // Authors
                    if (!plugin.authors.empty())
                    {
                        std::string authors(fmt::format("{}", fmt::join(plugin.authors, ", ")));
                        ImGui::TextDisabled("By %s", authors.c_str());
                    }

                    // Licenses as tags
                    if (!plugin.licenses.empty())
                    {
                        ImGui::TextDisabled("License:");
                        for (const std::string& license : plugin.licenses)
                        {
                            ImGui::SameLine();
                            draw_tag(license.c_str(), rgba_t(0x334c7fFF));
                        }
                    }

                    // Platforms as tags
                    if (!plugin.platforms.empty())
                    {
                        ImGui::TextDisabled("Platforms:");
                        for (const std::string& plat : plugin.platforms)
                        {
                            ImGui::SameLine();
                            draw_tag(plat.c_str(), rgba_t(0x3f3f3fFF));
                        }
                    }

                    ImGui::Spacing();

                    if (is_missing)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                           "Library not found at: %s",
                                           plugin.library.string().c_str());
                    }
                    else
                    {
                        // Right-align the action button
                        const char* btn_label = is_enabled ? "Disable" : "Enable";
                        float       btn_width = ImGui::CalcTextSize(btn_label).x + ImGui::GetStyle().FramePadding.x * 2;
                        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - btn_width + ImGui::GetCursorPosX());

                        if (is_enabled)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.08f, 0.08f, 1.0f));
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.45f, 0.20f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.55f, 0.28f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.38f, 0.16f, 1.0f));
                        }

                        if (ImGui::Button(btn_label))
                        {
                            ec.clear();
                            if (is_enabled)
                                fs::rename(enabled_path, disabled_path, ec);
                            else
                                fs::rename(disabled_path, enabled_path, ec);

                            if (ec)
                                error("Failed to {} plugin '{}': {}", btn_label, plugin.id, ec.message());
                        }
                        ImGui::PopStyleColor(3);
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
        ImGui::End();
    }

    m_show_window.Set(SubWindow::ManagePlugins, open);
}

void ScreenshotTool::DrawUninstallPluginsWindow()
{
    bool open = m_show_window.Has(SubWindow::UninstallPlugins);
    if (!open)
        return;

    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Uninstall plugins##uninstall_plugins_window", &open, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, rgba_t(0x999999FF).to_imvec4());
        ImGui::TextWrapped("Changes take effect after restarting oshot.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const auto& repos = m_plugin_manager.GetStateManager().GetAllRepos();

        if (repos.empty())
            ImGui::TextDisabled("No plugin repositories installed.");

        auto draw_tag = [](const char* label, const rgba_t col) {
            const ImVec2 text_size = ImGui::CalcTextSize(label);
            const ImVec2 pad(6.0f, 2.0f);
            const ImVec2 size(text_size.x + pad.x * 2, text_size.y + pad.y * 2);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), col.to_abgr(), 4.0f);
            draw_list->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), rgba_t(0xffffffFF).to_abgr(), label);

            ImGui::Dummy(size);
        };

        // Deferred out of the loop: RemoveRepo() must never run while `repos`
        // is still being iterated, and OpenPopup must fire from the same
        // ID-stack context as BeginPopupModal below, not from inside
        // PushID(repo.name)/BeginChild("card"), or the two won't resolve to
        // the same ImGuiID and the modal will never appear.
        static std::string pending_repo;
        static size_t      pending_plugin_count = 0;
        bool               request_popup        = false;

        for (const manifest_t& repo : repos)
        {
            ImGui::PushID(repo.name.c_str());

            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 0.027f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.078f));

            ImGui::BeginChild("card",
                              ImVec2(0, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar);
            ImGui::TextUnformatted(repo.name.c_str());

            if (!repo.url.empty())
            {
                ImGui::TextDisabled("Homepage:");
                ImGui::SameLine(0, 4);
                if (ImGui::TextLinkOpenURL(repo.url.c_str(), repo.url.c_str()))
                    minimize_window();
            }

            if (!repo.git_hash.empty())
                ImGui::TextDisabled("Commit: %.7s", repo.git_hash.c_str());

            ImGui::Spacing();

            ImGui::TextDisabled("Plugins:");
            const float window_visible_x2 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            bool        same_line         = true;
            for (size_t i = 0; i < repo.plugins.size(); ++i)
            {
                if (same_line)
                    ImGui::SameLine();
                draw_tag(repo.plugins[i].name.c_str(), rgba_t(0x334c7fFF));

                if (i + 1 < repo.plugins.size())
                {
                    const float last_x2 = ImGui::GetItemRectMax().x;
                    const float next_w  = ImGui::CalcTextSize(repo.plugins[i + 1].name.c_str()).x + 12.0f;
                    same_line           = last_x2 + ImGui::GetStyle().ItemSpacing.x + next_w < window_visible_x2;
                }
            }

            const char* btn_label = "Uninstall";
            float       btn_width = ImGui::CalcTextSize(btn_label).x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - btn_width + ImGui::GetCursorPosX());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.08f, 0.08f, 1.0f));

            if (ImGui::Button(btn_label))
            {
                pending_repo         = repo.name;
                pending_plugin_count = repo.plugins.size();
                request_popup        = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::EndChild();

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            ImGui::PopID();
            ImGui::Spacing();
        }

        // Same ID-stack context as the OpenPopup call below: window root,
        // no PushID, no child. This is what makes the two IDs actually match.
        if (request_popup)
            ImGui::OpenPopup("Confirm uninstall##uninstall_confirm");

        if (ImGui::BeginPopupModal("Confirm uninstall##uninstall_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Remove '%s' and its %zu plugin(s)?", pending_repo.c_str(), pending_plugin_count);
            ImGui::TextWrapped(
                "This deletes the repository cache, its config, and all plugin configs under it. "
                "This cannot be undone.");
            ImGui::Spacing();

            if (ImGui::Button("Cancel"))
            {
                pending_repo.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.08f, 0.08f, 1.0f));
            if (ImGui::Button("Uninstall"))
            {
                MUST_OK(m_plugin_manager.RemoveRepo(pending_repo),
                        spdlog::error("Failed to remove repository: {}", _r.error_v()));
                pending_repo.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);

            ImGui::EndPopup();
        }
    }
    ImGui::End();

    m_show_window.Set(SubWindow::UninstallPlugins, open);
}

void ScreenshotTool::StartInstall(const std::string& source)
{
    // The UI only ever shows an enabled "Install" button while no install is
    // running (see DrawInstallPluginsWindow), so this is a defensive check,
    // not the primary guard.
    if (m_install_state && m_install_state->running)
        return;

    if (m_install_thread.joinable())
        m_install_thread.join();

    auto state = std::make_shared<plugin_install_state_t>();

    // Every on_* callback just appends to the shared queue. Nothing here
    // touches ImGui, so it's safe to invoke from the worker thread.
    auto push = [state](plugin_install_event_t::Kind kind) {
        return [state, kind](const std::string_view msg) {
            std::lock_guard lock(state->events_mutex);
            state->pending_events.push_back({ kind, std::string(msg) });
        };
    };

    PluginCallbacks cb;
    cb.on_status  = push(plugin_install_event_t::Kind::Status);
    cb.on_success = push(plugin_install_event_t::Kind::Success);
    cb.on_warning = push(plugin_install_event_t::Kind::Warning);
    cb.on_error   = push(plugin_install_event_t::Kind::Error);
    cb.on_info    = push(plugin_install_event_t::Kind::Info);

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

    m_install_thread = std::thread([this, state, source]() {
        Result<> r = m_plugin_manager.Install(source);
        if (!r.ok())
        {
            std::lock_guard lock(state->events_mutex);
            state->pending_events.push_back({ plugin_install_event_t::Kind::Error, r.error_v() });
        }
        state->running = false;
    });
}

void ScreenshotTool::DrawInstallPluginsWindow()
{
    bool open = m_show_window.Has(SubWindow::InstallPlugins);
    if (!open)
        return;

    const bool is_installing = m_install_state && m_install_state->running;

    ImGui::SetNextWindowSize(ImVec2(560, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Install plugins##install_plugins_window", &open, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                           "NOTE: PLUGINS CAN HAVE MALWARE. INSTALL THEM AT YOUR OWN RISK");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Accepts a git repository URL, a local folder with a manifest and source code, "
            "or a prebuilt release archive (.zip/.tgz/.txz).");
        ImGui::Spacing();

        if (is_installing)
            ImGui::BeginDisabled();

        static const char* filters[] = { "*.zip", "*.tgz", "*.txz" };
        draw_input_text_file("Source", "##install_source", filters, 3, nullptr, m_install_source);

        const bool can_install = !is_installing && !m_install_source.empty();
        if (!can_install)
            ImGui::BeginDisabled();
        if (ImGui::Button("Install"))
        {
            m_show_window.Set(SubWindow::PluginInstallStatus);
            StartInstall(m_install_source);
        }
        if (!can_install)
            ImGui::EndDisabled();

        if (is_installing)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("Installing, see the status window for progress...");
        }

        ImGui::End();
    }

    m_show_window.Set(SubWindow::InstallPlugins, open);
}

void ScreenshotTool::DrawEventIcon(plugin_install_event_t::Kind kind)
{
    struct icon_t
    {
        const char* glyph;
        rgba_t      color;
    };
    static constexpr icon_t table[] = {
        { "o", rgba_t(0x808080FF) },  // Status (in progress)
        { "v", rgba_t(0x2ecc71FF) },  // Success
        { "!", rgba_t(0xf1c40fFF) },  // Warning
        { "x", rgba_t(0xe74c3cFF) },  // Error
        { "i", rgba_t(0x3498dbFF) },  // Info
    };
    const icon_t& icon = table[idx(kind)];
    ImGui::TextColored(icon.color.to_imvec4(), "%s", icon.glyph);
}

void ScreenshotTool::DrawPluginInstallStatus()
{
    bool open = m_show_window.Has(SubWindow::PluginInstallStatus);
    if (!open || !m_install_state)
        return;

    // Drain the worker thread's queue into the UI-owned tree. This is the
    // only place m_install_events gets mutated.
    {
        std::lock_guard lock(m_install_state->events_mutex);
        while (!m_install_state->pending_events.empty())
        {
            plugin_install_event_t ev = std::move(m_install_state->pending_events.front());
            m_install_state->pending_events.pop_front();

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
                m_install_events.push_back({ ev.kind, ev.text, {}, ev.kind == plugin_install_event_t::Kind::Status });
            }
        }
    }

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Plugin install##plugin_install_status_window", &open, ImGuiWindowFlags_NoSavedSettings))
    {
        for (size_t i = 0; i < m_install_events.size(); i++)
        {
            const install_node_t& node = m_install_events[i];
            ImGui::PushID(static_cast<int>(i));

            DrawEventIcon(node.kind);
            ImGui::SameLine();
            if (ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_None, "%s", node.text.c_str()))
            {
                for (const std::string& detail : node.details)
                    ImGui::TextWrapped("%s", detail.c_str());
                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (m_install_state->running)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Working...");
        }

        // Pending confirmation from the worker thread, rendered as a modal
        // inside this same window's frame.
        bool has_confirm;
        {
            std::lock_guard lock(m_install_state->confirm_mutex);
            has_confirm = m_install_state->confirm_pending && !m_install_state->confirm_answered;
        }
        if (has_confirm)
            ImGui::OpenPopup("Confirm##plugin_install_confirm");

        ImGui::SetNextWindowSize(ImVec2(550, 220));
        if (ImGui::BeginPopupModal("Confirm##plugin_install_confirm", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            std::lock_guard lock(m_install_state->confirm_mutex);
            ImGui::TextWrapped("%s", m_install_state->confirm_prompt.c_str());
            if (ImGui::Button("Yes"))
            {
                m_install_state->confirm_answer   = true;
                m_install_state->confirm_answered = true;
                m_install_state->confirm_cv.notify_all();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("No"))
            {
                m_install_state->confirm_answer   = false;
                m_install_state->confirm_answered = true;
                m_install_state->confirm_cv.notify_all();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();
    }

    // Don't let the person close the window out from under a running
    // install: the worker thread would keep pushing events into a queue
    // nothing is draining, and a pending confirm would hang forever with no
    // popup left to answer it from.
    if (!open && m_install_state->running)
        open = true;

    m_show_window.Set(SubWindow::PluginInstallStatus, open);
}
#endif

void ScreenshotTool::DrawLogsWindow()
{
    static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> imgui_ring;
    if (!imgui_ring)
        if (auto logger = spdlog::default_logger())
            imgui_ring = std::dynamic_pointer_cast<spdlog::sinks::ringbuffer_sink_mt>(logger->sinks()[2]);

    bool open = m_show_window.Has(SubWindow::Logs);
    if (!open || !imgui_ring)
        return;

    auto level_color = [](spdlog::level::level_enum lvl) -> rgba_t {
        switch (lvl)
        {
            case spdlog::level::trace:    return rgba_t(0x888888FF);  // gray
            case spdlog::level::debug:    return rgba_t(0x9999FFFF);  // periwinkle
            case spdlog::level::info:     return rgba_t(0x00AEFFFF);  // blueish
            case spdlog::level::warn:     return rgba_t(0xFFCC33FF);  // amber
            case spdlog::level::err:      return rgba_t(0xFF4D4DFF);  // red
            case spdlog::level::critical: return rgba_t(0xFF0000FF);  // pure red

            default: return rgba_t(0xFFFFFFFF);  // white
        }
    };

    auto level_tag = [](spdlog::level::level_enum lvl) -> const char* {
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

    ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Logs##logs_window", &open, ImGuiWindowFlags_NoSavedSettings))
    {
        // --- Toolbar ---
        if (ImGui::Button("Clear"))
            cleared_before = spdlog::log_clock::now();

        ImGui::SameLine();
        bool copy_all = ImGui::Button("Copy");
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &autoscroll);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
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
        text_filter.Draw("Filter", 200);

        ImGui::Separator();

        // --- Build filtered view ---
        const std::vector<spdlog::details::log_msg_buffer>& all = imgui_ring->last_raw();
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
                ImGui::TextColored(level_color(msg.level).to_imvec4(), "[%s]", level_tag(msg.level));
                ImGui::SameLine();
                ImGui::TextUnformatted(msg.payload.data(), msg.payload.data() + msg.payload.size());

                if (ImGui::BeginPopupContextItem(fmt::format("ctx##{}", i).c_str()))
                {
                    if (ImGui::MenuItem("Copy line"))
                        MUST_OK(g_clipboard.CopyText(msg.payload.data()),
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
        ImGui::End();
    }

    m_show_window.Set(SubWindow::Logs, open);
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

    // Reset success banner when the user targets a different model
    if (prev_model != model_to_get)
    {
        has_downloaded = false;
        prev_model     = model_to_get;
    }

    const bool is_downloading = m_ocr_download && m_ocr_download->running.load();

    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);  // 0 = auto height
    if (ImGui::Begin("Download OCR Model##ocr_download_window", &open, ImGuiWindowFlags_NoSavedSettings))
    {
        // Lock all inputs while a download is in flight
        if (is_downloading)
            ImGui::BeginDisabled();

        ImGui::SeparatorText("Source");
        ImGui::TextDisabled("GitHub repository containing Tesseract '.traineddata' models");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ocr_download_repo", &m_inputs.ocr_download_repo))
            ClearError(ectx, OcrDownloadError::FailedToDownload);

        ImGui::Spacing();

        ImGui::SeparatorText("Model");
        ImGui::TextDisabled("Language code (e.g eng, fra, deu, chi_sim)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ocr_model_to_download", &model_to_get))
            ClearError(ectx, OcrDownloadError::FailedToDownload);

        ImGui::Spacing();

        ImGui::SeparatorText("Destination");
        ImGui::SetNextItemWidth(-1);
        draw_input_text_folder(
            "",
            "##ocr_download_path",
            [&] {
                if (!fs::exists(m_inputs.ocr_model_downloaded_path))
                    SetError(ectx, OcrDownloadError::InvalidPath, "No such directory or path");
                else if (!fs::is_directory(m_inputs.ocr_model_downloaded_path))
                    SetError(ectx, OcrDownloadError::InvalidPath, "Not a directory");
                else
                    ClearError(ectx, OcrDownloadError::InvalidPath);
            },
            m_inputs.ocr_model_downloaded_path);
        ShowIfError(ectx, OcrDownloadError::InvalidPath);

        if (is_downloading)
            ImGui::EndDisabled();

        // URL preview when both field are filled
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
        const bool can_download = !m_ocr_download && fields_filled &&
                                  !ectx.HasAny(OcrDownloadError::InvalidPath, OcrDownloadError::FailedToDownload);

        if (!can_download)
            ImGui::BeginDisabled();

        // Only show button when not already downloading
        if (ImGui::Button("Download", ImVec2(-1, 0)))
        {
            const std::vector<std::string> cmd{
                "curl",
                "-fL",
                fmt::format("https://raw.githubusercontent.com/{}/main/{}.traineddata",
                            m_inputs.ocr_download_repo,
                            model_to_get),
                "-o",
                fmt::format("{}/{}.traineddata", m_inputs.ocr_model_downloaded_path, model_to_get)
            };

            m_ocr_download = std::make_shared<ocr_download_t>();

            std::thread([dl = m_ocr_download, cmd = std::move(cmd)]() mutable {
                TinyProcessLib::Process proc(
                    cmd,
                    "",
                    [](const char*, size_t) { /* stdout: unused */ },
                    [&dl](const char* buf, size_t n) {
                        std::lock_guard g(dl->err_mutex);

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
                                if (sscanf(line.data(), " %d", &pct) == 1 && pct >= 0 && pct <= 100)
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
            if (is_downloading)
                ImGui::Text("Download already in progress");
            else if (!fields_filled)
                ImGui::Text("Fill all fields first");
            else if (HasError(ectx, OcrDownloadError::InvalidPath))
                ImGui::Text("Fix the destination path error first");
            else
                ImGui::Text("A download error occurred — see error below");
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
                    // Size unknown yet, so let's show a marquee-style bar
                    const float t = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.8f, 1.0f);
                    ImGui::ProgressBar(-1.f * t, ImVec2(-1.f, 0.f), "Downloading...");
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
                        std::lock_guard g(m_ocr_download->err_mutex);
                        err = std::move(m_ocr_download->err);
                    }
                    SetError(ectx,
                             OcrDownloadError::FailedToDownload,
                             fmt::format("Failed to download '{}' to '{}':\n{}",
                                         model_to_get,
                                         m_inputs.ocr_model_downloaded_path,
                                         err));
                }
                else
                {
                    ClearError(ectx, OcrDownloadError::FailedToDownload);
                    has_downloaded = true;
                }
                m_ocr_download.reset();
            }
        }

        ImGui::Spacing();
        ShowIfError(ectx, OcrDownloadError::FailedToDownload);
        if (has_downloaded && !HasError(ectx, OcrDownloadError::FailedToDownload))
            ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.2f, 1.f), "Downloaded successfully!");

        ImGui::End();
    }

    m_show_window.Set(SubWindow::OcrDownload, open);
}

void ScreenshotTool::DrawAnnotations()
{
    static_assert(sizeof(point_t) == sizeof(ImVec2) && alignof(point_t) == alignof(ImVec2),
                  "point_t and ImVec2 layout mismatch");

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const float dpi       = ImGui::GetIO().DisplayFramebufferScale.x;

    auto draw_line = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        draw_list->AddLine(p1, p2, ann.color.to_abgr(), t);
    };

    auto draw_text = [&](const annotation_t& ann, const ImVec2& p1) {
        const float font_size = ann.thickness > 8.0f ? ann.thickness : ImGui::GetFontSize();
        ImFont*     font      = CacheAndGetFont(m_inputs.resolved_ann_font_path, font_size);
        draw_list->AddText(font, font_size, p1, ann.color.to_abgr(), ann.text.c_str());
    };

    auto draw_rectangle_or_filled =
        [&](const bool filled, const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
            ImVec2 min(std::min(p1.x, p2.x), std::min(p1.y, p2.y));
            ImVec2 max(std::max(p1.x, p2.x), std::max(p1.y, p2.y));
            filled ? draw_list->AddRectFilled(min, max, ann.color.to_abgr(), 0.0f, ImDrawFlags_None)
                   : draw_list->AddRect(min, max, ann.color.to_abgr(), 0.0f, t, ImDrawFlags_None);
        };

    auto draw_circle_or_filled =
        [&](const bool filled, const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
            float dx     = p2.x - p1.x;
            float dy     = p2.y - p1.y;
            float radius = std::sqrt(dx * dx + dy * dy);
            filled ? draw_list->AddCircleFilled(p1, radius, ann.color.to_abgr(), 0)
                   : draw_list->AddCircle(p1, radius, ann.color.to_abgr(), 0, t);
        };

    auto draw_counter_bubble = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        // Draw the circle outline
        draw_circle_or_filled(false, ann, p1, p2, t);

        const std::string& label = fmt::to_string(ann.count);

        // Pick a font size that fits comfortably inside the circle
        const float dx     = p2.x - p1.x;
        const float dy     = p2.y - p1.y;
        const float radius = std::sqrt(dx * dx + dy * dy);
        // Use ~70 % of the diameter so the number has breathing room
        const float font_size = std::max(8.0f, radius * 1.4f);
        ImFont*     font      = CacheAndGetFont(m_inputs.resolved_ann_font_path, font_size);

        // Measure the rendered label so we can center it
        const ImVec2 text_size =
            font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str()) : ImGui::CalcTextSize(label.c_str());

        const ImVec2 text_pos(p1.x - text_size.x * 0.5f, p1.y - text_size.y * 0.5f);
        draw_list->AddText(font, font_size, text_pos, ann.color.to_abgr(), label.c_str());
    };

    auto draw_pencil = [&](const annotation_t& ann, const float t) {
        if (ann.points.size() > 1)
        {
            draw_list->AddPolyline(reinterpret_cast<const ImVec2*>(ann.points.data()),
                                   int(ann.points.size()),
                                   ann.color.to_abgr(),
                                   t,
                                   ImDrawFlags_None);
        }
    };

    auto draw_arrow = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        ImVec2 v(p2.x - p1.x, p2.y - p1.y);
        float  len = std::sqrt(v.x * v.x + v.y * v.y);
        if (len < 1.0f)
            return;

        ImVec2 dir(v.x / len, v.y / len);
        ImVec2 perp(-dir.y, dir.x);  // unit perpendicular

        float head_len = 6.0f * t;
        float head_w   = 4.0f * t;

        head_len = std::min(head_len, len * 0.6f);

        ImVec2 base(p2.x - dir.x * head_len, p2.y - dir.y * head_len);
        ImVec2 left(base.x + perp.x * (head_w * 0.5f), base.y + perp.y * (head_w * 0.5f));
        ImVec2 right(base.x - perp.x * (head_w * 0.5f), base.y - perp.y * (head_w * 0.5f));

        // shaft
        draw_list->AddLine(p1, base, ann.color.to_abgr(), t);

        // head
        draw_list->AddTriangleFilled(p2, left, right, ann.color.to_abgr());
    };

    auto draw_annotation = [&](const annotation_t& ann) {
        const ImVec2 p1(ann.start.x, ann.start.y);
        const ImVec2 p2(ann.end.x, ann.end.y);
        const float  t = ann.thickness * dpi;

        switch (ann.type)
        {
            case ToolType::Line:            draw_line(ann, p1, p2, t); break;
            case ToolType::Arrow:           draw_arrow(ann, p1, p2, t); break;
            case ToolType::Rectangle:       draw_rectangle_or_filled(false, ann, p1, p2, t); break;
            case ToolType::RectangleFilled: draw_rectangle_or_filled(true, ann, p1, p2, t); break;
            case ToolType::Circle:          draw_circle_or_filled(false, ann, p1, p2, t); break;
            case ToolType::CircleFilled:    draw_circle_or_filled(true, ann, p1, p2, t); break;
            case ToolType::CounterBubble:   draw_counter_bubble(ann, p1, p2, t); break;
            case ToolType::Text:            draw_text(ann, p1); break;
            case ToolType::Pencil:          draw_pencil(ann, t); break;

            case ToolType::kNone:
            case ToolType::ToggleTextTools:
            case ToolType::CopyImage:
            case ToolType::SaveImage:
            case ToolType::Logo:
            case ToolType::COUNT:           break;
        }
    };

    for (const annotation_t& ann : m_annotations)
        draw_annotation(ann);

    // Render current annotation being drawn on top of committed ones
    if (m_current_actions.Has(CurrentAction::IsDrawing))
        draw_annotation(m_current_annotation);
}

void ScreenshotTool::DrawOutputMenuSelection()
{
#if OSHOT_LINUX
    if (!m_show_window.Has(SubWindow::OutputMenuSelection))
        return;

    static int output_sel = 0;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 10));

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 0), ImVec2(520, 480));
    ImGui::Begin("Choose an output to capture##select_output_crop",
                 nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    std::deque<region_t> layout;

    for (size_t i = 0; i < m_wayland_monitors.size(); ++i)
    {
        const monitor_t& m = m_wayland_monitors[i];
        layout.push_back(m.geo);

        ImGui::PushID(int(i));
        ImGui::RadioButton(
            fmt::format("{} ({}x{})", m.name[0] ? m.name : "Unknown", m.geo.w, m.geo.h).c_str(), &output_sel, int(i));
        ImGui::PopID();
    }

    output_sel = layout.empty() ? 0 : std::clamp(output_sel, 0, static_cast<int>(layout.size()) - 1);

    ImGui::Separator();

    const float button_width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(layout.empty());
    if (ImGui::Button("Crop", ImVec2(button_width, 0)))
    {
        const monitor_t& target = m_wayland_monitors[output_sel];
        m_show_window.Clear(SubWindow::OutputMenuSelection);
        MUST_OK(g_ss_tool.CropToOutput(layout, target), error("Crop to focused monitor failed: {}", _r.error_v()));
    }
    ImGui::EndDisabled();

    ImGui::End();
    ImGui::PopStyleVar(2);
#endif
}

void ScreenshotTool::Cancel()
{
    m_state = ToolState::Idle;

    auto delete_texture = [](ImTextureRef& tex) {
#if OSHOT_MACOS
        tex = ImTextureRef{};
#else
        if (tex._TexID)
        {
            GLuint texture = (GLuint)(intptr_t)tex._TexID;
            glDeleteTextures(1, &texture);
            tex = ImTextureRef{};
        }
#endif
    };

    delete_texture(m_texture_id);
    for (auto& tex : m_tool_textures)
        delete_texture(tex);

    // (just clears our references, not the actual ImGui fonts)
    m_font_cache.clear();

    if (m_on_cancel)
        m_on_cancel();
}

bool ScreenshotTool::OpenImage(const std::string& path)
{
    const Result<capture_result_t>& cap = load_image_rgba(path);
    MUST_OK(cap, {
        error("Failed to load image: {}", cap.error_v());
        return false;
    });

    m_screenshot = std::move(cap.get());
    fit_to_screen(m_screenshot);

#if OSHOT_MACOS
    // Tell backend to recreate Metal texture
    if (m_on_image_reload)
        m_on_image_reload(m_screenshot);
#else

    // Recreate texture (CreateTexture() already deletes the old ones)
    const Result<ImTextureRef>& r = CreateTexture(reinterpret_cast<void*>(static_cast<size_t>(m_texture_id._TexID)),
                                                  m_screenshot.view(),
                                                  m_screenshot.w,
                                                  m_screenshot.h);
    MUST_OK(r, {
        error("Failed to create openGL texture: {}", r.error_v());
        return false;
    });

    m_texture_id = r.get();
#endif

    // Reset interactions.
    // some are already reset from previous calls
    m_state           = ToolState::Selecting;
    m_current_tool    = ToolType::kNone;
    m_handle_hover    = HandleHovered::kNone;
    m_dragging_handle = HandleHovered::kNone;
    m_input_owner     = InputOwner::kNone;

    m_selection            = {};
    m_drag_start_selection = {};
    m_drag_start_mouse     = {};
    m_image_origin         = {};
    m_image_end            = {};

    return true;
}

capture_result_t ScreenshotTool::GetFinalImage(bool is_text_tools)
{
    UpdateWindowBg();

    const region_t& region = GetActiveRegion();

    capture_result_t result;
    result.w = region.w;
    result.h = region.h;
    result.data.resize(size_t(region.w) * region.h * 4);

    std::span<const uint8_t> src(m_screenshot.view());
    std::span<uint8_t>       dst(result.data);

    const int src_width = m_screenshot.w;
    const int dst_width = region.w;

    // Calculate bounds
    const int start_y = std::max(0, -region.y);
    const int end_y   = std::min(region.h, m_screenshot.h - region.y);
    const int start_x = std::max(0, -region.x);
    const int end_x   = std::min(region.w, m_screenshot.w - region.x);

    int width = end_x - start_x;

    // Copy only the valid region
    const size_t bytes_to_copy = (width > 0) ? size_t(width) * 4 : 0;

    if (bytes_to_copy > 0)
    {
        for (int y = start_y; y < end_y; ++y)
        {
            const int    src_y         = region.y + y;
            const size_t src_row_start = (size_t(src_y) * src_width + (region.x + start_x)) * 4;
            const size_t dst_row_start = (size_t(y) * dst_width + start_x) * 4;

            std::memcpy(dst.data() + dst_row_start, src.data() + src_row_start, bytes_to_copy);
        }
    }

    if (is_text_tools && !g_config->File.render_anns)
        return result;

    // Render annotations to the final image
    const float offset_x = m_selection.get_x();
    const float offset_y = m_selection.get_y();

    auto set_pixel = [&](int x, int y, rgba_t color) {
        if (x < 0 || x >= result.w || y < 0 || y >= result.h)
            return;

        size_t   idx = (size_t(y) * result.w + x) * 4;
        uint8_t* p   = &result.data[idx];

        if (color.a == 0xFF)
        {
            store_rgba(p, color);
            return;
        }

        rgba_t dst = load_rgba(p);
        store_rgba(p, blend(color, dst));
    };

    auto draw_line = [&](int x0, int y0, int x1, int y1, rgba_t color, float thickness) {
        // Bresenham's line algorithm with thickness
        int dx     = std::abs(x1 - x0);
        int dy     = std::abs(y1 - y0);
        int sx     = x0 < x1 ? 1 : -1;
        int sy     = y0 < y1 ? 1 : -1;
        int err    = dx - dy;
        int radius = int(thickness / 2.0f);

        while (true)
        {
            // Draw thick point
            for (int oy = -radius; oy <= radius; ++oy)
                for (int ox = -radius; ox <= radius; ++ox)
                    if (ox * ox + oy * oy <= radius * radius)
                        set_pixel(x0 + ox, y0 + oy, color);

            if (x0 == x1 && y0 == y1)
                break;

            int e2 = 2 * err;
            if (e2 > -dy)
            {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx)
            {
                err += dx;
                y0 += sy;
            }
        }
    };

    for (const annotation_t& ann : m_annotations)
    {
        int x1 = int(ann.start.x - offset_x);
        int y1 = int(ann.start.y - offset_y);
        int x2 = int(ann.end.x - offset_x);
        int y2 = int(ann.end.y - offset_y);
        int cx = x1;
        int cy = y1;

        int radius = int(std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)));

        // Previous switch case has been moved to an if/elseif long branch for keeping away duplicated code because of the counter bubble being
        // a combination of a circle and text inside it.
        // Please stfu, the compiler will make ts into a switch case automatically. Stop fighting the machine
        if (ann.type == ToolType::Text || ann.type == ToolType::CounterBubble)
        {
            const std::string& label = ann.type == ToolType::CounterBubble ? fmt::to_string(ann.count) : ann.text;

            if (label.empty())
                continue;

            float font_size;
            if (ann.type == ToolType::CounterBubble)
                font_size = std::max(8.0f, float(radius) * 1.4f);  // ~70 % of diameter so the digit has breathing room
            else
                font_size = ann.thickness > 8.0f ? ann.thickness : ImGui::GetFontSize();

            ImFont* font = CacheAndGetFont(m_inputs.resolved_ann_font_path, font_size);
            if (!font || !font->OwnerAtlas)
                continue;

            ImFontBaked* baked = font->GetFontBaked(font_size);
            if (!baked)
                continue;

            ImTextureData* tex = font->OwnerAtlas->TexData;
            // from obsolete GetTexDataAsFormat()
            if (!font->OwnerAtlas->TexIsBuilt || tex == NULL || tex->Pixels == NULL)
            {
                ImFontAtlasBuildMain(font->OwnerAtlas);
                tex = font->OwnerAtlas->TexData;
            }
            unsigned char* pixels  = tex->Pixels;
            int            atlas_w = tex->Width, atlas_h = tex->Height;
            if (!pixels || atlas_w == 0 || atlas_h == 0)
                continue;

            const char* p   = label.c_str();
            const char* end = p + label.size();

            float cursor_x;
            float cursor_y;
            if (ann.type == ToolType::CounterBubble)
            {
                // Measure total advance to compute the centered origin
                float total_w = 0.0f;
                float total_h = font_size;  // approximate; refined below
                {
                    const char* pp  = label.c_str();
                    const char* end = p + label.size();
                    while (pp < end)
                    {
                        unsigned int cp = 0;
                        pp += ImTextCharFromUtf8(&cp, pp, end);
                        if (cp == 0)
                            break;
                        const ImFontGlyph* g = baked->FindGlyph(ImWchar(cp));
                        if (g)
                        {
                            total_w += g->AdvanceX;
                            total_h = std::max(total_h, g->Y1 - g->Y0);
                        }
                    }
                }

                cursor_x = float(cx) - total_w * 0.5f;
                cursor_y = float(cy) - total_h * 0.5f;
            }
            else
            {
                cursor_x = x1;
                cursor_y = y2;
            }

            while (p < end)
            {
                unsigned int codepoint = 0;
                p += ImTextCharFromUtf8(&codepoint, p, end);
                if (codepoint == 0)
                    break;

                const ImFontGlyph* glyph = baked->FindGlyph(ImWchar(codepoint));
                if (!glyph)
                    continue;

                const int dst_x0 = int(cursor_x + glyph->X0);
                const int dst_y0 = int(cursor_y + glyph->Y0);
                const int dst_x1 = int(cursor_x + glyph->X1);
                const int dst_y1 = int(cursor_y + glyph->Y1);

                const int src_x0 = int(glyph->U0 * atlas_w);
                const int src_y0 = int(glyph->V0 * atlas_h);
                const int src_x1 = int(glyph->U1 * atlas_w);
                const int src_y1 = int(glyph->V1 * atlas_h);

                const int dst_gw = dst_x1 - dst_x0;
                const int dst_gh = dst_y1 - dst_y0;
                const int src_gw = src_x1 - src_x0;
                const int src_gh = src_y1 - src_y0;

                if (dst_gw <= 0 || dst_gh <= 0 || src_gw <= 0 || src_gh <= 0)
                {
                    cursor_x += glyph->AdvanceX;
                    continue;
                }

                const uint32_t* font_pixels = reinterpret_cast<const uint32_t*>(pixels);
                rgba_t          col         = ann.color;
                for (int dy = 0; dy < dst_gh; ++dy)
                {
                    const int src_ay = src_y0 + dy * src_gh / dst_gh;
                    if (src_ay < 0 || src_ay >= atlas_h)
                        continue;

                    for (int dx = 0; dx < dst_gw; ++dx)
                    {
                        const int src_ax = src_x0 + dx * src_gw / dst_gw;
                        if (src_ax < 0 || src_ax >= atlas_w)
                            continue;

                        const uint32_t atlas_px    = font_pixels[src_ay * atlas_w + src_ax];
                        const uint8_t  glyph_alpha = uint8_t((atlas_px >> 24) & 0xFF);
                        if (glyph_alpha == 0)
                            continue;

                        uint8_t src_a = col.a * glyph_alpha / 255u;
                        rgba_t  pixel(col.r, col.g, col.b, src_a);
                        set_pixel(dst_x0 + dx, dst_y0 + dy, pixel);
                    }
                }

                cursor_x += glyph->AdvanceX;
            }
        }

        if (ann.type == ToolType::CounterBubble || ann.type == ToolType::Circle)
        {
            // Midpoint circle algorithm
            int x       = radius;
            int y       = 0;
            int err     = 0;
            int thick_r = int(ann.thickness / 2.0f);

            while (x >= y)
            {
                for (int oy = -thick_r; oy <= thick_r; ++oy)
                    for (int ox = -thick_r; ox <= thick_r; ++ox)
                        if (ox * ox + oy * oy <= thick_r * thick_r)
                        {
                            set_pixel(cx + x + ox, cy + y + oy, ann.color);
                            set_pixel(cx + y + ox, cy + x + oy, ann.color);
                            set_pixel(cx - y + ox, cy + x + oy, ann.color);
                            set_pixel(cx - x + ox, cy + y + oy, ann.color);
                            set_pixel(cx - x + ox, cy - y + oy, ann.color);
                            set_pixel(cx - y + ox, cy - x + oy, ann.color);
                            set_pixel(cx + y + ox, cy - x + oy, ann.color);
                            set_pixel(cx + x + ox, cy - y + oy, ann.color);
                        }

                y += 1;
                err += 1 + 2 * y;
                if (2 * (err - x) + 1 > 0)
                {
                    x -= 1;
                    err += 1 - 2 * x;
                }
            }
        }

        else if (ann.type == ToolType::Line)
        {
            draw_line(x1, y1, x2, y2, ann.color, ann.thickness);
        }

        else if (ann.type == ToolType::Arrow)
        {
            draw_line(x1, y1, x2, y2, ann.color, ann.thickness);
            // Draw arrowhead
            float dx  = x2 - x1;
            float dy  = y2 - y1;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.1f)
            {
                dx /= len;
                dy /= len;
                float arrow_size = 15.0f + ann.thickness;
                int   ax1        = int(x2 - arrow_size * dx + arrow_size * 0.5f * dy);
                int   ay1        = int(y2 - arrow_size * dy - arrow_size * 0.5f * dx);
                int   ax2        = int(x2 - arrow_size * dx - arrow_size * 0.5f * dy);
                int   ay2        = int(y2 - arrow_size * dy + arrow_size * 0.5f * dx);
                draw_line(x2, y2, ax1, ay1, ann.color, ann.thickness);
                draw_line(x2, y2, ax2, ay2, ann.color, ann.thickness);
            }
        }

        else if (ann.type == ToolType::Rectangle)
        {
            draw_line(x1, y1, x2, y1, ann.color, ann.thickness);
            draw_line(x2, y1, x2, y2, ann.color, ann.thickness);
            draw_line(x2, y2, x1, y2, ann.color, ann.thickness);
            draw_line(x1, y2, x1, y1, ann.color, ann.thickness);
        }

        else if (ann.type == ToolType::RectangleFilled)
        {
            int rx1 = std::min(x1, x2);
            int rx2 = std::max(x1, x2);
            int ry1 = std::min(y1, y2);
            int ry2 = std::max(y1, y2);
            for (int fy = ry1; fy <= ry2; ++fy)
                for (int fx = rx1; fx <= rx2; ++fx)
                    set_pixel(fx, fy, ann.color);
        }

        else if (ann.type == ToolType::CircleFilled)
        {
            for (int fy = cy - radius; fy <= cy + radius; ++fy)
                for (int fx = cx - radius; fx <= cx + radius; ++fx)
                    if ((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) <= radius * radius)
                        set_pixel(fx, fy, ann.color);
        }

        else if (ann.type == ToolType::Pencil)
        {
            for (size_t i = 1; i < ann.points.size(); ++i)
            {
                int px1 = int(ann.points[i - 1].x - offset_x);
                int py1 = int(ann.points[i - 1].y - offset_y);
                int px2 = int(ann.points[i].x - offset_x);
                int py2 = int(ann.points[i].y - offset_y);
                draw_line(px1, py1, px2, py2, ann.color, ann.thickness);
            }
        }
    }

    return result;
}

region_t ScreenshotTool::GetActiveRegion() const
{
    bool has_selection = m_selection.get_width() > 0 && m_selection.get_height() > 0;

    if (!has_selection)
    {
        // Full screenshot (visible area)
        return region_t{ 0, 0, m_screenshot.w, m_screenshot.h };
    }

    // Convert from screen space -> image space
    float x = m_selection.get_x() - m_image_origin.x;
    float y = m_selection.get_y() - m_image_origin.y;
    float w = m_selection.get_width();
    float h = m_selection.get_height();

    // Clamp to image bounds (important if user drags outside)
    x = std::clamp(x, 0.0f, float(m_screenshot.w));
    y = std::clamp(y, 0.0f, float(m_screenshot.h));
    w = std::clamp(w, 0.0f, float(m_screenshot.w - x));
    h = std::clamp(h, 0.0f, float(m_screenshot.h - y));

    return region_t{ int(x), int(y), int(w), int(h) };
}

void ScreenshotTool::UpdateWindowBg()
{
    // Calculate where the screenshot will be drawn (centered)
    // clang-format off
    auto* vp = ImGui::GetMainViewport();
    ImVec2 image_size(
        float(m_screenshot.w),
        float(m_screenshot.h)
    );

    m_image_origin = ImVec2(
        vp->Pos.x + (vp->Size.x - image_size.x) * 0.5f,
        vp->Pos.y + (vp->Size.y - image_size.y) * 0.5f
    );

    m_image_end = ImVec2(
        m_image_origin.x + image_size.x,
        m_image_origin.y + image_size.y
    );
    // clang-format on
}

Result<> ScreenshotTool::CropToOutput(const std::deque<region_t>& layout, const monitor_t& target)
{
    Result<capture_result_t> cropped = crop_to_monitor(m_screenshot, layout, target.geo);
    TRY_MSG(cropped, "Failed to crop capture to focused monitor: {}");

    m_screenshot = std::move(cropped.get());

    spdlog::debug("target.transform = {}", target.transform);
    // WL_OUTPUT_TRANSFORM_90/180/270
    if (target.transform >= 1 && target.transform <= 3)
        m_screenshot = rotate_rgba(m_screenshot, 4 - target.transform);  // swap 1 (90) and 3 (270)

    const Result<ImTextureRef>& r = CreateTexture(reinterpret_cast<void*>(static_cast<size_t>(m_texture_id._TexID)),
                                                  m_screenshot.view(),
                                                  m_screenshot.w,
                                                  m_screenshot.h);
    TRY_MSG(r, "Failed to recreate texture after crop: {}");
    m_texture_id = r.get();

    UpdateWindowBg();  // re-center image_origin/image_end for the new, smaller size

    return Ok();
}

ImFont* ScreenshotTool::CacheAndGetFont(const std::string& font_path, const float font_size)
{
    if (font_path.empty())
        return ImGui::GetDefaultFont();

    const float safe_size = std::max(font_size, 16.0f);
    std::pair   key(font_path, safe_size);
    auto        it = m_font_cache.find(key);
    if (it != m_font_cache.end())
        return it->second.font;

    ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
        font_path.c_str(), font_size, nullptr, ImGui::GetIO().Fonts->GetGlyphRangesDefault());

    m_font_cache[key] = { font_path, font, true };
    if (font)
        ImFontAtlasBuildMain(ImGui::GetIO().Fonts);

    return font;
}

void ScreenshotTool::CreateCopyTextButton(const std::string& text_copy)
{
    ErrorContext<GeneralError>& ectx = m_general_errors;

    static bool armed = false;
    if (create_timed_button("Copy Text", "Copied!", armed))
    {
        const Result<>& res = g_clipboard.CopyText(text_copy);
        if (res.ok())
        {
            ClearError(ectx, GeneralError::FailedToCopyText);
            armed = true;
        }
        else
        {
            SetError(ectx, GeneralError::FailedToCopyText, res.error_v());
        }
    }

    if (HasError(ectx, GeneralError::FailedToCopyText))
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                           "Failed to copy text: %s",
                           GetError(ectx, GeneralError::FailedToCopyText).c_str());
    }
}

void ScreenshotTool::SyncRuntimeFromConfig()
{
    m_inputs.ocr_path  = g_config->File.ocr_path;
    m_inputs.ocr_model = g_config->File.ocr_model;
    RefreshOcrModels();

    // Sync annotation font
    m_inputs.ann_font               = g_config->File.fonts.empty() ? "" : g_config->File.fonts[0];
    m_inputs.resolved_ann_font_path = get_font_path(m_inputs.ann_font).string();

    extern_glfwSwapInterval(int(g_config->File.enable_vsync));
}

void ScreenshotTool::RefreshOcrModels()
{
    ErrorContext<OcrError>& ectx = m_ocr_errors;

    if (m_inputs.ocr_path == m_last_scanned_ocr_path)
    {
        ClearError(ectx, OcrError::NeedToScanDir);
        return;
    }

    get_filtered_filenames(
        m_inputs.ocr_path,
        m_ocr_models_list,
        [](const fs::path& entry) { return entry.extension().string() == ".traineddata"; },
        [](const fs::path& entry) { return entry.stem().string(); });
    m_last_scanned_ocr_path = m_inputs.ocr_path;
    ClearError(ectx, OcrError::NeedToScanDir);

    if (m_ocr_models_list.empty())
    {
        SetError(ectx, OcrError::InvalidPath, "Doesn't exist or is empty");
    }
    else
    {
        ClearError(ectx, OcrError::InvalidPath);
        const auto& it = std::find(m_ocr_models_list.begin(), m_ocr_models_list.end(), m_inputs.ocr_model);
        if (it == m_ocr_models_list.end())
            SetError(ectx, OcrError::InvalidModel);
        else
            ClearError(ectx, OcrError::InvalidModel);
    }
}

void ScreenshotTool::StyleDefaultColor()
{
    // clang-format off
    static const std::unordered_map<ImGuiCol, rgba_t> theme_colors = {
        { ImGuiCol_Text,          0xDCDDE1FF_rgba },
        { ImGuiCol_TextDisabled,  0x666870FF_rgba },
        { ImGuiCol_WindowBg,      0x0D1015FF_rgba },
        { ImGuiCol_ChildBg,       0x0D1015FF_rgba },
        { ImGuiCol_PopupBg,       0x10131AFF_rgba },

        { ImGuiCol_Border,        0x242933FF_rgba },

        { ImGuiCol_TitleBg,       0x090B0FFF_rgba },
        { ImGuiCol_TitleBgActive, 0x5274F0FF_rgba },

        { ImGuiCol_FrameBg,       0x151920FF_rgba },
        { ImGuiCol_FrameBgHovered,0x202630FF_rgba },
        { ImGuiCol_FrameBgActive, 0x29313EFF_rgba },

        { ImGuiCol_Button,        0x294CC7FF_rgba },
        { ImGuiCol_ButtonHovered, 0x385DE0FF_rgba },
        { ImGuiCol_ButtonActive,  0x203CA6FF_rgba },

        { ImGuiCol_Header,        0x1A1F28FF_rgba },
        { ImGuiCol_HeaderHovered, 0x252C38FF_rgba },
        { ImGuiCol_HeaderActive,  0x303946FF_rgba },

        { ImGuiCol_Tab,           0x10141BFF_rgba },
        { ImGuiCol_TabHovered,    0x252E42FF_rgba },
        { ImGuiCol_TabSelected,   0x294CC7FF_rgba },

        { ImGuiCol_SliderGrab,    0x294CC7FF_rgba },
        { ImGuiCol_SliderGrabActive, 0x5274F0FF_rgba },

        { ImGuiCol_ScrollbarBg,   0x07090CFF_rgba },
        { ImGuiCol_ScrollbarGrab, 0x2A303AFF_rgba },

        { ImGuiCol_CheckMark,     0x5274F0FF_rgba },
        { ImGuiCol_MenuBarBg,     0x090B0FFF_rgba },
        { ImGuiCol_PlotHistogram, 0x5274F0FF_rgba },
    };
    // clang-format on

    ImGui::StyleColorsDark();

    auto& style = ImGui::GetStyle();

    style.WindowRounding = 4.0f;
    style.FrameRounding  = 4.0f;
    style.GrabRounding   = 1.5f;
    style.TabRounding    = 2.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize  = 0.0f;

    for (const auto& [color, rgba] : theme_colors)
        style.Colors[color] = rgba.to_imvec4();
}

Result<ImTextureRef> ScreenshotTool::CreateTexture(void* tex, std::span<const uint8_t> data, int w, int h)
{
#if OSHOT_MACOS
    // Metal backend handles textures separately
    return Ok(ImTextureRef{});
#else
    // Existing OpenGL implementation
    if (tex)
    {
        GLuint old_texture = (GLuint)(intptr_t)tex;
        glDeleteTextures(1, &old_texture);
    }

    GLuint texture;
    glGenTextures(1, &texture);

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());

    ImTextureRef ref;
    ref._TexID = static_cast<ImTextureID>(texture);
    return Ok(ref);
#endif
}
