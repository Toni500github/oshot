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

#include <SDL3/SDL_render.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
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
#include "imgui/imgui_internal.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/spdlog.h"
#include "text_extraction.hpp"
#include "theme.hpp"
#include "tool_icons.h"
#include "util.hpp"
#include "util/color.hpp"
#include "util/dialogs.hpp"
#include "util/fonts.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
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

    // g_ss_tool outlives a capture, since the tray runs every one of them with it
    ResetSelection();
    m_anno_sel.handle_hover    = HandleHovered::kNone;
    m_anno_sel.dragging_handle = HandleHovered::kNone;
    m_current_annotation       = {};
    m_edited_text.reset();
    m_current_annotation_resize = nullptr;
    m_current_actions           = {};
    m_pending_scan              = ScanType::kNone;
    m_completed_op              = SavingOp::kNone;
    m_last_esc_time             = -1.0;
    m_flags                     = {};
    m_flags.Set(CaptureFlag::WrapText);
    m_hover.reset();
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

        result = capture_full_screen(get_session_type());
        TRY_MSG(result, "Failed to capture screenshot: {}");
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
    fit_to_screen(m_screenshot);

    const Result<ImTextureRef>& res = CreateTexture(0, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    TRY_MSG(res, "Failed to create the screenshot's texture: {}");
    m_texture_id = res.get();

    // Only the screenshot, nothing for the text tools window
    if (g_config->Runtime.instant_copy_save != SavingOp::kNone)
    {
        m_state = ToolState::Selecting;
        return Ok();
    }

#ifndef DISABLE_PLUGINS
    static std::once_flag plugins_loaded;
    std::call_once(plugins_loaded, [&] { load_plugins(m_plugin_manager.GetStateManager()); });
#endif

    m_inputs = { .ocr_path                  = "",
                 .ocr_language              = g_config->File.ocr_language,
                 .ocr_download_repo         = g_config->File.ocr_get_repo,
                 .ocr_model_downloaded_path = g_config->GetConfigDirPath() + DIR_SEP_STR "models",
                 .ocr_seconds               = 0.0f,
                 .ocr_results               = {},
                 .barcode_text              = "",
                 .barcode_scanned           = false,
                 .barcode_seconds           = 0.0f,
                 .barcode_scan_result       = {},
                 .ann_font                  = "",
                 .resolved_ann_font_path    = "",
                 .ocr_original              = "",
                 .barcode_original          = "" };

    m_current_color = (rgba_t(g_cache->GetValue(CacheEntry::AnnColor, Colors::RED.to_rgba())));
    m_state         = ToolState::Selecting;
    m_show_window.Set(SubWindow::MainTextTools, g_config->File.show_text_tools);
    m_code_table = {};

    SyncRuntimeFromConfig();

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
        m_download_errors.Set(OcrDownloadError::InvalidPath, _("No such directory or path"));
    else if (!fs::is_directory(m_inputs.ocr_model_downloaded_path))
        m_download_errors.Set(OcrDownloadError::InvalidPath, _("Not a directory"));

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

    // One Esc does one thing. ImGui's NewFrame() already used it to close a menu or popup or to leave the active item,
    // e.g. a text field, and the text being typed and the color picker handle their own
    m_flags.Set(CaptureFlag::Esc,
                ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                    !m_flags.HasAny(CaptureFlag::PopupOpen, CaptureFlag::ItemActive) &&
                    !m_current_actions.HasAny(CurrentAction::IsTextPlacing, CurrentAction::IsColorPicking));

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

    ImGui::GetBackgroundDrawList()->AddImage(m_texture_id, m_image_rect.Min, m_image_rect.Max);

    if (ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear)
        ImGui::GetBackgroundDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

    // Save/copy or scan requested: this frame is read back as the image, so draw only the annotations, with the
    // text still being typed
    if (WantsCleanFrame())
    {
        CommitTextAnnotation();
        DrawAnnotations();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    if (m_main_sel.selection.is_empty())
    {
        const ui::OverlayWindow win("##select_area",
                                    ImGui::GetMainViewport()->GetCenter(),
                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground,
                                    ImVec2(0.5f, 0.5f));
        ImGui::TextColored(theme::color(AppCol::HintSelect), "%s", _("Select an area"));
        ImGui::TextColored(theme::color(AppCol::HintFull), "%s", _("Press Save/Copy for the full screenshot"));
        ImGui::TextColored(theme::color(AppCol::HintSelectAll),
                           "%s",
                           i18n::tr_fmt("{}+A for whole image selection", SHORTCUT_MOD).c_str());
    }

    if (IsActive())
    {
        if (m_main_sel.selection.get_x() > 30 && m_main_sel.selection.get_y() > 30)
        {
            ui::ScopedStyle style;
            style.Var(ImGuiStyleVar_WindowPadding, ImVec2(3, 2)).Var(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
            const ui::OverlayWindow win("##frame_rate", ImVec2(0, 0), ImGuiWindowFlags_NoInputs);
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        }

        DrawAnnotations();
        DrawDarkOverlay();
        DrawAnnotationResizeBorder();
        DrawSelectionBorder();
        const bool swallowed_deselect_click = HandleAnnotationSelectionInput();
        if (m_current_annotation_resize)
        {
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
        const ImRect            sel_rect  = sel.rect();
        const bool              right     = display_w - sel_rect.Max.x > sel_rect.Min.x;
        const bool              covers    = last_rect && last_rect->Overlaps(sel_rect);
        ImGui::SetNextWindowPos(right ? ImVec2(display_w - margin, margin) : ImVec2(margin, margin),
                                covers ? ImGuiCond_Always : ImGuiCond_FirstUseEver,
                                right ? ImVec2(1.0f, 0.0f) : ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ui::FontRelative(22, 0), ImGuiCond_FirstUseEver);  // 0 = auto height
        ImGui::Begin(text_tools_name, &open, ImGuiWindowFlags_MenuBar);

        DrawMenuItems();
        DrawPreferencesWindow();
        DrawDownloadOCRWindow();
        DrawLogsWindow();
        DrawAboutWindow();
#ifndef DISABLE_PLUGINS
        DrawManagePluginsWindow();
        DrawBrowsePluginsWindow();
        DrawInstallPluginsWindow();
        DrawPluginInstallStatus();
#endif

        // OCR, codes, then a tab for each plugin that draws its own section
        if (ImGui::BeginTabBar("##text_tools_tabs", ImGuiTabBarFlags_FittingPolicyScroll))
        {
            if (ImGui::BeginTabItem(_("Text")))
            {
                DrawOcrTools();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(_("QR & Barcodes")))
            {
                DrawBarDecodeTools();
                ImGui::EndTabItem();
            }
#ifndef DISABLE_PLUGINS
            for (auto& [id, rt] : g_plugins)
            {
                const oshot_plugin_t* plugin = rt.plugin;
                if (!plugin->render)
                    continue;

                ImGui::PushID(id.c_str());
                if (ImGui::BeginTabItem(plugin->name))
                {
                    rt.Render();
                    ImGui::EndTabItem();
                }
                ImGui::PopID();
            }
#endif
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
    m_show_window.Set(SubWindow::MainTextTools, open);

#ifndef DISABLE_PLUGINS
    DrawPluginReports();
#endif

    HandleShortcutsInput();

    m_flags.Set(CaptureFlag::PopupOpen,
                ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    m_flags.Set(CaptureFlag::ItemActive, ImGui::IsAnyItemActive());

    // Unless a sub-window took it, Esc cancels a drag, deselects the annotation or drops the tool, and only then quits.
    // A quick second press is a habit, not a quit
    if (m_flags.Has(CaptureFlag::Esc))
    {
        if (m_input_owner == InputOwner::Selection)
        {
            m_main_sel.selection       = m_main_sel.before_drag;
            m_main_sel.dragging_handle = HandleHovered::kNone;
            m_input_owner              = InputOwner::kNone;
            m_state                    = m_main_sel.state_before_drag;
            UpdateAnnsToolbarPos();
        }
        else if (m_current_actions.Has(CurrentAction::IsDrawing))
        {
            m_current_actions.Clear(CurrentAction::IsDrawing);
            m_current_annotation = {};
        }
        else if (m_current_annotation_resize && m_anno_sel.dragging_handle != HandleHovered::kNone)
        {
            *m_current_annotation_resize = m_ann_drag_start;
            m_anno_sel.dragging_handle   = HandleHovered::kNone;
            m_input_owner                = InputOwner::kNone;
        }
        else if (m_current_annotation_resize)
        {
            DeselectAnnotation();
        }
        else if (m_current_tool != ToolType::kNone)
        {
            m_current_tool = ToolType::kNone;
        }
        else if (ImGui::GetTime() - m_last_esc_time >= 0.15)
        {
            Cancel();
            return;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        m_last_esc_time = ImGui::GetTime();
}

void ScreenshotTool::Cancel()
{
    m_state = ToolState::Idle;

    ReleaseTextures();

    // Only our references, the fonts themselves stay in ImGui's atlas
    m_font_cache.clear();

    if (m_backend.on_cancel)
        m_backend.on_cancel();
}

void ScreenshotTool::EndCapture()
{
#ifndef DISABLE_PLUGINS
    EndPluginsCapture();
#endif

    // Assigning {} frees the buffers, clear() would keep their capacity
    m_screenshot                = {};
    m_desktop                   = {};
    m_annotations               = {};
    m_current_annotation        = {};
    m_current_annotation_resize = nullptr;
    m_size_estimate             = {};  // waits for the encoder, which holds a copy of the selection

    // The model is tens of MB, and the next capture's first OCR reloads it quickly
    m_ocr_api.End();
}

bool ScreenshotTool::OpenImage(const std::string& path)
{
    Result<capture_result_t> cap = load_image_rgba(path);
    MUST_OK(cap, {
        dialog::error("Failed to load image: {}", cap.error_v());
        return false;
    });

    m_screenshot = std::move(cap.get());
    fit_to_screen(m_screenshot);

    // Recreate texture (CreateTexture() already deletes the old ones)
    const Result<ImTextureRef>& r =
        CreateTexture(m_texture_id._TexID, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    MUST_OK(r, {
        dialog::error("Failed to create the image's texture: {}", r.error_v());
        return false;
    });

    m_texture_id = r.get();

    ResetSelection();
    m_state = ToolState::Selecting;

#ifndef DISABLE_PLUGINS
    // For plugins, another image is a new capture
    EndPluginsCapture();
    StartPluginsCapture();
#endif
    return true;
}

Result<> ScreenshotTool::CropAndUseDisplay(const SDL_DisplayID display, const bool keep_desktop)
{
    if (m_screenshot.whole_desktop)
    {
        int count = 0;
        SDL_free(SDL_GetDisplays(&count));
        if (count <= 1)
        {
            m_screenshot.whole_desktop = false;
            return Ok();
        }
        m_desktop = { .image = std::move(m_screenshot), .crop = 0 };
    }
    if (m_desktop.image.data.empty())
        return Ok();

    if (display != m_desktop.crop)
    {
        Result<capture_result_t> cropped = crop_to_display(m_desktop.image, display);
        if (!cropped.ok())
        {
            // Show every monitor rather than nothing
            if (m_screenshot.data.empty())
                m_screenshot = std::move(m_desktop.image);
            m_screenshot.whole_desktop = false;
            m_desktop                  = {};
            return Err("Failed to crop the capture to the overlay's display: {}", cropped.error_v());
        }
        m_screenshot   = std::move(cropped.get());
        m_desktop.crop = display;
        spdlog::debug("Cropped the desktop to {}: {}x{}", SDL_GetDisplayName(display), m_screenshot.w, m_screenshot.h);

        // Before StartWindow() there's no texture yet
        if (m_texture_id._TexID)
        {
            fit_to_screen(m_screenshot);
            const Result<ImTextureRef>& r =
                CreateTexture(m_texture_id._TexID, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
            TRY_MSG(r, "Failed to recreate the screenshot's texture: {}");
            m_texture_id = r.get();
            ResetSelection();
        }
    }

    if (!keep_desktop)
        m_desktop = {};
    return Ok();
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
    m_image_rect                    = {};
}

// ------------------------------
// Idle rendering
// ------------------------------

double ScreenshotTool::GetIdleTimeout()
{
    // ImGui numbers the input events the backend queues, so a new number means input since the last frame
    const ImU32 event_id = ImGui::GetCurrentContext()->InputEventsNextEventId;
    if (std::exchange(m_idle.last_input_event_id, event_id) != event_id)
        KeepDrawing();

    // 1 s is long enough for hover effects, tooltip delays and the smooth animations to finish after input.
    // A download changes the screen by itself
    if (WantsCleanFrame() || m_idle.redraw_requested.exchange(false) ||
        std::chrono::steady_clock::now() - m_idle.last_activity < 1s || (m_ocr_download && m_ocr_download->running))
        return 0.0;
#ifndef DISABLE_PLUGINS
    if (m_install.state && m_install.state->running)
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

// ------------------------------
// Frame read-back
// ------------------------------

void ScreenshotTool::OnCleanFrame()
{
    const capture_result_t& img = m_backend.read_pixels ? m_backend.read_pixels(GetActiveRegion()) : GetFinalImage();

    const ScanType scan = std::exchange(m_pending_scan, ScanType::kNone);
    if (scan == ScanType::Ocr)
        RunOcr(img);
    else if (scan == ScanType::Barcode)
        RunBarcodeScan(img);

#ifndef DISABLE_PLUGINS
    if (m_flags.Take(CaptureFlag::FinalImageRequested))
        for (auto& [id, rt] : g_plugins)
            rt.DeliverFinalImage(img);
#endif

    const SavingOp op = std::exchange(m_completed_op, SavingOp::kNone);
    if (op == SavingOp::kNone)
        return;

    // If it failed or the save dialog was canceled, keep the selection and annotations for another try. Not with
    // --instant-copy/--instant-save, which would request it again every frame
    const bool done = !m_backend.on_complete || m_backend.on_complete(op, img, g_config->File.image_out_type.second);
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
    return IsActive() && !m_screenshot.data.empty();
}

region_t ScreenshotTool::GetActiveRegion() const
{
    bool has_selection = !m_main_sel.selection.is_empty();

    if (!has_selection)
    {
        // Full screenshot (visible area)
        return region_t{ .x = 0, .y = 0, .w = m_screenshot.w, .h = m_screenshot.h };
    }

    const auto  img_w = float(m_screenshot.w);
    const auto  img_h = float(m_screenshot.h);
    const auto& s     = m_main_sel.selection;
    const float k     = m_image_scale;  // screen units -> screenshot pixels

    const int x0 = int(std::round(std::clamp((s.get_x() - m_image_rect.Min.x) * k, 0.0f, img_w)));
    const int y0 = int(std::round(std::clamp((s.get_y() - m_image_rect.Min.y) * k, 0.0f, img_h)));
    const int x1 = int(std::round(std::clamp((s.get_right() - m_image_rect.Min.x) * k, 0.0f, img_w)));
    const int y1 = int(std::round(std::clamp((s.get_bottom() - m_image_rect.Min.y) * k, 0.0f, img_h)));

    return region_t{ .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
}

// ------------------------------
// Textures, fonts and style
// ------------------------------

// ImGui keeps the SDL_Texture* as an integer ImTextureID
static SDL_Texture* sdl_texture(const ImTextureID id)
{
    return reinterpret_cast<SDL_Texture*>(intptr_t(id));  // NOLINT(performance-no-int-to-ptr)
}

Result<ImTextureRef> ScreenshotTool::CreateTexture(ImTextureID tex, std::span<const uint8_t> data, int w, int h)
{
    if (tex)
        SDL_DestroyTexture(sdl_texture(tex));

    SDL_Renderer* renderer = SDL_GetRenderer(m_backend.window);
    if (!renderer)
        return Err("the overlay has no renderer");

    // fit_to_screen() keeps the screenshot within one display, so only a huge display or a weak GPU hits this
    const Sint64 max_size =
        SDL_GetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
    if (max_size > 0 && (w > max_size || h > max_size))
        return Err("the image is {}x{}, but the renderer takes at most {}x{}", w, h, max_size, max_size);

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!texture)
        return Err("{}", SDL_GetError());
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    if (!SDL_UpdateTexture(texture, nullptr, data.data(), w * 4))
    {
        const std::string error = SDL_GetError();
        SDL_DestroyTexture(texture);
        return Err("{}", error);
    }

    ImTextureRef ref;
    ref._TexID = ImTextureID(intptr_t(texture));
    return Ok(ref);
}

void ScreenshotTool::ReleaseTextures()
{
    const auto destroy = [](ImTextureRef& tex) {
        if (tex._TexID)
            SDL_DestroyTexture(sdl_texture(tex._TexID));
        tex = ImTextureRef{};
    };

    destroy(m_texture_id);
    for (auto& tex : m_tool_textures)
        destroy(tex);
#ifndef DISABLE_PLUGINS
    for (auto& [key, tex] : m_plugin_icons)
        destroy(tex);
    m_plugin_icons.clear();
#endif
}

ImFont* ScreenshotTool::CacheAndGetFont(const std::string& font_path)
{
    ImFont* font = GetCachedFont(font_path);
    if (font)
    {
        ImFontAtlasBuildMain(ImGui::GetIO().Fonts);
        return font;
    }

    ImFontConfig cfg;
    cfg.GlyphRanges = ImGui::GetIO().Fonts->GetGlyphRangesDefault();
    font            = add_font_file(ImGui::GetIO().Fonts, font_path, 16.0f, &cfg);
    if (!font)
    {
        // Cached too, so the warning shows once
        spdlog::warn("Font '{}' failed to load, using the default one", font_path);
        font = ImGui::GetDefaultFont();
    }

    m_font_cache[font_path] = font;
    ImFontAtlasBuildMain(ImGui::GetIO().Fonts);

    return font;
}

ImFont* ScreenshotTool::GetCachedFont(const std::string& font_path) const
{
    if (font_path.empty())
        return ImGui::GetDefaultFont();

    const auto it = m_font_cache.find(font_path);
    return it != m_font_cache.end() ? it->second : nullptr;
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

void ScreenshotTool::UpdateWindowBg()
{
    // Calculate where the screenshot will be drawn (centered)
    // clang-format off
    auto* vp = ImGui::GetMainViewport();
    // Drawn at full resolution where a screen unit has more than one pixel (scaled Wayland outputs, Retina)
    m_image_scale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    ImVec2 image_size(
        float(m_screenshot.w) / m_image_scale,
        float(m_screenshot.h) / m_image_scale
    );

    m_image_rect.Min = ImVec2(
        vp->Pos.x + std::floor((vp->Size.x - image_size.x) * 0.5f),
        vp->Pos.y + std::floor((vp->Size.y - image_size.y) * 0.5f)
    );

    m_image_rect.Max = m_image_rect.Min + image_size;
    // clang-format on
}

std::vector<ToolType> ScreenshotTool::GetToolbarButtons() const
{
    const bool            text_tools_shown = m_show_window.Has(SubWindow::MainTextTools);
    std::vector<ToolType> tools;
    for (size_t i = idx(ToolType::kNone) + 1; i < idx(ToolType::COUNT); ++i)
    {
        const auto tool = toe<ToolType>(i);
        if (has_toolbar_button(tool) && !(text_tools_shown && tool == ToolType::ToggleTextTools))
            tools.push_back(tool);
    }
    return tools;
}

size_t ScreenshotTool::CountPluginToolbarButtons()
{
    size_t n = 0;
#ifndef DISABLE_PLUGINS
    for (const auto& [id, rt] : g_plugins)
        n += size_t(std::ranges::count(rt.actions, OSHOT_PLACE_TOOLBAR, &plugin_action_t::place));
#endif
    return n;
}

void ScreenshotTool::UpdateAnnsToolbarPos()
{
    m_toolbar_rows.clear();

    const ImGuiStyle&           style     = ImGui::GetStyle();
    const ImVec2                btn       = GetToolButtonSize();
    const ImVec2                display   = ImGui::GetIO().DisplaySize;
    const selection_rect_t&     sel       = m_main_sel.selection;
    const std::vector<ToolType> tools     = GetToolbarButtons();
    const size_t                n_plugins = CountPluginToolbarButtons();

    // At least one, sticking out past a border shorter than a button
    const size_t row_capacity  = std::max<size_t>(1, fit_count(sel.get_width(), btn.x, style.ItemSpacing.x));
    const size_t side_capacity = std::max<size_t>(1, fit_count(sel.get_height(), btn.y, style.ItemSpacing.y));

    // Each border takes as many buttons as fit along it, and what's left goes around again one button further
    // out, until all are placed or a ring would be entirely off screen
    size_t next = 0;
    for (size_t ring = 0; next < tools.size(); ++ring)
    {
        const ImVec2 out = GetToolbarRingOffset(ring);

        std::array<bool, idx(SelectionBorder::COUNT)> on_screen{};
        on_screen[idx(SelectionBorder::Bottom)] = sel.get_bottom() + out.y + btn.y <= display.y;
        on_screen[idx(SelectionBorder::Right)]  = sel.get_right() + out.x + btn.x <= display.x;
        on_screen[idx(SelectionBorder::Top)]    = sel.get_y() - out.y - btn.y >= 0.0f;
        on_screen[idx(SelectionBorder::Left)]   = sel.get_x() - out.x - btn.x >= 0.0f;
        if (std::ranges::none_of(on_screen, std::identity{}))
            break;

        for (size_t i = 0; i < on_screen.size() && next < tools.size(); ++i)
        {
            if (!on_screen[i])
                continue;

            const auto   border   = toe<SelectionBorder>(i);
            const bool   is_row   = border == SelectionBorder::Bottom || border == SelectionBorder::Top;
            const size_t capacity = is_row ? row_capacity : side_capacity;
            const size_t left     = tools.size() - next;

            // SaveImage only goes in a row with room for the plugins' buttons after it
            size_t take = std::min(capacity, left);
            if (take == left && take + n_plugins > capacity)
                take = left - 1;
            if (take == 0)
                continue;

            const auto first = tools.begin() + std::ptrdiff_t(next);
            m_toolbar_rows.push_back(
                { .border = border, .ring = ring, .tools = { first, first + std::ptrdiff_t(take) } });
            next += take;
        }
    }

    m_flags.Set(CaptureFlag::ToolbarFits, next == tools.size());
}

void ScreenshotTool::ClampSelectionToImage(selection_info_t& sel)
{
    selection_rect_t& s = sel.selection;

    // When it's moved, push it back whole instead of shrinking it
    if (sel.dragging_handle == HandleHovered::Move)
    {
        const ImRect& img = m_image_rect;
        s.translate(std::max(img.Min.x - s.get_x(), 0.0f) + std::min(img.Max.x - s.get_right(), 0.0f),
                    std::max(img.Min.y - s.get_y(), 0.0f) + std::min(img.Max.y - s.get_bottom(), 0.0f));
        return;
    }

    for (ImVec2* p : { &s.start, &s.end })
    {
        p->x = std::clamp(p->x, m_image_rect.Min.x, m_image_rect.Max.x);
        p->y = std::clamp(p->y, m_image_rect.Min.y, m_image_rect.Max.y);
    }
}

// The languages of the tesseract-ocr/tessdata models, by code
static constexpr auto OCR_LANGUAGE_NAMES = std::to_array<std::pair<std::string_view, const char*>>({
    { "afr", N_("Afrikaans") },
    { "amh", N_("Amharic") },
    { "ara", N_("Arabic") },
    { "asm", N_("Assamese") },
    { "aze", N_("Azerbaijani") },
    { "aze_cyrl", N_("Azerbaijani (Cyrillic)") },
    { "bel", N_("Belarusian") },
    { "ben", N_("Bengali") },
    { "bod", N_("Tibetan") },
    { "bos", N_("Bosnian") },
    { "bre", N_("Breton") },
    { "bul", N_("Bulgarian") },
    { "cat", N_("Catalan") },
    { "ceb", N_("Cebuano") },
    { "ces", N_("Czech") },
    { "chi_sim", N_("Chinese (Simplified)") },
    { "chi_sim_vert", N_("Chinese (Simplified, vertical)") },
    { "chi_tra", N_("Chinese (Traditional)") },
    { "chi_tra_vert", N_("Chinese (Traditional, vertical)") },
    { "chr", N_("Cherokee") },
    { "cos", N_("Corsican") },
    { "cym", N_("Welsh") },
    { "dan", N_("Danish") },
    { "deu", N_("German") },
    { "deu_latf", N_("German (Fraktur)") },
    { "div", N_("Dhivehi") },
    { "dzo", N_("Dzongkha") },
    { "ell", N_("Greek") },
    { "eng", N_("English") },
    { "enm", N_("Middle English") },
    { "epo", N_("Esperanto") },
    { "est", N_("Estonian") },
    { "eus", N_("Basque") },
    { "fao", N_("Faroese") },
    { "fas", N_("Persian") },
    { "fil", N_("Filipino") },
    { "fin", N_("Finnish") },
    { "fra", N_("French") },
    { "frm", N_("Middle French") },
    { "fry", N_("Western Frisian") },
    { "gla", N_("Scottish Gaelic") },
    { "gle", N_("Irish") },
    { "glg", N_("Galician") },
    { "grc", N_("Ancient Greek") },
    { "guj", N_("Gujarati") },
    { "hat", N_("Haitian Creole") },
    { "heb", N_("Hebrew") },
    { "hin", N_("Hindi") },
    { "hrv", N_("Croatian") },
    { "hun", N_("Hungarian") },
    { "hye", N_("Armenian") },
    { "iku", N_("Inuktitut") },
    { "ind", N_("Indonesian") },
    { "isl", N_("Icelandic") },
    { "ita", N_("Italian") },
    { "ita_old", N_("Italian (old)") },
    { "jav", N_("Javanese") },
    { "jpn", N_("Japanese") },
    { "jpn_vert", N_("Japanese (vertical)") },
    { "kan", N_("Kannada") },
    { "kat", N_("Georgian") },
    { "kat_old", N_("Georgian (old)") },
    { "kaz", N_("Kazakh") },
    { "khm", N_("Khmer") },
    { "kir", N_("Kyrgyz") },
    { "kmr", N_("Kurdish (Kurmanji)") },
    { "kor", N_("Korean") },
    { "kor_vert", N_("Korean (vertical)") },
    { "lao", N_("Lao") },
    { "lat", N_("Latin") },
    { "lav", N_("Latvian") },
    { "lit", N_("Lithuanian") },
    { "ltz", N_("Luxembourgish") },
    { "mal", N_("Malayalam") },
    { "mar", N_("Marathi") },
    { "mkd", N_("Macedonian") },
    { "mlt", N_("Maltese") },
    { "mon", N_("Mongolian") },
    { "mri", N_("Maori") },
    { "msa", N_("Malay") },
    { "mya", N_("Burmese") },
    { "nep", N_("Nepali") },
    { "nld", N_("Dutch") },
    { "nor", N_("Norwegian") },
    { "oci", N_("Occitan") },
    { "ori", N_("Odia") },
    { "pan", N_("Punjabi") },
    { "pol", N_("Polish") },
    { "por", N_("Portuguese") },
    { "pus", N_("Pashto") },
    { "que", N_("Quechua") },
    { "ron", N_("Romanian") },
    { "rus", N_("Russian") },
    { "san", N_("Sanskrit") },
    { "sin", N_("Sinhala") },
    { "slk", N_("Slovak") },
    { "slv", N_("Slovenian") },
    { "snd", N_("Sindhi") },
    { "spa", N_("Spanish") },
    { "spa_old", N_("Spanish (old)") },
    { "sqi", N_("Albanian") },
    { "srp", N_("Serbian") },
    { "srp_latn", N_("Serbian (Latin)") },
    { "sun", N_("Sundanese") },
    { "swa", N_("Swahili") },
    { "swe", N_("Swedish") },
    { "syr", N_("Syriac") },
    { "tam", N_("Tamil") },
    { "tat", N_("Tatar") },
    { "tel", N_("Telugu") },
    { "tgk", N_("Tajik") },
    { "tha", N_("Thai") },
    { "tir", N_("Tigrinya") },
    { "ton", N_("Tongan") },
    { "tur", N_("Turkish") },
    { "uig", N_("Uyghur") },
    { "ukr", N_("Ukrainian") },
    { "urd", N_("Urdu") },
    { "uzb", N_("Uzbek") },
    { "uzb_cyrl", N_("Uzbek (Cyrillic)") },
    { "vie", N_("Vietnamese") },
    { "yid", N_("Yiddish") },
    { "yor", N_("Yoruba") },
});

void ScreenshotTool::SyncRuntimeFromConfig()
{
    // The language last picked in the text tools, unless ocr-language changed since
    const std::string& last = g_cache->GetValue<std::string>(CacheEntry::OcrLanguage, "", true);
    const bool use_last   = !last.empty() && g_cache->GetValue<std::string>(CacheEntry::OcrLanguageDefault, "", true) ==
                                                 g_config->File.ocr_language;
    m_inputs.ocr_language = use_last ? last : g_config->File.ocr_language;
    m_inputs.ocr_path     = use_last ? g_cache->GetValue<std::string>(CacheEntry::OcrLanguageFolder, "", true) : "";
    RefreshOcrModels();
    if (use_last && m_inputs.ocr_path.empty())
    {
        m_inputs.ocr_language = g_config->File.ocr_language;
        RefreshOcrModels();
    }

    m_inputs.ann_font               = g_config->File.fonts.empty() ? "" : g_config->File.fonts[0];
    m_inputs.resolved_ann_font_path = get_font_path(m_inputs.ann_font).string();

    SDL_SetRenderVSync(SDL_GetRenderer(m_backend.window), int(g_config->File.enable_vsync));
}

void ScreenshotTool::RefreshOcrModels()
{
    ErrorContext<OcrError>& ectx = m_ocr_errors;

    // Scanned every time, so languages installed while the tray runs show up on the next capture
    m_ocr_folders = g_config->GetOcrFolders();
    m_ocr_languages.clear();
    for (const std::string& folder : m_ocr_folders)
    {
        const size_t    first = m_ocr_languages.size();
        std::error_code ec;
        for (const fs::directory_entry& entry :
             fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec))
        {
            // osd and equ are for page orientation and equations, not languages
            const std::string code = entry.path().stem().string();
            if (entry.path().extension() != ".traineddata" || code == "osd" || code == "equ")
                continue;

            const auto name = std::ranges::find(OCR_LANGUAGE_NAMES, code, [](const auto& n) { return n.first; });
            m_ocr_languages.push_back(
                { .code   = code,
                  .folder = folder,
                  .label  = name != OCR_LANGUAGE_NAMES.end() ? fmt::format("{} ({})", _(name->second), code) : code });
        }
        std::ranges::sort(
            m_ocr_languages.begin() + static_cast<ptrdiff_t>(first), m_ocr_languages.end(), {}, &ocr_language_t::label);
    }

    if (m_ocr_languages.empty())
    {
        m_inputs.ocr_path.clear();
        ectx.Set(OcrError::InvalidPath);
        return;
    }
    ectx.Clear(OcrError::InvalidPath);

    // Keep the folder it was picked from while that still has it, else the first folder that does
    auto it = std::ranges::find_if(m_ocr_languages, [&](const ocr_language_t& l) {
        return l.code == m_inputs.ocr_language && l.folder == m_inputs.ocr_path;
    });
    if (it == m_ocr_languages.end())
        it = std::ranges::find(m_ocr_languages, m_inputs.ocr_language, &ocr_language_t::code);
    m_inputs.ocr_path = it != m_ocr_languages.end() ? it->folder : "";
    if (m_inputs.ocr_path.empty())
        ectx.Set(OcrError::InvalidModel);
    else
        ectx.Clear(OcrError::InvalidModel);
}

std::vector<std::pair<HandleHovered, ImVec2>> ScreenshotTool::GetSelectionHandles(const selection_info_t& sel) const
{
    const annotation_t* ann = sel.is_ann ? m_current_annotation_resize : nullptr;
    if (ann && (ann->type == ToolType::Line || ann->type == ToolType::Arrow))
        return { { HandleHovered::LineStart, ann->start }, { HandleHovered::LineEnd, ann->end } };

    const ImRect r = sel.selection.rect();

    // Corners first, they win where handles overlap
    std::vector<std::pair<HandleHovered, ImVec2>> handles = {
        { HandleHovered::TopLeft, r.GetTL() },
        { HandleHovered::TopRight, r.GetTR() },
        { HandleHovered::BottomLeft, r.GetBL() },
        { HandleHovered::BottomRight, r.GetBR() },
    };

    // Text scales with its font size, which only the corners change
    if (!ann || ann->type != ToolType::Text)
    {
        const ImVec2 c = r.GetCenter();
        handles.insert(handles.end(),
                       { { HandleHovered::Top, ImVec2(c.x, r.Min.y) },
                         { HandleHovered::Bottom, ImVec2(c.x, r.Max.y) },
                         { HandleHovered::Left, ImVec2(r.Min.x, c.y) },
                         { HandleHovered::Right, ImVec2(r.Max.x, c.y) } });
    }
    return handles;
}

void ScreenshotTool::UpdateHandleHoverState(selection_info_t& sel)
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    sel.handle_hover        = HandleHovered::kNone;

    const auto handles = GetSelectionHandles(sel);

    // Smaller on a small annotation, so its handles leave room to grab it in the middle
    float hover = 15.0f;
    if (sel.is_ann)
    {
        float extent = std::min(sel.selection.get_width(), sel.selection.get_height());
        if (handles.size() == 2)
            extent = std::hypot(handles[1].second.x - handles[0].second.x, handles[1].second.y - handles[0].second.y);
        hover = std::clamp(extent * 0.25f, 5.0f, 15.0f);
    }

    for (const auto& [type, pos] : handles)
    {
        if (ImRect(pos.x - hover, pos.y - hover, pos.x + hover, pos.y + hover).Contains(mouse_pos))
        {
            sel.handle_hover = type;
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

            case HandleHovered::LineStart:
            case HandleHovered::LineEnd:   ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;

            default: ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); break;
        }
    }
    else if (m_state == ToolState::Selected || m_state == ToolState::Resizing)
    {
        // Inside the selection, for moving it
        if (sel.selection.rect().Contains(ImGui::GetMousePos()) || m_hover)
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
            // Directional endpoints, not corners, so normalize them and pad by half the stroke
            // so thin or axis-aligned segments stay clickable
            const float half_t = std::max(ann.thickness, 4.0f) * 0.5f;
            return { std::min(ann.start.x, ann.end.x) - half_t,
                     std::min(ann.start.y, ann.end.y) - half_t,
                     std::max(ann.start.x, ann.end.x) + half_t,
                     std::max(ann.start.y, ann.end.y) + half_t };
        }

        case ToolType::Text:
        {
            ImFont* font = GetCachedFont(ann.font);
            if (font)
                ImFontAtlasBuildMain(ImGui::GetIO().Fonts);
            const ImVec2 size = font ? font->CalcTextSizeA(ann.thickness, FLT_MAX, 0.0f, ann.text.c_str())
                                     : ImGui::CalcTextSize(ann.text.c_str());
            return { ann.start, ann.start + size };
        }

        case ToolType::Pencil:
        {
            if (ann.points.empty())
                return { ann.start, ann.start };
            ImVec2 min = ann.points.front();
            ImVec2 max = min;
            for (const ImVec2& p : ann.points)
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
            // Opposite corners, in the order they were dragged
            return { std::min(ann.start.x, ann.end.x),
                     std::min(ann.start.y, ann.end.y),
                     std::max(ann.start.x, ann.end.x),
                     std::max(ann.start.y, ann.end.y) };
    }
}

ImRect ScreenshotTool::GetAnnotationHandleBox(const annotation_t& ann) const
{
    // A Text annotation's thickness is its font size
    const float  stroke  = ann.type == ToolType::Text ? 0.0f : ann.thickness * ImGui::GetIO().DisplayFramebufferScale.x;
    const float  padding = ann.type == ToolType::Text ? TEXT_HANDLE_PADDING : stroke * 0.5f + 3.0f;
    const ImRect bbox    = GetAnnotationBBox(ann);
    return { bbox.Min.x - padding, bbox.Min.y - padding, bbox.Max.x + padding, bbox.Max.y + padding };
}

ImVec2 ScreenshotTool::GetToolButtonSize()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return { TOOL_BUTTON_SIZE + style.FramePadding.x * 2.0f, TOOL_BUTTON_SIZE + style.FramePadding.y * 2.0f };
}

ImVec2 ScreenshotTool::GetToolbarRingOffset(const size_t ring)
{
    const ImVec2 btn     = GetToolButtonSize();
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    return { TOOLBAR_OFFSET + float(ring) * (btn.x + spacing.x), TOOLBAR_OFFSET + float(ring) * (btn.y + spacing.y) };
}

// ------------------------------
// Scans
// ------------------------------

void ScreenshotTool::RequestScan(ScanType type)
{
    if (g_config->File.render_anns && !m_annotations.empty() && m_backend.read_pixels)
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

    const Result<>& configure_res =
        m_inputs.ocr_path.empty()
            ? Result<>(Err(i18n::tr_fmt("No OCR language folder has '{}'", m_inputs.ocr_language)))
            : m_ocr_api.Configure(m_inputs.ocr_path.c_str(), m_inputs.ocr_language.c_str());
    if (!configure_res.ok())
    {
        spdlog::error("Failed to load the OCR model '{}' from '{}': {}",
                      m_inputs.ocr_language,
                      m_inputs.ocr_path,
                      configure_res.error_v());
        ectx.Set(OcrError::FailedToOCR, configure_res.error_v());
#ifndef DISABLE_PLUGINS
        NotifyPluginsOfOcr(nullptr);
#endif
        return;
    }

    const auto           start  = std::chrono::steady_clock::now();
    Result<ocr_result_t> result = m_ocr_api.ExtractTextCapture(img);
    if (!result.ok())
    {
        // Don't leave the previous selection's text under the error, where Copy Text would still copy it
        m_inputs.ocr_results = {};
        m_inputs.ocr_original.clear();
        spdlog::debug("OCR: {}", result.error_v());
        ectx.Set(OcrError::FailedToOCR, result.error_v());
#ifndef DISABLE_PLUGINS
        NotifyPluginsOfOcr(nullptr);
#endif
        return;
    }

    ectx.Clear(OcrError::FailedToOCR);
    m_inputs.ocr_seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    m_inputs.ocr_results = std::move(result.get());
#ifndef DISABLE_PLUGINS
    TransformText(m_inputs.ocr_results.data, m_inputs.ocr_original, OSHOT_TEXT_OCR);
    NotifyPluginsOfOcr(&m_inputs.ocr_results);
#endif
}

void ScreenshotTool::RunBarcodeScan(const capture_result_t& img)
{
    ErrorContext<BarcodeError>& ectx = m_barcode_errors;

    const auto               start = std::chrono::steady_clock::now();
    Result<barcode_result_t> scan  = m_barcode_api.ExtractTextsCapture(img);
    m_inputs.barcode_seconds       = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    if (!scan.ok())
    {
        // m_code_table.rows index into the codes, so they go with them
        m_inputs.barcode_scan_result = {};
        m_code_table.rows.clear();
        m_code_table.selected = -1;
        m_inputs.barcode_text.clear();
        m_inputs.barcode_original.clear();
        spdlog::debug("Barcode: {}", scan.error_v());
        ectx.Set(BarcodeError::FailedToScan, scan.error_v());
#ifndef DISABLE_PLUGINS
        for (auto& [id, rt] : g_plugins)
            if (OSHOT_PLUGIN_HAS(rt.plugin, on_barcodes_done) && rt.plugin->on_barcodes_done)
                rt.plugin->on_barcodes_done(rt.state, nullptr, 0);
#endif
        return;
    }

    ectx.Clear(BarcodeError::FailedToScan);
    m_inputs.barcode_scan_result = std::move(scan.get());
    m_inputs.barcode_scanned     = true;
    m_code_table.selected        = -1;

    // Each code once, in the order they were found, for the text tools' table
    const std::vector<barcode_t>& found = m_inputs.barcode_scan_result.codes;
    m_code_table.rows.clear();
    m_code_table.rows_width = 0.0f;
    for (size_t i = 0; i < found.size(); ++i)
        if (std::ranges::none_of(m_code_table.rows, [&](const code_row_t& row) {
                return found[row.code].type == found[i].type && found[row.code].data == found[i].data;
            }))
            m_code_table.rows.push_back({ .code = i, .text = {}, .lines = 0 });

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
