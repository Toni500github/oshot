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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "cache.hpp"
#include "config.hpp"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3_loader.h"
#include "imgui/imgui_internal.h"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/spdlog.h"
#include "text_extraction.hpp"
#include "tool_icons.h"
#include "util.hpp"
#ifndef DISABLE_PLUGINS
#  include "plugin.hpp"
#endif
#include "helpers.hpp"

using namespace std::chrono_literals;

// ------------------------------
// Lifecycle
// ------------------------------

Result<> ScreenshotTool::Start()
{
    Result<capture_result_t> result{ Err() };
    m_session = get_session_type();

    // g_ss_tool outlives a capture, since the tray runs every one of them with it
    ResetSelection();
    m_anno_sel.handle_hover     = HandleHovered::kNone;
    m_anno_sel.dragging_handle  = HandleHovered::kNone;
    m_current_annotation        = {};
    m_current_annotation_resize = nullptr;
    m_current_actions           = {};
    m_pending_scan              = ScanType::kNone;
    m_completed_op              = SavingOp::kNone;
    m_final_image_requested     = false;
    m_popup_open                = false;
    m_show_window.Clear(SubWindow::OutputMenuSelection);
    m_annotations.clear();

    if (!g_config->Runtime.source_file.empty())
    {
        result = load_image_rgba(g_config->Runtime.source_file);
        TRY_MSG(result, "Failed to load image: {}");
    }
    else
    {
        if (g_config->File.delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(g_config->File.delay));

        result = capture_full_screen(m_session);
        TRY_MSG(result, "Failed to capture screenshot: {}");

#if OSHOT_FREEDESKTOP
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
    KeepDrawing();
    m_uncropped = {};
    if (m_show_window.Has(SubWindow::OutputMenuSelection))
        m_uncropped = m_screenshot;
    fit_to_screen(m_screenshot);

    // Only the screenshot, nothing for the text tools window
    if (g_config->Runtime.instant_copy_save != SavingOp::kNone)
    {
        m_state = ToolState::Selecting;
#if OSHOT_METAL
        m_texture_id = ImTextureRef{};  // will be set by backend
#else
        const Result<ImTextureRef>& res = CreateTexture(0, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
        TRY_MSG(res, "Failed to create openGL texture: {}");

        m_texture_id = res.get();
#endif
        return Ok();
    }

#ifndef DISABLE_PLUGINS
    static std::once_flag plugins_loaded;
    std::call_once(plugins_loaded, [&] { load_plugins(m_plugin_manager.GetStateManager()); });
#endif

    m_inputs = { .ocr_path                  = g_config->File.ocr_path,
                 .ocr_model                 = g_config->File.ocr_model,
                 .ocr_download_repo         = g_config->File.ocr_get_repo,
                 .ocr_model_downloaded_path = g_config->GetConfigDirPath() + DIR_SEP_STR "models",
                 .ocr_results               = {},
                 .barcode_text              = "",
                 .barcode_scan_result       = {},
                 .ann_font                  = "",
                 .resolved_ann_font_path    = "",
                 .ocr_original              = "",
                 .barcode_original          = "" };

    m_current_color = (rgba_t(g_cache->GetValue(CacheEntry::AnnColor, Colors::RED.to_rgba())));
    m_state         = ToolState::Selecting;
    m_imgui_id_texts.insert_or_assign(OSHOT_TEXT_OCR, &m_inputs.ocr_results.data);
    m_imgui_id_texts.insert_or_assign(OSHOT_TEXT_BARCODE, &m_inputs.barcode_text);
    m_show_window.Set(SubWindow::MainTextTools, g_config->File.show_text_tools);

    SyncRuntimeFromConfig();

#if OSHOT_METAL
    m_texture_id = ImTextureRef{};  // will be set by backend
#else
    const Result<ImTextureRef>& res = CreateTexture(0, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    TRY_MSG(res, "Failed to create openGL texture: {}");

    m_texture_id = res.get();

    // The screenshot texture worked, so assume these do too
    const bool aa = g_config->File.aa_icons;

    m_tool_textures[idx(ToolType::Rectangle)]       = CreateTexture(0, TOOL_ICON(aa, ICON_SQUARE)).get();
    m_tool_textures[idx(ToolType::RectangleFilled)] = CreateTexture(0, TOOL_ICON(aa, ICON_RECT_FILLED)).get();
    m_tool_textures[idx(ToolType::CircleFilled)]    = CreateTexture(0, TOOL_ICON(aa, ICON_CIRCLE_FILLED)).get();
    m_tool_textures[idx(ToolType::ToggleTextTools)] =
        CreateTexture(0, ICON_TEXT_TOOLS_RGBA, ICON_TEXT_TOOLS_W, ICON_TEXT_TOOLS_H).get();
    m_tool_textures[idx(ToolType::Circle)]        = CreateTexture(0, TOOL_ICON(aa, ICON_CIRCLE)).get();
    m_tool_textures[idx(ToolType::CounterBubble)] = CreateTexture(0, TOOL_ICON(aa, ICON_COUNTER_BUBBLE)).get();
    m_tool_textures[idx(ToolType::Pencil)]        = CreateTexture(0, TOOL_ICON(aa, ICON_PENCIL)).get();

    m_tool_textures[idx(ToolType::Arrow)]     = CreateTexture(0, TOOL_ICON(aa, ICON_ARROW)).get();
    m_tool_textures[idx(ToolType::Text)]      = CreateTexture(0, TOOL_ICON(aa, ICON_TEXT)).get();
    m_tool_textures[idx(ToolType::CopyImage)] = CreateTexture(0, TOOL_ICON(aa, ICON_COPY)).get();
    m_tool_textures[idx(ToolType::SaveImage)] = CreateTexture(0, TOOL_ICON(aa, ICON_SAVE)).get();
    m_tool_textures[idx(ToolType::Line)]      = CreateTexture(0, TOOL_ICON(aa, ICON_LINE)).get();
    m_tool_textures[idx(ToolType::Logo)]      = CreateTexture(0, OSHOT_LOGO_RGBA, OSHOT_LOGO_W, OSHOT_LOGO_H).get();
#endif

#ifndef DISABLE_PLUGINS
    // No-ops for PluginManager calls before the first StartPluginTask(), since an empty std::function throws
    PluginCallbacks cb;
    cb.on_status  = [](const std::string_view) {};
    cb.on_success = [](const std::string_view) {};
    cb.on_warning = [](const std::string_view) {};
    cb.on_error   = [](const std::string_view) {};
    cb.on_info    = [](const std::string_view) {};
    cb.on_output  = [](const std::string_view) {};
    cb.confirm    = [](const std::string_view, bool) -> bool { return false; };
    m_plugin_manager.SetCallbacks(cb);
#endif

    if (!fs::exists(m_inputs.ocr_model_downloaded_path))
        SetError(m_download_errors, OcrDownloadError::InvalidPath, _("No such directory or path"));
    else if (!fs::is_directory(m_inputs.ocr_model_downloaded_path))
        SetError(m_download_errors, OcrDownloadError::InvalidPath, _("Not a directory"));

#ifndef DISABLE_PLUGINS
    StartPluginsCapture();
#endif
    return Ok();
}

void ScreenshotTool::RenderOverlay()
{
#ifndef DISABLE_PLUGINS
    for (auto& [id, rt] : g_plugins)
        rt.RunUiTasks();
#endif

    const bool subwindow_shown = m_state == ToolState::Selected && m_show_window.Has(SubWindow::MainTextTools) &&
                                 m_show_window.HasAny(SubWindow::OcrDownload, SubWindow::Preferences);
    const bool disable_esc =
        m_current_actions.HasAny(CurrentAction::IsTextPlacing, CurrentAction::IsColorPicking) || subwindow_shown;

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

    // Save/copy or scan requested: this frame is read back as the image, so draw only the annotations
    if (WantsCleanFrame())
    {
        DrawAnnotations();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

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

    if (m_main_sel.selection.get_width() == 0 || m_main_sel.selection.get_height() == 0)
    {
        static ImVec2 hint_display;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), center_cond(hint_display), ImVec2(0.5f, 0.5f));
        ImGui::Begin("##select_area", nullptr, minimal_win_flags);
        ImGui::TextColored(Colors::GREEN, "%s", _("Select an area"));
        ImGui::TextColored(Colors::CYAN, "%s", _("Press Save/Copy for the full screenshot"));
        ImGui::TextColored((0x0077FFFF_rgba), "%s", _("CTRL+A for whole image selection"));
        ImGui::End();
    }

    if (m_state == ToolState::Selecting || m_state == ToolState::Selected || m_state == ToolState::Resizing ||
        m_state == ToolState::AnnResizing)
    {
        if (m_main_sel.selection.get_x() > 30 && m_main_sel.selection.get_y() > 30)
        {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3, 2));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
            ImGui::Begin("##frame_rate",
                         nullptr,
                         minimal_win_flags & ~(ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize));
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::End();
            ImGui::PopStyleVar(2);
        }

        DrawAnnotations();
        DrawDarkOverlay();
        DrawAnnotationResizeBorder();
        DrawSelectionBorder();
        const bool swallowed_deselect_click = HandleAnnotationSelectionInput();
        if (m_current_annotation_resize)
        {
            UpdateHandleHoverState(m_anno_sel);
            UpdateCursor(m_anno_sel);
        }
        else if (!swallowed_deselect_click)
        {
            HandleSelectionInput(m_main_sel);
            UpdateHandleHoverState(m_main_sel);
            UpdateCursor(m_main_sel);
        }
    }

    if (m_state == ToolState::Selected)
    {
        DrawAnnotationToolbar();

        if (m_current_actions.Has(CurrentAction::IsColorPicking))
            HandleColorPickerInput();
        else
            HandleAnnotationInput();

        if (g_config->Runtime.instant_copy_save != SavingOp::kNone)
            RequestComplete(g_config->Runtime.instant_copy_save);
    }

#ifndef DISABLE_PLUGINS
    NotifyPluginsOfSelection();
#endif

    ImGui::End();
    ImGui::PopStyleVar();

    bool open = m_show_window.Has(SubWindow::MainTextTools);
    if (m_state == ToolState::Selected && open)
    {
        const char* text_tools_name = _("Text tools###text_tools");

        // Where it was last shown (this session or imgui.ini), if it's about to (re)appear
        std::optional<ImRect> last_rect;
        if (const ImGuiWindow* window = ImGui::FindWindowByName(text_tools_name))
        {
            if (!window->WasActive)
                last_rect = window->Rect();
        }
        else if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(text_tools_name)))
        {
            const ImVec2 pos(float(settings->Pos.x), float(settings->Pos.y));
            last_rect = ImRect(pos, ImVec2(pos.x + float(settings->Size.x), pos.y + float(settings->Size.y)));
        }

        // It stays where it was left, unless it would cover the new selection, then it opens on the side with
        // more room like the first time. Decided before Begin(), since moving it afterwards shows it in the old
        // place for a frame
        constexpr float         margin    = 60.0f;
        const float             display_w = ImGui::GetIO().DisplaySize.x;
        const selection_rect_t& sel       = m_main_sel.selection;
        const ImRect sel_rect(sel.get_x(), sel.get_y(), sel.get_x() + sel.get_width(), sel.get_y() + sel.get_height());
        const bool   right  = display_w - sel_rect.Max.x > sel_rect.Min.x;
        const bool   covers = last_rect && last_rect->Overlaps(sel_rect);
        ImGui::SetNextWindowPos(right ? ImVec2(display_w - margin, margin) : ImVec2(margin, margin),
                                covers ? ImGuiCond_Always : ImGuiCond_FirstUseEver,
                                right ? ImVec2(1.0f, 0.0f) : ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(font_relative(22, 0), ImGuiCond_FirstUseEver);  // 0 = auto height
        ImGui::Begin(text_tools_name, &open, ImGuiWindowFlags_MenuBar);

        DrawMenuItems();
        DrawPreferencesWindow();
        DrawDownloadOCRWindow();
        DrawLogsWindow();
        DrawOcrTools();
        DrawAboutWindow();
        DrawBarDecodeTools();
#ifndef DISABLE_PLUGINS
        DrawManagePluginsWindow();
        DrawBrowsePluginsWindow();
        DrawInstallPluginsWindow();
        DrawPluginInstallStatus();
        for (auto& [id, rt] : g_plugins)
        {
            const oshot_plugin_t* plugin = rt.plugin;
            if (!plugin->render)
                continue;

            ImGui::PushID(id.c_str());
            if (ImGui::CollapsingHeader(plugin->name))
            {
                ImGui::BeginChild(
                    "##plugin", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
                rt.Render();
                ImGui::EndChild();
            }
            ImGui::PopID();
        }
#endif
        ImGui::End();
    }
    m_show_window.Set(SubWindow::MainTextTools, open);

#ifndef DISABLE_PLUGINS
    DrawPluginReports();
#endif

    HandleShortcutsInput();

    // ImGui's NewFrame() already closed the menu or popup open on this Esc, and that's all the Esc should do
    const bool popup_was_open =
        std::exchange(m_popup_open, ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !disable_esc && !popup_was_open)
        Cancel();
}

void ScreenshotTool::Cancel()
{
    m_state = ToolState::Idle;

    auto delete_texture = [](ImTextureRef& tex) {
#if OSHOT_METAL
        tex = ImTextureRef{};
#else
        if (tex._TexID)
        {
            auto texture = GLuint(intptr_t(tex._TexID));
            glDeleteTextures(1, &texture);
            tex = ImTextureRef{};
        }
#endif
    };

    delete_texture(m_texture_id);
    for (auto& tex : m_tool_textures)
        delete_texture(tex);
#ifndef DISABLE_PLUGINS
    for (auto& [key, tex] : m_plugin_icons)
        delete_texture(tex);
    m_plugin_icons.clear();
#endif

    // Only our references, the fonts themselves stay in ImGui's atlas
    m_font_cache.clear();

    if (m_on_cancel)
        m_on_cancel();
}

void ScreenshotTool::EndCapture()
{
#ifndef DISABLE_PLUGINS
    EndPluginsCapture();
#endif

    // Assigning {} frees the buffers, clear() would keep their capacity
    m_screenshot                = {};
    m_uncropped                 = {};
    m_annotations               = {};
    m_current_annotation        = {};
    m_current_annotation_resize = nullptr;

    // ~20 MB for a model. Loading it again takes about 0.1 s, on the next capture's first OCR
    m_ocr_api.End();
}

bool ScreenshotTool::OpenImage(const std::string& path)
{
    Result<capture_result_t> cap = load_image_rgba(path);
    MUST_OK(cap, {
        error("Failed to load image: {}", cap.error_v());
        return false;
    });

    m_screenshot = std::move(cap.get());
    fit_to_screen(m_screenshot);

#if OSHOT_METAL
    // Tell backend to recreate Metal texture
    if (m_on_image_reload)
        m_on_image_reload(m_screenshot);
#else

    // Recreate texture (CreateTexture() already deletes the old ones)
    const Result<ImTextureRef>& r =
        CreateTexture(m_texture_id._TexID, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    MUST_OK(r, {
        error("Failed to create openGL texture: {}", r.error_v());
        return false;
    });

    m_texture_id = r.get();
#endif

    ResetSelection();
    m_state = ToolState::Selecting;

#ifndef DISABLE_PLUGINS
    // For plugins, another image is a new capture
    EndPluginsCapture();
    StartPluginsCapture();
#endif
    return true;
}

void ScreenshotTool::ResetSelection()
{
    m_current_tool             = ToolType::kNone;
    m_main_sel.handle_hover    = HandleHovered::kNone;
    m_main_sel.dragging_handle = HandleHovered::kNone;
    m_input_owner              = InputOwner::kNone;

    m_main_sel.selection            = {};
    m_main_sel.drag_start_selection = {};
    m_main_sel.drag_start_mouse     = {};
    m_image_origin                  = {};
    m_image_end                     = {};
}

Result<> ScreenshotTool::CropToOutput(const std::deque<region_t>& layout, const monitor_t& target, int transform)
{
    const capture_result_t&  full    = m_uncropped.data.empty() ? m_screenshot : m_uncropped;
    Result<capture_result_t> cropped = crop_to_monitor(full, layout, target.geo);
    TRY_MSG(cropped, "Failed to crop capture to focused monitor: {}");

    m_screenshot = std::move(cropped.get());
    m_uncropped  = {};

    spdlog::debug("target.transform = {}", target.transform);
    if (transform >= 1 && transform <= 3)
        m_screenshot = rotate_rgba(m_screenshot, 4 - transform);  // swap 1 (90) and 3 (270)
    fit_to_screen(m_screenshot);

    const Result<ImTextureRef>& r =
        CreateTexture(m_texture_id._TexID, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    TRY_MSG(r, "Failed to recreate texture after crop: {}");
    m_texture_id = r.get();

    UpdateWindowBg();  // re-center for the new size

    return Ok();
}

// ------------------------------
// Frame read-back
// ------------------------------

// ------------------------------
// Idle rendering
// ------------------------------

double ScreenshotTool::GetIdleTimeout()
{
    // ImGui numbers the input events the backend queues, so a new number means input since the last frame
    const ImU32 event_id = ImGui::GetCurrentContext()->InputEventsNextEventId;
    if (std::exchange(m_last_input_event_id, event_id) != event_id)
        KeepDrawing();

    // 1 s is long enough for hover effects, tooltip delays and the smooth animations to finish after input.
    // A download changes the screen by itself
    if (WantsCleanFrame() || m_redraw_requested.exchange(false) ||
        std::chrono::steady_clock::now() - m_last_activity < 1s || (m_ocr_download && m_ocr_download->running))
        return 0.0;
#ifndef DISABLE_PLUGINS
    if (m_install_state && m_install_state->running)
        return 0.0;
    for (auto& [id, rt] : g_plugins)
    {
        std::scoped_lock lock(rt.reports_mutex);
        if (std::ranges::any_of(rt.reports, [](const plugin_report_t& r) { return !r.done; }))
            return 0.0;
    }
#endif

    // Waking up anyway shows what other threads changed (plugin results and tasks, a quit request),
    // and blinks the cursor of a focused text field
    return ImGui::GetIO().WantTextInput ? 0.25 : 0.5;
}

static std::atomic<void (*)()> wake_overlay_fn{ nullptr };

void register_wake_overlay(void (*fn)())
{
    wake_overlay_fn = fn;
}

void wake_overlay()
{
    g_ss_tool.RequestRedraw();
    if (void (*fn)() = wake_overlay_fn.load())
        fn();
}

void ScreenshotTool::OnCleanFrame()
{
    const capture_result_t& img = m_read_pixels ? m_read_pixels(GetActiveRegion()) : GetFinalImage();

    const ScanType scan = std::exchange(m_pending_scan, ScanType::kNone);
    if (scan == ScanType::Ocr)
        RunOcr(img);
    else if (scan == ScanType::Barcode)
        RunBarcodeScan(img);

#ifndef DISABLE_PLUGINS
    if (std::exchange(m_final_image_requested, false))
        for (auto& [id, rt] : g_plugins)
            rt.DeliverFinalImage(img);
#endif

    const SavingOp op = std::exchange(m_completed_op, SavingOp::kNone);
    if (op == SavingOp::kNone)
        return;

    // If it failed or the save dialog was canceled, keep the selection and annotations for another try. Not with
    // --instant-copy/--instant-save, which would request it again every frame
    const bool done = !m_on_complete || m_on_complete(op, img, g_config->File.image_out_type.second);
    if (done || g_config->Runtime.instant_copy_save != SavingOp::kNone)
        m_state = ToolState::Idle;
}

// ------------------------------
// Getters
// ------------------------------

capture_result_t ScreenshotTool::GetFinalImage()
{
    UpdateWindowBg();

    const region_t& region = GetActiveRegion();

    capture_result_t result;
    result.w = region.w;
    result.h = region.h;
    result.data.resize(size_t(region.w) * region.h * 4);

    const int src_width = m_screenshot.w;
    const int dst_width = region.w;

    // The part of the region inside the screenshot
    const int start_y = std::max(0, -region.y);
    const int end_y   = std::min(region.h, m_screenshot.h - region.y);
    const int start_x = std::max(0, -region.x);
    const int end_x   = std::min(region.w, m_screenshot.w - region.x);

    int width = end_x - start_x;

    const size_t bytes_to_copy = (width > 0) ? size_t(width) * 4 : 0;

    if (bytes_to_copy > 0)
    {
        for (int y = start_y; y < end_y; ++y)
        {
            const int    src_y         = region.y + y;
            const size_t src_row_start = (size_t(src_y) * src_width + (region.x + start_x)) * 4;
            const size_t dst_row_start = (size_t(y) * dst_width + start_x) * 4;

            std::memcpy(result.data.data() + dst_row_start, m_screenshot.view().data() + src_row_start, bytes_to_copy);
        }
    }

    return result;
}

bool ScreenshotTool::CanReadFinalImage() const
{
    return m_state != ToolState::Idle && !m_screenshot.data.empty() &&
           !m_show_window.Has(SubWindow::OutputMenuSelection);
}

region_t ScreenshotTool::GetActiveRegion() const
{
    bool has_selection = m_main_sel.selection.get_width() > 0 && m_main_sel.selection.get_height() > 0;

    if (!has_selection)
    {
        // Full screenshot (visible area)
        return region_t{ .x = 0, .y = 0, .w = m_screenshot.w, .h = m_screenshot.h };
    }

    const auto  img_w = float(m_screenshot.w);
    const auto  img_h = float(m_screenshot.h);
    const auto& s     = m_main_sel.selection;
    const float k     = m_image_scale;  // screen units -> screenshot pixels

    const int x0 = int(std::round(std::clamp((s.get_x() - m_image_origin.x) * k, 0.0f, img_w)));
    const int y0 = int(std::round(std::clamp((s.get_y() - m_image_origin.y) * k, 0.0f, img_h)));
    const int x1 = int(std::round(std::clamp((s.get_x() + s.get_width() - m_image_origin.x) * k, 0.0f, img_w)));
    const int y1 = int(std::round(std::clamp((s.get_y() + s.get_height() - m_image_origin.y) * k, 0.0f, img_h)));

    return region_t{ .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
}

// ------------------------------
// Textures, fonts and style
// ------------------------------

Result<ImTextureRef> ScreenshotTool::CreateTexture(ImTextureID tex, std::span<const uint8_t> data, int w, int h)
{
#if OSHOT_METAL
    // Metal backend handles textures separately
    return Ok(ImTextureRef{});
#else
    if (tex)
    {
        const auto old_texture = static_cast<GLuint>(tex);
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

ImFont* ScreenshotTool::CacheAndGetFont(const std::string& font_path, const float font_size)
{
    ImFont* font = GetCachedFont(font_path, font_size);
    if (font)
    {
        ImFontAtlasBuildMain(ImGui::GetIO().Fonts);
        return font;
    }

    ImFontConfig cfg;
    cfg.GlyphRanges = ImGui::GetIO().Fonts->GetGlyphRangesDefault();
    font            = add_font_file(ImGui::GetIO().Fonts, font_path, font_size, &cfg);
    if (!font)
    {
        // Cached too, so the warning shows once
        spdlog::warn("Font '{}' failed to load, using the default one", font_path);
        font = ImGui::GetDefaultFont();
    }

    const float safe_size = std::max(font_size, 16.0f);
    std::pair   key(font_path, safe_size);
    m_font_cache[key] = { .font_path = font_path, .font = font, .loaded = true };
    ImFontAtlasBuildMain(ImGui::GetIO().Fonts);

    return font;
}

ImFont* ScreenshotTool::GetCachedFont(const std::string& font_path, const float font_size) const
{
    if (font_path.empty())
        return ImGui::GetDefaultFont();

    const float safe_size = std::max(font_size, 16.0f);
    std::pair   key(font_path, safe_size);
    auto        it = m_font_cache.find(key);
    if (it != m_font_cache.end())
        return it->second.font;

    return nullptr;
}

void ScreenshotTool::StyleDefaultColor()
{
    // clang-format off
    constexpr std::array theme_colors = std::to_array<std::pair<ImGuiCol, rgba_t>>({
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
    });
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
        style.Colors[color] = rgba;
}

// ------------------------------
// State updates
// ------------------------------

// How many buttons of size `btn` separated by `spacing` fit in `length` px:
// n * btn + (n - 1) * spacing <= length
static size_t fit_count(float length, float btn, float spacing)
{
    if (length < btn)
        return 0;
    return static_cast<size_t>((length + spacing) / (btn + spacing));
}

static bool get_filtered_filenames(const std::string&                                 dir,
                                   std::vector<std::string>&                          list,
                                   const std::function<bool(const fs::path&)>&        filter,
                                   const std::function<std::string(const fs::path&)>& value_push)
{
    if (!fs::exists(dir))
        return false;

    for (const auto& entry : fs::recursive_directory_iterator(
             dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied))
    {
        if (filter(entry.path()))
            list.emplace_back(value_push(entry.path()));
    }

    return true;
}

void ScreenshotTool::UpdateWindowBg()
{
    // Calculate where the screenshot will be drawn (centered)
    // clang-format off
    auto* vp = ImGui::GetMainViewport();
    m_image_scale = m_full_res_image ? std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x) : 1.0f;
    ImVec2 image_size(
        float(m_screenshot.w) / m_image_scale,
        float(m_screenshot.h) / m_image_scale
    );

    m_image_origin = ImVec2(
        vp->Pos.x + std::floor((vp->Size.x - image_size.x) * 0.5f),
        vp->Pos.y + std::floor((vp->Size.y - image_size.y) * 0.5f)
    );

    m_image_end = ImVec2(
        m_image_origin.x + image_size.x,
        m_image_origin.y + image_size.y
    );
    // clang-format on
}

void ScreenshotTool::UpdateAnnsToolbarPos()
{
    m_fill_tools_complete = false;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2      btn   = GetToolButtonSize();

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float  sel_x   = m_main_sel.selection.get_x();
    const float  sel_y   = m_main_sel.selection.get_y();
    const float  sel_w   = m_main_sel.selection.get_width();
    const float  sel_h   = m_main_sel.selection.get_height();

    // The buttons go outside the selection, so skip the borders where they'd leave the screen
    std::array<bool, idx(SelectionBorder::COUNT)> on_screen{};
    on_screen[idx(SelectionBorder::Bottom)] = sel_y + sel_h + TOOLBAR_OFFSET + btn.y <= display.y;
    on_screen[idx(SelectionBorder::Right)]  = sel_x + sel_w + TOOLBAR_OFFSET + btn.x <= display.x;
    on_screen[idx(SelectionBorder::Top)]    = sel_y - TOOLBAR_OFFSET - btn.y >= 0.0f;
    on_screen[idx(SelectionBorder::Left)]   = sel_x - TOOLBAR_OFFSET - btn.x >= 0.0f;

    const size_t end  = idx(ToolType::COUNT) - 1;
    size_t       next = idx(ToolType::kNone) + 1;

    ToolType last_tool = ToolType::kNone;
    for (size_t i = 0; i < m_tools_borders.size(); ++i)
    {
        std::vector<ToolType>& tools = m_tools_borders[i];
        tools.clear();
        if (!on_screen[i])
            continue;

        const bool   is_side = i % 2 != 0;  // Bottom/Top = horizontal, Right/Left = vertical
        const size_t capacity =
            is_side ? fit_count(sel_h, btn.y, style.ItemSpacing.y) : fit_count(sel_w, btn.x, style.ItemSpacing.x);

        for (; next < end && tools.size() < capacity; ++next)
        {
            last_tool = toe<ToolType>(next);
            if (has_toolbar_button(last_tool))
                tools.emplace_back(last_tool);
        }
    }

    if (last_tool == ToolType::SaveImage)  // NOTE: Update if modifying enum class ToolType
        m_fill_tools_complete = true;
}

void ScreenshotTool::NormalizeSelection(selection_info_t& sel)
{
    if (sel.selection.start.x > sel.selection.end.x)
        std::swap(sel.selection.start.x, sel.selection.end.x);
    if (sel.selection.start.y > sel.selection.end.y)
        std::swap(sel.selection.start.y, sel.selection.end.y);
}

void ScreenshotTool::ClampSelectionToImage(selection_info_t& sel)
{
    selection_rect_t& s = sel.selection;

    // When it's moved, push it back whole instead of shrinking it
    if (sel.dragging_handle == HandleHovered::Move)
    {
        const float dx =
            std::max(m_image_origin.x - s.get_x(), 0.0f) + std::min(m_image_end.x - (s.get_x() + s.get_width()), 0.0f);
        const float dy =
            std::max(m_image_origin.y - s.get_y(), 0.0f) + std::min(m_image_end.y - (s.get_y() + s.get_height()), 0.0f);
        s.start.x += dx;
        s.end.x += dx;
        s.start.y += dy;
        s.end.y += dy;
        return;
    }

    for (point_t* p : { &s.start, &s.end })
    {
        p->x = std::clamp(p->x, m_image_origin.x, m_image_end.x);
        p->y = std::clamp(p->y, m_image_origin.y, m_image_end.y);
    }
}

void ScreenshotTool::SyncRuntimeFromConfig()
{
    m_inputs.ocr_path  = g_config->File.ocr_path;
    m_inputs.ocr_model = g_config->File.ocr_model;
    RefreshOcrModels();

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

    // get_filtered_filenames() appends to the list
    m_ocr_models_list.clear();
    get_filtered_filenames(
        m_inputs.ocr_path,
        m_ocr_models_list,
        [](const fs::path& entry) { return entry.extension().string() == ".traineddata"; },
        [](const fs::path& entry) { return entry.stem().string(); });
    std::ranges::sort(m_ocr_models_list);

    m_last_scanned_ocr_path = m_inputs.ocr_path;
    ClearError(ectx, OcrError::NeedToScanDir);

    if (m_ocr_models_list.empty())
    {
        SetError(ectx, OcrError::InvalidPath, _("Doesn't exist or is empty"));
    }
    else
    {
        ClearError(ectx, OcrError::InvalidPath);
        if (std::ranges::find(m_ocr_models_list, m_inputs.ocr_model) == m_ocr_models_list.end())
            SetError(ectx, OcrError::InvalidModel);
        else
            ClearError(ectx, OcrError::InvalidModel);
    }
}

void ScreenshotTool::UpdateHandleHoverState(selection_info_t& sel)
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    sel.handle_hover        = HandleHovered::kNone;

    const float sel_x = sel.selection.get_x();
    const float sel_y = sel.selection.get_y();
    const float sel_w = sel.selection.get_width();
    const float sel_h = sel.selection.get_height();
    const float hover = 15.0f;

    // clang-format off
    const std::array<handle_info_t, 8> handles = {
        { { .type = HandleHovered::TopLeft,
            .pos  = ImVec2(sel_x, sel_y),
            .rect = ImRect(ImVec2(sel_x - hover, sel_y - hover),
                           ImVec2(sel_x + hover, sel_y + hover)) },

          { .type = HandleHovered::TopRight,
            .pos  = ImVec2(sel_x + sel_w, sel_y),
            .rect = ImRect(ImVec2(sel_x + sel_w - hover, sel_y - hover),
                           ImVec2(sel_x + sel_w + hover, sel_y + hover)) },

          { .type = HandleHovered::BottomLeft,
            .pos  = ImVec2(sel_x, sel_y + sel_h),
            .rect = ImRect(ImVec2(sel_x - hover, sel_y + sel_h - hover),
                           ImVec2(sel_x + hover, sel_y + sel_h + hover)) },

          { .type = HandleHovered::BottomRight,
            .pos  = ImVec2(sel_x + sel_w, sel_y + sel_h),
            .rect = ImRect(ImVec2(sel_x + sel_w - hover, sel_y + sel_h - hover),
                           ImVec2(sel_x + sel_w + hover, sel_y + sel_h + hover)) },

          { .type = HandleHovered::Top,
            .pos  = ImVec2(sel_x + sel_w / 2, sel_y),
            .rect = ImRect(ImVec2(sel_x + sel_w / 2 - hover, sel_y - hover),
                           ImVec2(sel_x + sel_w / 2 + hover, sel_y + hover)) },

          { .type = HandleHovered::Bottom,
            .pos  = ImVec2(sel_x + sel_w / 2, sel_y + sel_h),
            .rect = ImRect(ImVec2(sel_x + sel_w / 2 - hover, sel_y + sel_h - hover),
                           ImVec2(sel_x + sel_w / 2 + hover, sel_y + sel_h + hover)) },

          { .type = HandleHovered::Left,
            .pos  = ImVec2(sel_x, sel_y + sel_h / 2),
            .rect = ImRect(ImVec2(sel_x - hover, sel_y + sel_h / 2 - hover),
                           ImVec2(sel_x + hover, sel_y + sel_h / 2 + hover)) },

          { .type = HandleHovered::Right,
            .pos  = ImVec2(sel_x + sel_w, sel_y + sel_h / 2),
            .rect = ImRect(ImVec2(sel_x + sel_w - hover, sel_y + sel_h / 2 - hover),
                           ImVec2(sel_x + sel_w + hover, sel_y + sel_h / 2 + hover)) } }
    };
    // clang-format on

    for (const handle_info_t& handle : handles)
    {
        if (handle.rect.Contains(mouse_pos))
        {
            sel.handle_hover = handle.type;
            break;
        }
    }
}

void ScreenshotTool::UpdateCursor(const selection_info_t& sel)
{
    if (m_current_tool != ToolType::kNone)
    {  // NOLINT
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
    else if (sel.handle_hover != HandleHovered::kNone || sel.dragging_handle != HandleHovered::kNone)
    {
        HandleHovered handle = (sel.dragging_handle != HandleHovered::kNone) ? sel.dragging_handle : sel.handle_hover;

        annotation_t*& annres        = m_current_annotation_resize;
        const bool     is_ann_text   = annres && sel.is_ann && annres->type == ToolType::Text;
        const bool     is_ann_circle = annres && sel.is_ann &&
                                       (annres->type == ToolType::Circle || annres->type == ToolType::CircleFilled ||
                                        annres->type == ToolType::CounterBubble);

        switch (handle)
        {
            case HandleHovered::Move: ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); break;

            case HandleHovered::TopLeft:
            case HandleHovered::BottomRight:
                if (is_ann_text)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                else
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                break;

            case HandleHovered::TopRight:
            case HandleHovered::BottomLeft:
                if (is_ann_text)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                else
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                break;

            case HandleHovered::Top:
            case HandleHovered::Bottom:
                if (is_ann_circle || is_ann_text)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                else
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                break;

            case HandleHovered::Left:
            case HandleHovered::Right:
                if (is_ann_circle || is_ann_text)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                else
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                break;

            default: ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); break;
        }
    }
    else if (m_state == ToolState::Selected || m_state == ToolState::Resizing || m_state == ToolState::AnnResizing)
    {
        const float sel_x = sel.selection.get_x();
        const float sel_y = sel.selection.get_y();
        const float sel_w = sel.selection.get_width();
        const float sel_h = sel.selection.get_height();

        // Inside the selection, for moving it
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

// ------------------------------
// Geometry
// ------------------------------

ImRect ScreenshotTool::GetAnnotationBBox(const annotation_t& ann) const
{
    switch (ann.type)
    {
        case ToolType::Circle:
        case ToolType::CircleFilled:
        case ToolType::CounterBubble:
        {
            // start = center, end = a point on the circumference
            const float dx     = ann.end.x - ann.start.x;
            const float dy     = ann.end.y - ann.start.y;
            const float radius = std::sqrt(dx * dx + dy * dy);
            return { ann.start.x - radius, ann.start.y - radius, ann.start.x + radius, ann.start.y + radius };
        }

        case ToolType::Line:
        case ToolType::Arrow:
        {
            // start/end are directional endpoints, not corners, so normalize
            // them, and pad by half the stroke so thin/axis-aligned segments
            // stay clickable.
            const float half_t = std::max(ann.thickness, 4.0f) * 0.5f;
            return { std::min(ann.start.x, ann.end.x) - half_t,
                     std::min(ann.start.y, ann.end.y) - half_t,
                     std::max(ann.start.x, ann.end.x) + half_t,
                     std::max(ann.start.y, ann.end.y) + half_t };
        }

        case ToolType::Text:
        {
            const float font_size = ann.thickness > 8.0f ? ann.thickness : ImGui::GetFontSize();
            ImFont*     font      = GetCachedFont(m_inputs.resolved_ann_font_path, font_size);
            if (font)
                ImFontAtlasBuildMain(ImGui::GetIO().Fonts);
            const ImVec2 size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, ann.text.c_str())
                                     : ImGui::CalcTextSize(ann.text.c_str());
            return { ann.start.x, ann.start.y, ann.start.x + size.x, ann.start.y + size.y };
        }

        case ToolType::Pencil:
        {
            if (ann.points.empty())
                return { ann.start.x, ann.start.y, ann.start.x, ann.start.y };
            ImVec2 min(ann.points.front().x, ann.points.front().y);
            ImVec2 max = min;
            for (const point_t& p : ann.points)
            {
                min.x = std::min(min.x, p.x);
                min.y = std::min(min.y, p.y);
                max.x = std::max(max.x, p.x);
                max.y = std::max(max.y, p.y);
            }
            return { min, max };
        }

        case ToolType::Rectangle:
        case ToolType::RectangleFilled:
        default:
            // Already corners
            return { ann.start.x, ann.start.y, ann.end.x, ann.end.y };
    }
}

ImRect ScreenshotTool::GetAnnotationHandleBox(const annotation_t& ann) const
{
    constexpr float PIXEL   = 1.0f;
    const float     padding = ann.thickness - PIXEL;
    const ImRect    bbox    = GetAnnotationBBox(ann);
    return { bbox.Min.x - padding, bbox.Min.y - padding, bbox.Max.x + padding, bbox.Max.y + padding };
}

ImVec2 ScreenshotTool::GetToolButtonSize()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return { TOOL_BUTTON_SIZE + style.FramePadding.x * 2.0f, TOOL_BUTTON_SIZE + style.FramePadding.y * 2.0f };
}

// ------------------------------
// Scans
// ------------------------------

void ScreenshotTool::RequestScan(ScanType type)
{
    if (g_config->File.render_anns && !m_annotations.empty() && m_read_pixels)
    {
        m_pending_scan = type;
        return;
    }

    const capture_result_t& img = GetFinalImage();
    if (type == ScanType::Ocr)
        RunOcr(img);
    else
        RunBarcodeScan(img);
}

void ScreenshotTool::RunOcr(const capture_result_t& img)
{
    ErrorContext<OcrError>& ectx = m_ocr_errors;

    const Result<>& configure_res = m_ocr_api.Configure(m_inputs.ocr_path.c_str(), m_inputs.ocr_model.c_str());
    if (!configure_res.ok())
    {
        SetError(ectx, OcrError::FailedToOCR, configure_res.error_v());
#ifndef DISABLE_PLUGINS
        NotifyPluginsOfOcr(nullptr);
#endif
        return;
    }

    Result<ocr_result_t> result = m_ocr_api.ExtractTextCapture(img);
    if (!result.ok())
    {
        // Don't leave the previous selection's text under the error, where Copy Text would still copy it
        m_inputs.ocr_results = {};
        m_inputs.ocr_original.clear();
        SetError(ectx, OcrError::FailedToOCR, result.error_v());
#ifndef DISABLE_PLUGINS
        NotifyPluginsOfOcr(nullptr);
#endif
        return;
    }

    ClearError(ectx, OcrError::FailedToOCR);
    m_inputs.ocr_results = std::move(result.get());
#ifndef DISABLE_PLUGINS
    TransformText(m_inputs.ocr_results.data, m_inputs.ocr_original, OSHOT_TEXT_OCR);
    NotifyPluginsOfOcr(&m_inputs.ocr_results);
#endif
}

void ScreenshotTool::RunBarcodeScan(const capture_result_t& img)
{
    ErrorContext<BarcodeError>& ectx = m_barcode_errors;

    Result<barcode_result_t> scan = m_barcode_api.ExtractTextsCapture(img);
    if (!scan.ok())
    {
        m_inputs.barcode_scan_result = {};
        m_inputs.barcode_text.clear();
        m_inputs.barcode_original.clear();
        SetError(ectx, BarcodeError::FailedToScan, scan.error_v());
#ifndef DISABLE_PLUGINS
        for (auto& [id, rt] : g_plugins)
            if (OSHOT_PLUGIN_HAS(rt.plugin, on_barcodes_done) && rt.plugin->on_barcodes_done)
                rt.plugin->on_barcodes_done(rt.state, nullptr, 0);
#endif
        return;
    }

    ClearError(ectx, BarcodeError::FailedToScan);
    m_inputs.barcode_scan_result = std::move(scan.get());
    m_inputs.barcode_text.clear();
    for (const std::string& data : m_inputs.barcode_scan_result.datas)
        m_inputs.barcode_text += data + "\n\n";

#ifndef DISABLE_PLUGINS
    TransformText(m_inputs.barcode_text, m_inputs.barcode_original, OSHOT_TEXT_BARCODE);

    std::vector<oshot_barcode_t> codes;
    codes.reserve(m_inputs.barcode_scan_result.codes.size());
    for (const barcode_t& c : m_inputs.barcode_scan_result.codes)
        codes.push_back({ .type = { .p = c.type.c_str(), .len = c.type.length() },
                          .data = { .p = c.data.c_str(), .len = c.data.length() },
                          .x    = c.x,
                          .y    = c.y,
                          .w    = c.w,
                          .h    = c.h });
    for (auto& [id, rt] : g_plugins)
        if (OSHOT_PLUGIN_HAS(rt.plugin, on_barcodes_done) && rt.plugin->on_barcodes_done)
            rt.plugin->on_barcodes_done(rt.state, codes.data(), codes.size());
#endif
}
