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

// Annotation tools and toolbar buttons, in toolbar order.
// UpdateAnnsToolbarPos() expects SaveImage to be the last button
enum class ToolType : std::uint8_t
{
    kNone,
    Arrow,
    Line,
    Rectangle,
    RectangleFilled,
    Circle,
    CircleFilled,
    CounterBubble,
    Text,
    Pencil,
    ToggleTextTools,
    CopyImage,
    SaveImage,
    Logo,  // not actually a tooltype
    COUNT
};

// The overlay's selection state machine
enum class ToolState : std::uint8_t
{
    Idle,
    Selecting,
    Selected,
    Resizing,
    AnnResizing,  // an annotation is selected, to move or resize it
};

// The handle of a selection under the cursor. Move is anywhere inside it
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

// Clock-wise order of where we place the
// annotations buttons
enum class SelectionBorder : std::uint8_t
{
    Bottom,
    Right,
    Top,
    Left,
    COUNT
};

// What a left click goes to, so a drag that started on one thing doesn't move another
enum class InputOwner : std::uint8_t
{
    kNone,
    Selection,
    AnnSelection,
    Tools
};

// Tabs of the Preferences window
enum class PrefTab : std::int8_t
{
    kNone    = -1,
    Defaults = 0,
#ifndef DISABLE_PLUGINS
    Plugins,
#endif
    Theme
};

// Windows drawn over the overlay, shown while their flag is set in m_show_window
enum class SubWindow : std::uint8_t
{
    OcrDownload,
    About,
    Preferences,
    MainTextTools,
    InstallPlugins,
    PluginInstallStatus,
    ManagePlugins,
    BrowsePlugins,
    Logs,
    OutputMenuSelection,
    COUNT
};

// Values of the color-picker-alpha-mode option
enum class ColorPickerAlpha : std::uint8_t
{
    Disabled,  // alpha channel is not editable
    Inline,    // alpha editable via the picker's inline slider
    Bar,       // alpha editable via a dedicated alpha bar, too
};

// A text tools scan waiting for a frame with the annotations, see RequestScan()
enum class ScanType : std::uint8_t
{
    kNone,
    Ocr,
    Barcode
};

// Interactions in progress, which e.g. keep Esc from cancelling the capture
enum class CurrentAction : std::uint8_t
{
    IsDrawing,
    IsColorPicking,
    IsTextPlacing,
    COUNT
};

// Errors shown in the windows, one enum per domain, used with ErrorContext
enum class OcrDownloadError : std::uint8_t
{
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

// A dragged rectangle. `start` is where the drag began, so it can be any corner:
// the getters give the normalized rectangle
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
    // Opposite corners. Circles and counter bubbles: the center and a point on
    // the circle. Lines and arrows: the endpoints, in drawing direction
    point_t start;
    point_t end;

    // Text tool
    std::string text;

    // CounterBubble tool: the number in the bubble
    unsigned int count = 0;

    // Pencil tool
    std::vector<point_t> points;

    // RGBA
    rgba_t color = rgba_t(0xFF0000FF);

    ToolType type      = ToolType::kNone;
    float    thickness = 3.0f;
};

// Contains user inputs, APIs results,
// and maybe some user settings.
// StartWindow() sets it with designated initializers, so keep the field order
struct inputs_results_t
{
    std::string  ocr_path;
    std::string  ocr_model;
    std::string  ocr_download_repo;
    std::string  ocr_model_downloaded_path;  // where the OCR model downloader saves models
    ocr_result_t ocr_results;

    std::string   barcode_text;
    zbar_result_t zbar_scan_result;

    std::string ann_font;
    std::string resolved_ann_font_path;  // ann_font found by get_font_path()
};

#ifndef DISABLE_PLUGINS
// One entry pushed by a PluginCallbacks::on_* call. Produced on the install
// worker thread, only ever read/cleared on the render thread.
struct plugin_install_event_t
{
    // DrawEventIcon() indexes its icons by this order
    enum class Kind : std::uint8_t
    {
        Status,
        Success,
        Warning,
        Error,
        Info,
        Output  // raw git/build output, appended to the running step
    };

    Kind        kind;
    std::string text;
};

// A node in the status window's tree. `in_progress` nodes are the ones a
// following non-Status event resolves in place instead of appending a new
// node for. Built with designated initializers, so keep the field order
struct install_node_t
{
    plugin_install_event_t::Kind kind;
    std::string                  text;
    std::vector<std::string>     details;
    bool                         in_progress = false;
    std::string                  output;  // build output printed while this step ran
};

// Shared between the render thread and the install worker thread for the
// duration of a single install operation. Owned via shared_ptr so the
// worker thread can finish writing to it even if the UI drops its own
// reference first (e.g. oshot is closed mid-install).
struct plugin_install_state_t
{
    // PluginCallbacks::on_* events, drained by DrawPluginInstallStatus()
    std::mutex                         events_mutex;
    std::deque<plugin_install_event_t> pending_events;

    // A PluginCallbacks::confirm question: the worker waits on confirm_cv
    // until the status window's popup answers it
    std::mutex              confirm_mutex;
    std::condition_variable confirm_cv;
    std::string             confirm_prompt;
    bool                    confirm_pending  = false;
    bool                    confirm_answered = false;
    bool                    confirm_answer   = false;

    std::atomic<bool> running{ true };
};

// The plugin index for the Browse plugins window, downloaded on a worker thread
struct plugin_index_state_t
{
    // Guards the fields below
    std::mutex mutex;

    // offline: the last downloaded copy
    bool stale = false;

    bool           loading = false;
    bool           loaded  = false;
    std::string    error;
    plugin_index_t index;
};
#endif

// Flags indexed by an enum class that ends with COUNT
template <typename Enum>
struct GeneralContext
{
    std::bitset<idx(Enum::COUNT)> flags;

    template <typename... E>
    [[nodiscard]] bool HasAny(E... e) const
    {
        return (Has(e) || ...);
    }

    void Set(Enum e, bool flag = true) { flags.set(idx(e), flag); }

    void Clear(Enum e) { flags.reset(idx(e)); }

    [[nodiscard]] bool Has(Enum e) const { return flags.test(idx(e)); }
};

// GeneralContext of errors, each with its message
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

// The overlay: selection, annotations, toolbar, text tools and the other windows.
// Backend-agnostic: main_tool_* owns the window and the textures, and talks to it
// through the Set* callbacks
class ScreenshotTool
{
public:
#ifndef DISABLE_PLUGINS
    ScreenshotTool(StateManager&& state) : m_plugin_manager(std::move(state), m_plugin_cb, /*is_cli=*/false) {}
    ~ScreenshotTool()
    {
        if (m_install_thread.joinable())
            m_install_thread.join();
        if (m_index_thread.joinable())
            m_index_thread.join();
    }
#endif

    // ------------------------------
    // Errors
    // ------------------------------
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

    // ------------------------------
    // Lifecycle
    // ------------------------------
    // Takes the screenshot, or loads Runtime.source_file. Called before the window
    // is created, so the overlay isn't in it
    Result<> Start();

    // Sets up the overlay once the window and graphics context exist
    Result<> StartWindow();

    // Draws a frame of the overlay and handles its input
    void RenderOverlay();

    // Ends the capture: frees the textures and calls the SetOnCancel() callback
    void Cancel();

    // Frees the OCR engine. Must run before the process exits, see OcrAPI::End()
    void EndOcr() { m_ocr_api.End(); }

    // Replaces the screenshot with an image file and starts a new selection.
    // Shows an error dialog on failure
    bool OpenImage(const std::string& path);

    // Crops the whole-desktop screenshot to the Wayland output picked in
    // DrawOutputMenuSelection(), and undoes the output's rotation (`transform`, 0-3 quarter turns)
    Result<> CropToOutput(const std::deque<region_t>& layout, const monitor_t& target, int transform = 0);

    // ------------------------------
    // Backend setters
    // ------------------------------
    // Called by OnCleanFrame() with what to do, the active region with its annotations, and the image format.
    // Returns false if it failed: the overlay then stays open, so the user can try again
    void SetOnComplete(std::function<bool(SavingOp, const capture_result_t&, ImageExt)> cb)
    {
        m_on_complete = std::move(cb);
    }

    // Called by OpenImage() on macOS, so the backend recreates the screenshot texture
    void SetOnImageReload(std::function<void(const capture_result_t&)> fn) { m_on_image_reload = std::move(fn); }

    // Reads a region (in screenshot pixels) of the frame the backend just rendered
    void SetReadPixels(std::function<capture_result_t(const region_t&)> fn) { m_read_pixels = std::move(fn); }

    // The texture of the screenshot, made by the Metal backend
    void SetBackendTexture(void* tex) { m_texture_id._TexID = static_cast<ImTextureID>(size_t(tex)); }

    // The texture of a toolbar icon, made by the Metal backend
    void SetToolTexture(ToolType type, void* tex)
    {
        m_tool_textures[idx(type)]._TexID = static_cast<ImTextureID>(size_t(tex));
    }

    // Draw the screenshot at its full resolution on a scaled output, instead of
    // shrinking it to the logical screen size. The backend must then read back
    // framebuffer pixels at GetImageScale().
    void SetFullResolutionImage(bool enable) { m_full_res_image = enable; }

    void SetOnCancel(const std::function<void()>& cb) { m_on_cancel = cb; }

    // ------------------------------
    // Frame read-back
    // ------------------------------
    // Marks that the user asked to save/copy; the actual work is deferred
    // until OnCleanFrame() is called after the frame has been rendered.
    void RequestComplete(SavingOp op) { m_completed_op = op; }

    // Whether the next frame must be drawn with only the screenshot and the annotations,
    // for OnCleanFrame() to read back. Such a frame isn't presented
    [[nodiscard]] bool WantsCleanFrame() const { return IsCompleted() || m_pending_scan != ScanType::kNone; }

    // Called by the backend once the frame WantsCleanFrame() asked for has been
    // rendered: reads the active region from it, for the pending scan and save/copy
    void OnCleanFrame();

    // ------------------------------
    // Getters
    // ------------------------------
    // Top-left corner of the screenshot on screen: it's drawn centered
    const point_t GetImageOrigin() const { return { .x = m_image_origin.x, .y = m_image_origin.y }; }

    // Screenshot pixels per screen unit: above 1 on a scaled (HiDPI) output when
    // the backend renders at full resolution, see SetFullResolutionImage()
    [[nodiscard]] float GetImageScale() const { return m_image_scale; }

    // A copy of the active region of the screenshot, without annotations
    capture_result_t GetFinalImage();

    // The selection in screenshot pixels, or the whole screenshot without one
    [[nodiscard]] region_t GetActiveRegion() const;

    // Text buffers plugins read and write by ID (OSHOT_TEXT_OCR, OSHOT_TEXT_BARCODE)
    auto& GetImGuiIDTexts() { return m_imgui_id_texts; }

    [[nodiscard]] bool IsActive() const { return m_state != ToolState::Idle; }
    [[nodiscard]] bool IsCompleted() const { return m_completed_op != SavingOp::kNone; }
    capture_result_t&  GetRawScreenshot() { return m_screenshot; }

    // ------------------------------
    // Textures, fonts and style
    // ------------------------------
    // Uploads RGBA pixels to a new OpenGL texture, deleting `tex` first.
    // Returns an empty one on macOS, where the Metal backend makes them
    Result<ImTextureRef> CreateTexture(ImTextureID tex, std::span<const uint8_t> data, int w, int h);

    // Fonts by path and size, added to the atlas the first time. Sizes under 16 share one entry
    ImFont* CacheAndGetFont(const std::string& font_name, const float font_size);

    // The default font for an empty path, nullptr if CacheAndGetFont() didn't load it yet
    [[nodiscard]] ImFont* GetCachedFont(const std::string& font_path, const float font_size) const;

    // Applies oshot's default ImGui colors
    static void StyleDefaultColor();

private:
    // Gap between the selection border and the toolbar
    static constexpr float TOOLBAR_OFFSET = 10.0f;

    static constexpr int    TOOL_BUTTON_SIZE = 24;
    static constexpr ImVec2 TOOL_BUTTON_SIZE2{ TOOL_BUTTON_SIZE, TOOL_BUTTON_SIZE };

    // An OCR model download on a worker thread, shared with it
    struct ocr_download_t
    {
        // -1 = indeterminate
        std::atomic<float> progress{ -1.f };

        // accumulates partial stderr lines
        std::string line_buf;

        // smoothed, main thread only
        float display_progress{ 0.f };

        std::atomic<bool> running{ true };
        std::atomic<int>  exit_code{ -1 };
        std::mutex        err_mutex;
        std::string       err;
    };

    // A selection and its drag state: one for the main selection, one for the
    // annotation being moved or resized
    struct selection_info_t
    {
        // Snapshot of a Pencil annotation's points taken right before a drag
        // starts. HandleAnnotationSelectionInput() scales from it, so the
        // transform doesn't compound frame after frame
        std::vector<point_t> drag_start_points;

        // Line/Arrow: keep start/end as they are, they give the direction
        bool preserve_direction = false;

        const bool       is_ann;
        const InputOwner main_input_owner;
        const ToolState  resizing_state;
        const ToolState  idle_state;

        HandleHovered    handle_hover    = HandleHovered::kNone;
        HandleHovered    dragging_handle = HandleHovered::kNone;
        selection_rect_t selection;
        selection_rect_t drag_start_selection;
        ImVec2           drag_start_mouse;

        selection_info_t(const bool b, const InputOwner o, const ToolState r, const ToolState i)
            : is_ann(b), main_input_owner(o), resizing_state(r), idle_state(i)
        {}
    };

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

    // ------------------------------
    // Screenshot
    // ------------------------------
    // The whole-desktop capture at full resolution while the output picker is
    // open, so the chosen monitor is cropped from it and not from the shrunk copy
    capture_result_t m_uncropped;

    // Where the screenshot is drawn (centered), in screen units. See UpdateWindowBg()
    ImVec2 m_image_origin;
    ImVec2 m_image_end;

    // Outputs for the output picker (Wayland)
    std::deque<monitor_t> m_wayland_monitors;

    capture_result_t m_screenshot;
    ImTextureRef     m_texture_id;
    SessionType      m_session;
    float            m_image_scale    = 1.0f;
    bool             m_full_res_image = false;

    // ------------------------------
    // Selection
    // ------------------------------
    ToolState                     m_state       = ToolState::Idle;
    InputOwner                    m_input_owner = InputOwner::kNone;
    GeneralContext<CurrentAction> m_current_actions;

    selection_info_t m_main_sel{ false, InputOwner::Selection, ToolState::Resizing, ToolState::Selected };
    selection_info_t m_anno_sel{ true, InputOwner::AnnSelection, ToolState::AnnResizing, ToolState::AnnResizing };

    // ------------------------------
    // Annotations and toolbar
    // ------------------------------
    // The annotation selected for moving or resizing, in m_annotations
    annotation_t* m_current_annotation_resize = nullptr;

    // Toolbar buttons on each border of the selection, from UpdateAnnsToolbarPos()
    std::array<std::vector<ToolType>, idx(SelectionBorder::COUNT)> m_tools_borders;

    // All the toolbar buttons fit around the selection
    bool m_fill_tools_complete{};

    std::vector<annotation_t>                      m_annotations;
    annotation_t                                   m_current_annotation;
    ToolType                                       m_current_tool = ToolType::kNone;
    rgba_t                                         m_current_color;
    std::array<float, idx(ToolType::COUNT)>        m_tool_thickness;
    std::array<ImTextureRef, idx(ToolType::COUNT)> m_tool_textures;

    // ------------------------------
    // Text tools
    // ------------------------------
    // Set by RequestScan(), until OnCleanFrame()
    ScanType m_pending_scan = ScanType::kNone;

    // Text buffers by ID, see GetImGuiIDTexts()
    std::unordered_map<std::string, std::string*> m_imgui_id_texts;

    // The ocr_path RefreshOcrModels() last scanned
    std::string m_last_scanned_ocr_path;

    OcrAPI                          m_ocr_api;
    ZbarAPI                         m_zbar_api;
    inputs_results_t                m_inputs;
    std::shared_ptr<ocr_download_t> m_ocr_download;
    std::vector<std::string>        m_ocr_models_list;
    ErrorContext<OcrError>          m_ocr_errors;
    ErrorContext<ZbarError>         m_zbar_errors;
    ErrorContext<OcrDownloadError>  m_download_errors;
    ErrorContext<GeneralError>      m_general_errors;

    // ------------------------------
    // Windows and fonts
    // ------------------------------
    GeneralContext<SubWindow>                             m_show_window;
    std::map<std::pair<std::string, float>, font_cache_t> m_font_cache;

    // A menu or popup was open at the end of the last frame, see RenderOverlay()
    bool m_popup_open = false;

    // ------------------------------
    // Completion and backend callbacks
    // ------------------------------
    // Set by RequestComplete(), until OnCleanFrame()
    SavingOp m_completed_op = SavingOp::kNone;

    std::function<void()>                                            m_on_cancel;
    std::function<void(const capture_result_t&)>                     m_on_image_reload;
    std::function<bool(SavingOp, const capture_result_t&, ImageExt)> m_on_complete;
    std::function<capture_result_t(const region_t&)>                 m_read_pixels;

#ifndef DISABLE_PLUGINS
    // ------------------------------
    // Plugins
    // ------------------------------
    // m_plugin_cb must be declared (and therefore constructed) before
    // m_plugin_manager, which the constructor initializes with a copy of it
    PluginCallbacks m_plugin_cb;
    PluginManager   m_plugin_manager;

    // text field backing the install window
    std::string m_install_source;

    // The running plugin task (StartPluginTask()), shared with m_install_thread
    std::shared_ptr<plugin_install_state_t> m_install_state;

    // The status window's tree, built from m_install_state's events
    std::vector<install_node_t> m_install_events;

    std::shared_ptr<plugin_index_state_t> m_index_state = std::make_shared<plugin_index_state_t>();
    std::thread                           m_install_thread;
    std::thread                           m_index_thread;
    std::string                           m_index_filter;
#endif

    // ------------------------------
    // Errors
    // ------------------------------
    // Shows the error in red if it's set, and returns whether it is
    template <typename Enum>
    bool ShowIfError(const ErrorContext<Enum>& ctx, Enum e)
    {
        bool has = ctx.Has(e);
        if (has)
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", ctx.Get(e).c_str());
        return has;
    }

    // ------------------------------
    // Input
    // ------------------------------
    // Returns true when the click only deselected an annotation: the main
    // selection must then ignore it this frame
    bool HandleAnnotationSelectionInput();

    void HandleShortcutsInput();
    void HandleSelectionInput(selection_info_t& sel);
    void HandleResizeInput(selection_info_t& sel);
    void HandleAnnotationInput();
    void HandleColorPickerInput();

    // ------------------------------
    // State updates
    // ------------------------------
    // Centers the screenshot in the viewport: sets m_image_origin, m_image_end and m_image_scale
    void UpdateWindowBg();

    // Spreads the toolbar buttons over the selection's borders
    void UpdateAnnsToolbarPos();

    // Swaps the selection's start and end so start is the top-left corner
    void NormalizeSelection(selection_info_t& sel);

    // Drops the main selection, its drag state and the current tool
    void ResetSelection();

    // Copies the config into m_inputs, after it was loaded or changed in Preferences
    void SyncRuntimeFromConfig();

    // Lists the models in the OCR path, if it changed since the last scan
    void RefreshOcrModels();

    void UpdateHandleHoverState(selection_info_t& sel);
    void UpdateCursor(const selection_info_t& sel);

    // ------------------------------
    // Geometry
    // ------------------------------
    // The annotation's bounds on screen
    [[nodiscard]] ImRect GetAnnotationBBox(const annotation_t& ann) const;

    // GetAnnotationBBox() expanded by the same border padding DrawAnnotationResizeBorder()
    // draws with, so the resize handles you see and the ones you can actually
    // click line up exactly. Only used for Circle/CircleFilled/CounterBubble/Text;
    // Line/Arrow keep their raw, un-normalized start/end to preserve direction.
    [[nodiscard]] ImRect GetAnnotationHandleBox(const annotation_t& ann) const;

    ImVec2 GetToolButtonSize();

    // ------------------------------
    // Scans
    // ------------------------------
    // Runs an OCR or barcode scan of the active region. With the annotations-in-text-tools
    // option and annotations drawn, it waits for OnCleanFrame() to read them back with the image
    void RequestScan(ScanType type);

    // Scan `img` and show the results in the text tools
    void RunOcr(const capture_result_t& img);
    void RunBarcodeScan(const capture_result_t& img);

    // ------------------------------
    // Drawing
    // ------------------------------
    // Special util draw function, used in DrawAnnotationResizeBorder() and DrawSelectionBorder()
    void DrawASelectionBorder(selection_info_t& sel,
                              const float       sel_x,
                              const float       sel_y,
                              const float       sel_w,
                              const float       sel_h);

    // A button that copies `text_copy` and says "Copied!" for a moment
    void CreateCopyTextButton(const std::string& text_copy, const std::string_view label1 = "Copy Text");

    void DrawDarkOverlay();
    void DrawAnnotations();
    void DrawSelectionBorder();
    void DrawAnnotationResizeBorder();
    void DrawAnnotationToolbar();
    void DrawMenuItems();
    void DrawOcrTools();
    void DrawBarDecodeTools();

    // ------------------------------
    // Windows
    // ------------------------------
    void DrawAboutWindow();
    void DrawPreferencesWindow();
    void DrawDownloadOCRWindow();
    void DrawLogsWindow();
    void DrawOutputMenuSelection();

#ifndef DISABLE_PLUGINS
    // ------------------------------
    // Plugins
    // ------------------------------
    // Downloads the plugin index into m_index_state, on m_index_thread
    void FetchIndexAsync();

    // Runs a PluginManager operation (install, update, uninstall) on a
    // background thread. Wires fresh PluginCallbacks around a new
    // plugin_install_state_t so the render thread can drain progress from
    // it without touching ImGui from off the render thread.
    void StartPluginTask(std::string title, std::function<Result<>(PluginManager&)> task);

    void DrawManagePluginsWindow();
    void DrawBrowsePluginsWindow();
    void DrawInstallPluginsWindow();
    void DrawPluginInstallStatus();
    void DrawEventIcon(plugin_install_event_t::Kind kind);
#endif
};

// The overlay of this process
extern ScreenshotTool g_ss_tool;

// Files dropped on the window, picked up by the path field under the cursor
extern std::deque<std::string> g_dropped_paths;

#endif  // !_SCREENSHOT_TOOL_HPP_
