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

#ifndef _SCREENSHOT_TOOL_HPP_
#define _SCREENSHOT_TOOL_HPP_

#include <algorithm>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "config.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "screen_capture.hpp"
#include "text_extraction.hpp"
#include "util.hpp"

#ifndef DISABLE_PLUGINS
#  include "plugin_manager.hh"
#endif

enum class ToolType : std::uint8_t
{
    kNone,
    Arrow,
    Rectangle,
    RectangleFilled,
    Circle,
    CircleFilled,
    CounterBubble,
    Line,
    Text,
    Pencil,
    ToggleTextTools,
    CopyImage,
    SaveImage,
    Logo,  // not actually a tooltype
    COUNT
};

enum class ToolState : std::uint8_t
{
    Idle,
    Capturing,
    Selecting,
    Selected,
    Resizing,
    AnnResizing,
};

enum class HandleHovered : std::uint8_t
{
    kNone,
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Move
};

enum class InputOwner : std::uint8_t
{
    kNone,
    Selection,
    AnnSelection,
    Tools
};

enum class PrefTab : std::int8_t
{
    kNone    = -1,
    Defaults = 0,
#ifndef DISABLE_PLUGINS
    Plugins,
#endif
    Theme
};

enum class SubWindow : std::uint8_t
{
    OcrDownload,
    About,
    Preferences,
    MainTextTools,
    InstallPlugins,
    PluginInstallStatus,
    ManagePlugins,
    UninstallPlugins,
    Logs,
    OutputMenuSelection,
    COUNT
};

// Used for config.hpp
enum class ColorPickerAlpha : std::uint8_t
{
    Disabled,  // alpha channel is not editable
    Inline,    // alpha editable via the picker's inline slider
    Bar,       // alpha editable via a dedicated alpha bar, too
};

enum class CurrentAction : std::uint8_t
{
    IsDrawing,
    IsColorPicking,
    IsTextPlacing,
    COUNT
};

enum class OcrDownloadError : std::uint8_t
{
    InvalidRepo,
    InvalidPath,
    FailedToDownload,
    COUNT
};

enum class OcrError : std::uint8_t
{
    InvalidModel,
    InvalidPath,
    NeedToScanDir,
    FailedToOCR,
    COUNT
};

enum class ZbarError : std::uint8_t
{
    FailedToScan,
    COUNT,
};

enum class GeneralError : std::uint8_t
{
    FailedToCopyText,
    COUNT,
};

struct point_t
{
    float x{};
    float y{};
};

struct selection_rect_t
{
    point_t start;
    point_t end;

    [[nodiscard]] float get_x() const { return std::min(start.x, end.x); }
    [[nodiscard]] float get_y() const { return std::min(start.y, end.y); }
    [[nodiscard]] float get_width() const { return std::abs(end.x - start.x); }
    [[nodiscard]] float get_height() const { return std::abs(end.y - start.y); }
};

struct annotation_t
{
    ToolType             type = ToolType::kNone;
    point_t              start;
    point_t              end;
    std::string          text;                            // For text tool
    unsigned int         count = 0;                       // For CounterBubble tool
    std::vector<point_t> points;                          // For pencil tool
    rgba_t               color     = rgba_t(0xFF0000FF);  // RGBA
    float                thickness = 3.0f;
};

// Contains user inputs, APIs results,
// and maybe some user settings
struct inputs_results_t
{
    std::string  ocr_path;
    std::string  ocr_model;
    std::string  ocr_download_repo;
    std::string  ocr_model_downloaded_path;
    ocr_result_t ocr_results;

    std::string   barcode_text;
    zbar_result_t zbar_scan_result;

    std::string ann_font;
    std::string resolved_ann_font_path;
};

#ifndef DISABLE_PLUGINS
// One entry pushed by a PluginCallbacks::on_* call. Produced on the install
// worker thread, only ever read/cleared on the render thread.
struct plugin_install_event_t
{
    enum class Kind : std::uint8_t
    {
        Status,
        Success,
        Warning,
        Error,
        Info
    };

    Kind        kind;
    std::string text;
};

// A node in the status window's tree. `in_progress` nodes are the ones a
// following non-Status event resolves in place instead of appending a new
// node for.
struct install_node_t
{
    plugin_install_event_t::Kind kind;
    std::string                  text;
    std::vector<std::string>     details;
    bool                         in_progress;
};

// Shared between the render thread and the install worker thread for the
// duration of a single install operation. Owned via shared_ptr so the
// worker thread can safely finish writing to it even if the UI tears down
// its own reference first (e.g. the person closes the app mid-install).
struct plugin_install_state_t
{
    std::atomic<bool> running{ true };

    std::mutex                         events_mutex;
    std::deque<plugin_install_event_t> pending_events;

    std::mutex              confirm_mutex;
    std::condition_variable confirm_cv;
    std::string             confirm_prompt;
    bool                    confirm_pending  = false;
    bool                    confirm_answered = false;
    bool                    confirm_answer   = false;
};
#endif

template <typename Enum>
struct GeneralContext
{
    std::bitset<idx(Enum::COUNT)> flags;

    void Set(Enum e, bool flag = true) { flags.set(idx(e), flag); }

    void Clear(Enum e) { flags.reset(idx(e)); }

    [[nodiscard]] bool Has(Enum e) const { return flags.test(idx(e)); }

    template <typename... E>
    bool HasAny(E... e) const
    {
        return (Has(e) || ...);
    }
};

template <typename Enum>
struct ErrorContext : public GeneralContext<Enum>
{
    std::array<std::string, idx(Enum::COUNT)> texts;

    void Set(Enum e, std::string_view msg = {})
    {
        GeneralContext<Enum>::Set(e);
        texts[idx(e)] = msg;
    }

    void Clear(Enum e)
    {
        GeneralContext<Enum>::Clear(e);
        texts[idx(e)].clear();
    }

    [[nodiscard]] const std::string& Get(Enum e) const { return texts[idx(e)]; }
};

class ScreenshotTool
{
public:
#ifndef DISABLE_PLUGINS
    ScreenshotTool(StateManager&& state) : m_plugin_manager(std::move(state), m_plugin_cb, /*is_cli=*/false) {}
    ~ScreenshotTool()
    {
        if (m_install_thread.joinable())
            m_install_thread.join();
    }
#endif

    Result<>             Start();
    Result<>             StartWindow();
    Result<ImTextureRef> CreateTexture(ImTextureID tex, std::span<const uint8_t> data, int w, int h);
    bool                 OpenImage(const std::string& path);
    Result<>             CropToOutput(const std::deque<region_t>& layout, const monitor_t& target, int transform = 0);
    [[nodiscard]] bool   IsActive() const { return m_state != ToolState::Idle; }
    capture_result_t&    GetRawScreenshot() { return m_screenshot; }
    const point_t        GetImageOrigin() const { return { .x = m_image_origin.x, .y = m_image_origin.y }; }
    void                 SetBackendTexture(void* tex) { m_texture_id._TexID = static_cast<ImTextureID>(size_t(tex)); }
    void                 SetToolTexture(ToolType type, void* tex)
    {
        m_tool_textures[idx(type)]._TexID = static_cast<ImTextureID>(size_t(tex));
    }

    auto&                     GetImGuiIDTexts() { return m_imgui_id_texts; }
    [[nodiscard]] const auto& GetImageTexture() const { return m_texture_id; }

    void SetOnImageReload(std::function<void(const capture_result_t&)> fn) { m_on_image_reload = std::move(fn); }

    capture_result_t       GetFinalImage(bool is_text_tools = false);
    [[nodiscard]] region_t GetActiveRegion() const;

    ImFont*               CacheAndGetFont(const std::string& font_name, const float font_size);
    [[nodiscard]] ImFont* GetCachedFont(const std::string& font_path, const float font_size) const;

    void        RenderOverlay();
    void        Cancel();
    static void StyleDefaultColor();

    template <typename Enum>
    void SetError(ErrorContext<Enum>& ctx, Enum e, const std::string_view err = "")
    {
        ctx.Set(e, err);
    }

    template <typename Enum>
    const std::string& GetError(const ErrorContext<Enum>& ctx, Enum e) const
    {
        return ctx.Get(e);
    }

    template <typename Enum>
    void ClearError(ErrorContext<Enum>& ctx, Enum e)
    {
        ctx.Clear(e);
    }

    template <typename Enum>
    bool HasError(const ErrorContext<Enum>& ctx, Enum e) const
    {
        return ctx.Has(e);
    }

    void SetOnComplete(const auto& cb) { m_on_complete = cb; }

    // Marks that the user asked to save/copy; the actual work is deferred
    // until FireOnComplete() is called after the frame has been rendered.
    void RequestComplete(SavingOp op) { m_completed_op = op; }

    [[nodiscard]] bool IsCompleted() const { return m_completed_op != SavingOp::kNone; }

    void FireOnComplete()
    {
        if (m_on_complete)
            m_on_complete(m_completed_op, GetActiveRegion(), g_config->File.image_out_type.second);
        m_completed_op = SavingOp::kNone;
    }

    void SetOnCancel(const std::function<void()>& cb) { m_on_cancel = cb; }

private:
    struct font_cache_t
    {
        std::string font_path;
        ImFont*     font   = nullptr;
        bool        loaded = false;
    };

    struct handle_info_t
    {
        HandleHovered type;
        ImVec2        pos;
        ImRect        rect;
    };

    struct ocr_download_t
    {
        std::atomic<bool>  running{ true };
        std::atomic<float> progress{ -1.f };  // -1 = indeterminate
        std::atomic<int>   exit_code{ -1 };
        std::mutex         err_mutex;
        std::string        err;
        std::string        line_buf;                 // accumulates partial stderr lines
        float              display_progress{ 0.f };  // smoothed, main thread only
    };

    struct selection_info_t
    {
        const bool       is_ann;
        const InputOwner main_input_owner;
        const ToolState  resizing_state;
        const ToolState  idle_state;

        HandleHovered    handle_hover    = HandleHovered::kNone;
        HandleHovered    dragging_handle = HandleHovered::kNone;
        selection_rect_t selection;
        selection_rect_t drag_start_selection;
        ImVec2           drag_start_mouse;
        bool             preserve_direction = false;

        // Snapshot of a Pencil annotation's points taken right before a drag
        // starts, used as the immutable baseline for the scale transform in
        // HandleAnnotationSelectionInput() (never overwritten mid-drag, to
        // avoid compounding the transform frame after frame).
        std::vector<point_t> drag_start_points;

        selection_info_t(const bool b, const InputOwner o, const ToolState r, const ToolState i)
            : is_ann(b), main_input_owner(o), resizing_state(r), idle_state(i)
        {}
    };

    OcrAPI           m_ocr_api;
    ZbarAPI          m_zbar_api;
    capture_result_t m_screenshot;

    ImTextureRef m_texture_id;
    ToolState    m_state = ToolState::Idle;

    InputOwner m_input_owner = InputOwner::kNone;

    ErrorContext<OcrDownloadError> m_download_errors;
    ErrorContext<OcrError>         m_ocr_errors;
    ErrorContext<ZbarError>        m_zbar_errors;
    ErrorContext<GeneralError>     m_general_errors;

    inputs_results_t m_inputs;
    selection_info_t m_main_sel{ false, InputOwner::Selection, ToolState::Resizing, ToolState::Selected };
    selection_info_t m_anno_sel{ true, InputOwner::AnnSelection, ToolState::AnnResizing, ToolState::AnnResizing };

    ImVec2 m_image_origin;
    ImVec2 m_image_end;

    std::shared_ptr<ocr_download_t>                       m_ocr_download;
    std::vector<std::string>                              m_ocr_models_list;
    std::map<std::pair<std::string, float>, font_cache_t> m_font_cache;
    std::function<void()>                                 m_on_cancel;
    std::function<void(const capture_result_t&)>          m_on_image_reload;
    SavingOp                                              m_completed_op = SavingOp::kNone;

    std::function<void(SavingOp, const region_t&, ImageExt)> m_on_complete;

    std::deque<monitor_t>                          m_wayland_monitors;
    SessionType                                    m_session;
    std::string                                    m_last_scanned_ocr_path;
    GeneralContext<SubWindow>                      m_show_window;
    GeneralContext<CurrentAction>                  m_current_actions;
    std::array<ImTextureRef, idx(ToolType::COUNT)> m_tool_textures;
    ToolType                                       m_current_tool = ToolType::kNone;
    std::vector<annotation_t>                      m_annotations;
    annotation_t                                   m_current_annotation;
    annotation_t*                                  m_current_annotation_resize = nullptr;
    rgba_t                                         m_current_color;
    std::unordered_map<std::string, std::string*>  m_imgui_id_texts;
    std::array<float, idx(ToolType::COUNT)>        m_tool_thickness;

#ifndef DISABLE_PLUGINS
    // m_plugin_cb must be declared (and therefore constructed) before
    // m_plugin_manager: the constructor initializes m_plugin_manager with a
    // copy of m_plugin_cb, so the reverse order would copy a not-yet-
    // constructed PluginCallbacks.
    PluginCallbacks m_plugin_cb;
    PluginManager   m_plugin_manager;

    std::string                             m_install_source;  // text field backing the install window
    std::thread                             m_install_thread;
    std::shared_ptr<plugin_install_state_t> m_install_state;
    std::vector<install_node_t>             m_install_events;
#endif

    void CreateCopyTextButton(const std::string& text_copy, const std::string_view label1 = "Copy Text");
    void RefreshOcrModels();
    void NormalizeSelection(selection_info_t& sel);
    void SyncRuntimeFromConfig();

    void HandleShortcutsInput();
    bool HandleAnnotationSelectionInput();
    void HandleSelectionInput(selection_info_t& sel);
    void HandleResizeInput(selection_info_t& sel);
    void HandleAnnotationInput();
    void HandleColorPickerInput();

    void DrawDarkOverlay();
    void DrawAnnotations();
    void DrawMenuItems();
    void DrawSelectionBorder();
    void DrawAnnotationResizeBorder();

    // Special util draw function, used in DrawAnnotationResizeBorder() and DrawSelectionBorder()
    void DrawASelectionBorder(selection_info_t& sel,
                              const float       sel_x,
                              const float       sel_y,
                              const float       sel_w,
                              const float       sel_h);

    void DrawOcrTools();
    void DrawBarDecodeTools();
    void DrawAboutWindow();
    void DrawAnnotationToolbar();
    void DrawPreferencesWindow();
    void DrawDownloadOCRWindow();
    void DrawLogsWindow();

    void DrawOutputMenuSelection();

#ifndef DISABLE_PLUGINS
    void DrawManagePluginsWindow();
    void DrawInstallPluginsWindow();
    void DrawPluginInstallStatus();
    void DrawUninstallPluginsWindow();
    void DrawEventIcon(plugin_install_event_t::Kind kind);

    // Kicks off `source` (git URL / local folder / archive path) on a
    // background thread. Wires fresh PluginCallbacks around a new
    // plugin_install_state_t so the render thread can drain progress from
    // it without touching ImGui from off the render thread.
    void StartInstall(const std::string& source);
#endif

    void UpdateHandleHoverState(selection_info_t& sel);
    void UpdateCursor(const selection_info_t& sel);
    void UpdateWindowBg();

    [[nodiscard]] ImRect GetAnnotationBBox(const annotation_t& ann) const;

    // GetAnnotationBBox() expanded by the same border padding DrawAnnotationResizeBorder()
    // draws with, so the resize handles you see and the ones you can actually
    // click line up exactly. Only used for Circle/CircleFilled/CounterBubble/Text;
    // Line/Arrow keep their raw, un-normalized start/end to preserve direction.
    [[nodiscard]] ImRect GetAnnotationHandleBox(const annotation_t& ann) const;

    template <typename Enum>
    bool ShowIfError(const ErrorContext<Enum>& ctx, Enum e)
    {
        bool has = ctx.Has(e);
        if (has)
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", ctx.Get(e).c_str());
        return has;
    }
};

extern ScreenshotTool          g_ss_tool;
extern std::deque<std::string> g_dropped_paths;

#endif  // !_SCREENSHOT_TOOL_HPP_
