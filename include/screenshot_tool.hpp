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
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "screen_capture.hpp"
#include "text_extraction.hpp"
#include "util.hpp"
#include "util/color.hpp"
#include "util/image.hpp"
#include "util/result.hpp"

#ifndef DISABLE_PLUGINS
#  include "plugin_index.hh"
#  include "plugin_manager.hh"

class StateManager;
struct plugin_runtime_t;
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
    Move,
    LineStart,  // a Line/Arrow annotation's endpoints
    LineEnd
};

// The selection's borders the toolbar buttons go on, clockwise, in the order they're filled
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

// The overlay's on/off state, in ScreenshotTool::m_flags. Start() resets it for each capture
enum class CaptureFlag : std::uint8_t
{
    ToolbarFits,          // all the toolbar buttons fit around the selection
    AnnPopupPlaced,       // the annotation settings popup was placed with last frame's size
    FocusTextInput,       // focus the text input on its next frame
    WrapText,             // wrap the lines of the OCR and barcode text boxes
    PopupOpen,            // a menu or popup was open at the end of the last frame, see RenderOverlay()
    ItemActive,           // an item was active at the end of the last frame
    Esc,                  // this frame's Esc, until a sub-window or RenderOverlay() uses it
    FinalImageRequested,  // set by RequestFinalImage(), until OnCleanFrame()
    ImageCopied,          // the overlay closed after copying the image

    // The text tools show the text from before the plugins' transformers
    ShowOcrOriginal,
    ShowBarcodeOriginal,
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
    FailedToOCR,
    COUNT
};

enum class BarcodeError : std::uint8_t
{
    FailedToScan,
    COUNT,
};

enum class GeneralError : std::uint8_t
{
    FailedToCopyText,
    COUNT,
};

// A dragged rectangle. `start` is where the drag began, so it can be any corner, and the getters give the
// normalized rectangle
struct selection_rect_t
{
    ImVec2 start;
    ImVec2 end;

    [[nodiscard]] float get_x() const { return std::min(start.x, end.x); }
    [[nodiscard]] float get_y() const { return std::min(start.y, end.y); }
    [[nodiscard]] float get_right() const { return std::max(start.x, end.x); }
    [[nodiscard]] float get_bottom() const { return std::max(start.y, end.y); }
    [[nodiscard]] float get_width() const { return std::abs(end.x - start.x); }
    [[nodiscard]] float get_height() const { return std::abs(end.y - start.y); }
    [[nodiscard]] bool  is_empty() const { return start.x == end.x || start.y == end.y; }

    [[nodiscard]] ImRect rect() const { return { get_x(), get_y(), get_right(), get_bottom() }; }

    // Makes `start` the top-left corner
    void normalize()
    {
        if (start.x > end.x)
            std::swap(start.x, end.x);
        if (start.y > end.y)
            std::swap(start.y, end.y);
    }

    void translate(const float dx, const float dy)
    {
        start.x += dx;
        end.x += dx;
        start.y += dy;
        end.y += dy;
    }

    static selection_rect_t from_rect(const ImRect& r) { return { .start = r.Min, .end = r.Max }; }
};

struct annotation_t
{
    // Opposite corners. For circles and counter bubbles it's the center and a point
    // on the circle; for lines and arrows, the endpoints in drawing direction
    ImVec2 start;
    ImVec2 end;

    // Text tool, and the font file it's drawn with (empty for the interface font)
    std::string text;
    std::string font;

    // CounterBubble tool: the number in the bubble
    unsigned int count = 0;

    // Pencil tool
    std::vector<ImVec2> points;

    // RGBA
    rgba_t color = Colors::RED;

    ToolType type      = ToolType::kNone;
    float    thickness = 3.0f;
};

// The text tools' inputs and scan results, and a few settings from the config.
// StartWindow() sets it with designated initializers, so keep the field order
struct inputs_results_t
{
    std::string  ocr_path;  // the folder ocr_language is loaded from
    std::string  ocr_language;
    std::string  ocr_download_repo;
    std::string  ocr_model_downloaded_path;  // where the OCR model downloader saves models
    float        ocr_seconds = 0.0f;         // how long the last OCR took
    ocr_result_t ocr_results;

    std::string      barcode_text;
    bool             barcode_scanned = false;  // so an empty barcode_text means it found nothing
    float            barcode_seconds = 0.0f;
    barcode_result_t barcode_scan_result;

    std::string ann_font;
    std::string resolved_ann_font_path;  // ann_font found by get_font_path()

    // The OCR and barcode text from before the plugins' text transformers ran
    std::string ocr_original;
    std::string barcode_original;
};

// An OCR language in the Language combo
struct ocr_language_t
{
    std::string code;    // e.g. "ita"
    std::string folder;  // the one of m_ocr_folders it's in
    std::string label;   // e.g. "Italian (ita)"
};

// A decoded code in the text tools' table
struct code_row_t
{
    size_t      code;   // in barcode_result_t::codes
    std::string text;   // its data, wrapped to the Data column, at most 3 lines
    size_t      lines;  // in text
};

#ifndef DISABLE_PLUGINS
// One entry pushed by a PluginCallbacks::on_* call. Made on the install worker thread, read and cleared on the
// render thread
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

// One plugin task's state, shared with the worker thread, which may still write to it
// after the UI dropped its reference (oshot closed mid-install)
struct plugin_install_state_t
{
    // PluginCallbacks::on_* events, drained by DrawPluginInstallStatus()
    std::mutex                         events_mutex;
    std::deque<plugin_install_event_t> pending_events;

    // A PluginCallbacks::confirm question. The worker waits on confirm_cv
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

    // Set when offline and this is the last downloaded copy
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

    void Toggle(Enum e) { flags.flip(idx(e)); }

    [[nodiscard]] bool Has(Enum e) const { return flags.test(idx(e)); }

    // Clears the flag, returning whether it was set
    bool Take(Enum e)
    {
        const bool was = Has(e);
        Clear(e);
        return was;
    }
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

// The overlay: selection, annotations, toolbar, text tools and the other windows. run_main_tool() owns the
// window and its renderer (see overlay_window()) and drives the frame loop
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): one instance, and the members are grouped by topic
class ScreenshotTool
{
public:
#ifndef DISABLE_PLUGINS
    ScreenshotTool(StateManager&& state) : m_plugin_manager(std::move(state), {}, /*is_cli=*/false) {}
    ~ScreenshotTool()
    {
        if (m_install.thread.joinable())
            m_install.thread.join();
        if (m_index.thread.joinable())
            m_index.thread.join();
    }
#endif

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

    // Ends the capture. The frame loop stops after this frame
    void Cancel();

    // Frees the OCR engine. Must run before the process exits, see OcrAPI::End()
    void EndOcr() { m_ocr_api.End(); }

    // Frees what only the capture that just ended needs (the screenshot, its
    // annotations and the OCR engine). The tray keeps g_ss_tool between captures
    void EndCapture();

    // Replaces the screenshot with an image file and starts a new selection.
    // Shows an error dialog on failure
    bool OpenImage(const std::string& path);

    // Crops a whole-desktop capture to the overlay's display. With keep_desktop the desktop stays
    // for another call, since Wayland only tells which output the overlay is on once it's mapped
    Result<> CropAndUseDisplay(SDL_DisplayID display, bool keep_desktop);

    // ------------------------------
    // Frame read-back
    // ------------------------------
    // Saves or copies the selection once OnCleanFrame() reads back the next frame. The overlay closes once it
    // worked, and stays open after an error or a canceled save dialog
    void RequestComplete(SavingOp op) { m_completed_op = op; }

    // Whether the overlay closed after copying the image
    [[nodiscard]] bool ImageCopied() const { return m_flags.Has(CaptureFlag::ImageCopied); }

    // Whether the next frame must be drawn with only the screenshot and the annotations,
    // for OnCleanFrame() to read back. Such a frame isn't presented
    [[nodiscard]] bool WantsCleanFrame() const
    {
        return IsCompleted() || m_pending_scan != ScanType::kNone || m_flags.Has(CaptureFlag::FinalImageRequested);
    }

    // Called by the frame loop once the frame WantsCleanFrame() asked for has been rendered. Reads
    // the active region from it for the pending scan, save/copy and plugins' request_final_image()
    void OnCleanFrame();

    // Read back the active region with its annotations on the next frame, for request_final_image()
    void RequestFinalImage() { m_flags.Set(CaptureFlag::FinalImageRequested); }

    // Whether RequestFinalImage() can work now (an image is shown and something is selected)
    [[nodiscard]] bool CanReadFinalImage() const;

    // ------------------------------
    // Idle rendering
    // ------------------------------
    /**
     * How long the frame loop may wait for input before drawing the next frame, so an untouched
     * overlay doesn't redraw at the refresh rate. Call it before polling the window's events
     * @return Seconds to wait for an event, 0 to draw right away
     **/
    [[nodiscard]] double GetIdleTimeout();

    // ------------------------------
    // Getters
    // ------------------------------
    // A copy of the active region of the screenshot, without annotations
    capture_result_t GetFinalImage();

    // The selection in screenshot pixels, or the whole screenshot without one
    [[nodiscard]] region_t GetActiveRegion() const;

#ifndef DISABLE_PLUGINS
    // The text buffer plugins read and write by ID (OSHOT_TEXT_OCR, OSHOT_TEXT_BARCODE), nullptr for another ID
    [[nodiscard]] std::string* GetTextBuffer(std::string_view id);
#endif

    [[nodiscard]] bool IsActive() const { return m_state != ToolState::Idle; }
    [[nodiscard]] bool IsCompleted() const { return m_completed_op != SavingOp::kNone; }

    // ------------------------------
    // Textures, fonts and style
    // ------------------------------
    // Uploads RGBA pixels to a new texture of the overlay's renderer, destroying `tex` first
    Result<ImTextureRef> CreateTexture(ImTextureID tex, std::span<const uint8_t> data, int w, int h);

    // Frees what belongs to the overlay's renderer and ImGui context (the screenshot, toolbar and plugin icon
    // textures, and the cached fonts). Called before the renderer is destroyed
    void EndWindow();

    // Fonts by path, added to the atlas the first time and drawn at any size. Returns the default font for a
    // file that can't be loaded
    ImFont* CacheAndGetFont(const std::string& font_path);

    // A font CacheAndGetFont() loaded, nullptr if it didn't yet, or the default font for an empty path
    [[nodiscard]] ImFont* GetCachedFont(const std::string& font_path) const;

private:
    // Gap between the selection border and the toolbar
    static constexpr float TOOLBAR_OFFSET = 10.0f;

    // Between a text annotation and its resize handles
    static constexpr float TEXT_HANDLE_PADDING = 4.0f;

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
        const bool       is_ann;
        const InputOwner main_input_owner;
        const ToolState  resizing_state;
        const ToolState  idle_state;

        HandleHovered    handle_hover    = HandleHovered::kNone;
        HandleHovered    dragging_handle = HandleHovered::kNone;
        selection_rect_t selection;
        selection_rect_t drag_start_selection;
        ImVec2           drag_start_mouse;

        // Main selection only: the selection and state from before the drag, which Esc puts back.
        // drag_start_selection moves when a handle crosses the opposite edge
        selection_rect_t before_drag;
        ToolState        state_before_drag = ToolState::Selecting;

        selection_info_t(const bool b, const InputOwner o, const ToolState r, const ToolState i)
            : is_ann(b), main_input_owner(o), resizing_state(r), idle_state(i)
        {}
    };

    // ------------------------------
    // Screenshot
    // ------------------------------
    // Where the screenshot is drawn (centered), in screen units. See UpdateWindowBg()
    ImRect m_image_rect;

    // The whole-desktop capture while CropAndUseDisplay() may still pick another display, and the display it's
    // cropped to now
    struct
    {
        capture_result_t image;
        SDL_DisplayID    crop = 0;
    } m_desktop;

    capture_result_t m_screenshot;
    ImTextureRef     m_texture_id;
    float            m_image_scale = 1.0f;  // screenshot pixels per screen unit, above 1 on a scaled output

    // ------------------------------
    // Selection
    // ------------------------------
    ToolState                     m_state       = ToolState::Idle;
    InputOwner                    m_input_owner = InputOwner::kNone;
    GeneralContext<CurrentAction> m_current_actions;
    GeneralContext<CaptureFlag>   m_flags;

    selection_info_t m_main_sel{ false, InputOwner::Selection, ToolState::Resizing, ToolState::Selected };
    selection_info_t m_anno_sel{ true, InputOwner::AnnSelection, ToolState::Selected, ToolState::Selected };

    // ------------------------------
    // Annotations and toolbar
    // ------------------------------
    // The annotation selected for moving or resizing, in m_annotations. m_annotations must not grow or shrink
    // while it's set
    annotation_t* m_current_annotation_resize = nullptr;

    // The selected annotation as it was when the drag started, which every frame of the drag starts from
    annotation_t m_ann_drag_start;

    // The annotation a (double-)click would focus, in m_annotations, outlined while hovered, and what to tell about it
    struct hover_t
    {
        size_t      annotation;
        const char* hint;
    };
    std::optional<hover_t> m_hover;

    // A row (or column) of toolbar buttons outside a border of the selection, centered on it. Ring 0 is next to
    // the selection, each further ring one button further out
    struct toolbar_row_t
    {
        SelectionBorder       border;
        size_t                ring;
        std::vector<ToolType> tools;
    };

    // From UpdateAnnsToolbarPos(), in the order the buttons go
    std::vector<toolbar_row_t> m_toolbar_rows;

    // The open annotation settings popup's size last frame, to place it before Begin()
    ImVec2 m_ann_popup_size{};

    std::vector<annotation_t>                      m_annotations;
    annotation_t                                   m_current_annotation;
    ToolType                                       m_current_tool = ToolType::kNone;
    rgba_t                                         m_current_color;
    std::array<float, idx(ToolType::COUNT)>        m_tool_thickness;
    std::array<ImTextureRef, idx(ToolType::COUNT)> m_tool_textures;

    // While IsTextPlacing: the m_annotations index of the text being edited, which isn't drawn meanwhile
    std::optional<size_t> m_edited_text;

    // A new ID for each text typed or edited, or ImGui would put the last one's text into the next
    unsigned m_text_input_id = 0;

    // ------------------------------
    // Text tools
    // ------------------------------
    // Set by RequestScan(), until OnCleanFrame()
    ScanType m_pending_scan = ScanType::kNone;

    // The folders RefreshOcrModels() last scanned, from Config::GetOcrFolders()
    std::vector<std::string> m_ocr_folders;

    OcrAPI           m_ocr_api;
    BarcodeAPI       m_barcode_api;
    inputs_results_t m_inputs;

    // The decoded codes the table shows, each once, built by RunBarcodeScan()
    struct
    {
        std::vector<code_row_t> rows;
        float                   rows_width = 0.0f;  // the Data column's width, which the rows are wrapped to
        ImGuiTextFilter         filter;
        int                     selected = -1;  // the row whose whole text the table shows, or -1
    } m_code_table;

    std::shared_ptr<ocr_download_t> m_ocr_download;
    std::future<size_t>             m_size_estimate;  // encoded size of the selection, for its label
    std::vector<ocr_language_t>     m_ocr_languages;
    ErrorContext<OcrError>          m_ocr_errors;
    ErrorContext<BarcodeError>      m_barcode_errors;
    ErrorContext<OcrDownloadError>  m_download_errors;
    ErrorContext<GeneralError>      m_general_errors;

    // ------------------------------
    // Windows and fonts
    // ------------------------------
    GeneralContext<SubWindow>      m_show_window;
    std::map<std::string, ImFont*> m_font_cache;

    // The last Esc that didn't quit
    double m_last_esc_time = -1.0;

    // The last input or KeepDrawing(), see GetIdleTimeout()
    struct
    {
        std::chrono::steady_clock::time_point last_activity;
        ImU32                                 last_input_event_id = 0;
    } m_idle;

    // ------------------------------
    // Completion
    // ------------------------------
    // Set by RequestComplete(), until OnCleanFrame()
    SavingOp m_completed_op = SavingOp::kNone;

#ifndef DISABLE_PLUGINS
    // ------------------------------
    // Plugins
    // ------------------------------
    PluginManager m_plugin_manager;

    // The last plugin task (StartPluginTask()) and the install windows
    struct
    {
        std::string                             source;  // the install window's source field
        std::shared_ptr<plugin_install_state_t> state;   // shared with `thread`
        std::vector<install_node_t>             events;  // the status window's tree, built from state's events
        std::thread                             thread;
    } m_install;

    // The plugin index for the Browse plugins window
    struct
    {
        std::shared_ptr<plugin_index_state_t> state = std::make_shared<plugin_index_state_t>();
        std::thread                           thread;
        ImGuiTextFilter                       filter;
    } m_index;

    // The selection plugins were last told about, see NotifyPluginsOfSelection()
    region_t m_plugin_selection;

    // The textures of the plugins' toolbar icons, by plugin ID and action ID. Deleted by Cancel()
    std::map<std::pair<std::string, int32_t>, ImTextureRef> m_plugin_icons;
#endif

    // Draw continuously for a moment, e.g. after input
    void KeepDrawing() { m_idle.last_activity = std::chrono::steady_clock::now(); }

    // ------------------------------
    // Input
    // ------------------------------
    // Returns true when the click only deselected an annotation, so the main selection ignores it this frame
    bool HandleAnnotationSelectionInput();

    void HandleShortcutsInput();
    void HandleSelectionInput(selection_info_t& sel);
    void HandleResizeInput(selection_info_t& sel);
    void HandleAnnotationInput();
    void HandleColorPickerInput();

    // Adds the text being typed, or saves the one being edited (removed if it's left empty)
    void CommitTextAnnotation();

    // Reopens the input on m_annotations[i], with the Text tool set to its color, size and font
    void EditTextAnnotation(size_t i);

    // ------------------------------
    // State updates
    // ------------------------------
    // Centers the screenshot in the viewport by setting m_image_rect and m_image_scale
    void UpdateWindowBg();

    // Spreads the toolbar buttons over the selection's borders
    void UpdateAnnsToolbarPos();

    // The toolbar's buttons in order, without the Text tools one while its window is open
    [[nodiscard]] std::vector<ToolType> GetToolbarButtons() const;

    // The plugins' toolbar buttons, drawn after SaveImage in its row
    [[nodiscard]] static size_t CountPluginToolbarButtons();

    // Keeps the selection on the screenshot (around a smaller image opened with -f there's nothing to capture)
    void ClampSelectionToImage(selection_info_t& sel);

    // Drops the main selection, its drag state and the current tool
    void ResetSelection();

    // Copies the config into m_inputs, after it was loaded or changed in Preferences
    void SyncRuntimeFromConfig();

    // Lists the models in the OCR path, if it changed since the last scan
    void RefreshOcrModels();

    // The handles of a selection, with their positions. A Line/Arrow annotation only has its endpoints
    [[nodiscard]] std::vector<std::pair<HandleHovered, ImVec2>> GetSelectionHandles(const selection_info_t& sel) const;

    void UpdateHandleHoverState(selection_info_t& sel);
    void UpdateCursor(const selection_info_t& sel);

    // Drops the selected annotation and its drag
    void DeselectAnnotation();

    // The annotations under `pos`: the ones whose drawn shape it's on, topmost first, then the outlines it's inside
    [[nodiscard]] std::vector<annotation_t*> GetAnnotationsAt(const ImVec2& pos);

    // Moves or resizes the selected annotation, from m_ann_drag_start and the mouse's movement since
    void ApplyAnnotationDrag();

    // ------------------------------
    // Geometry
    // ------------------------------
    // The annotation's bounds on screen
    [[nodiscard]] ImRect GetAnnotationBBox(const annotation_t& ann) const;

    // GetAnnotationBBox() plus a margin for the stroke, where the selected annotation's border and handles go
    [[nodiscard]] ImRect GetAnnotationHandleBox(const annotation_t& ann) const;

    static ImVec2 GetToolButtonSize();

    // How far the toolbar's ring `ring` is from the selection, horizontally and vertically
    static ImVec2 GetToolbarRingOffset(size_t ring);

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
    // Draws a selection's border and handles, for DrawAnnotationResizeBorder() and DrawSelectionBorder()
    void DrawASelectionBorder(selection_info_t& sel, const ImRect& r);

    // A button that copies `text_copy` and says "Copied!" for a moment.
    // Without `show_error`, the caller shows GeneralError::FailedToCopyText
    void CreateCopyTextButton(const std::string& text_copy,
                              const char*        label1     = _("Copy Text"),
                              const ImVec2&      size       = ImVec2(0, 0),
                              bool               show_error = true);

    // The Wrap, Original and Copy buttons over an output, right-aligned. Original shows while `original` (the text
    // from before the transformers) isn't empty, and Copy copies the text shown
    void DrawOutputButtons(const std::string& text,
                           const std::string& original,
                           CaptureFlag        show_original,
                           const char*        copy_label,
                           bool               show_wrap);

    // Every decoded code once, with its type and a copy button, and the clicked one's whole text under them.
    // Fills `height` pixels and scrolls. Past 10 codes a filter goes above
    void DrawBarcodeTable(float height);

    void DrawDarkOverlay();
    void DrawAnnotations();
    void DrawSelectionBorder();
    void DrawAnnotationResizeBorder();
    void DrawAnnotationToolbar();

    // The color, size and font of `tool`, opened by right-clicking its button (the last item), which is on the
    // selection's `side`
    void DrawAnnotationSettingsPopup(ToolType tool, SelectionBorder side);
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

#ifndef DISABLE_PLUGINS
    // ------------------------------
    // Plugins
    // ------------------------------
    // Downloads the plugin index into m_index.state, on m_index.thread
    void FetchIndexAsync();

    // Runs a PluginManager operation (install, update, uninstall) on a worker
    // thread, with callbacks that queue its progress in a new plugin_install_state_t
    // for the render thread to draw
    void StartPluginTask(const std::string& title, std::function<Result<>(PluginManager&)> task);

    // Call the plugins' on_capture_start(), and on_capture_end() for the ones that got it
    void StartPluginsCapture();
    void EndPluginsCapture();

    // Calls on_ocr_done() with what OCR read, or with nothing (nullptr) after it read nothing or failed
    void NotifyPluginsOfOcr(const ocr_result_t* res);

    // Runs the text transformers over a scan's text, keeping what it was in `original`
    void TransformText(std::string& text, std::string& original, const char* buffer_id);

    // Queues an action's fn() for the start of the next frame, so it can't change the actions being drawn
    void RunPluginAction(plugin_runtime_t& rt, int32_t action);

    // The plugins' OSHOT_PLACE_TOOLBAR buttons, after oshot's own
    void DrawPluginToolbarButtons(bool same_line);

    // The plugins' OSHOT_PLACE_TEXT_TOOLS entries, as a Plugins menu in the text tools' menu bar
    void DrawPluginActionsMenu();

    // The box in the overlay's corner with what report_progress() and report_result() said
    void DrawPluginReports();

    // Calls on_selection_changed() once the selection is done changing and differs
    // from what the plugins were last told
    void NotifyPluginsOfSelection();

    void DrawManagePluginsWindow();
    void DrawBrowsePluginsWindow();
    void DrawInstallPluginsWindow();
    void DrawPluginInstallStatus();
    void DrawEventIcon(plugin_install_event_t::Kind kind);
#endif
};

extern ScreenshotTool g_ss_tool;

// Files dropped on the window, picked up by the path field under the cursor
extern std::deque<std::string> g_dropped_paths;

#endif  // !_SCREENSHOT_TOOL_HPP_
