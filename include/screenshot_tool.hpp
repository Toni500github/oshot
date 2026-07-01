#ifndef _SCREENSHOT_TOOL_HPP_
#define _SCREENSHOT_TOOL_HPP_

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "../src/plugins/oshot_plugin.h"
#include "dylib.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "screen_capture.hpp"
#include "text_extraction.hpp"
#include "util.hpp"

enum class ToolType : size_t
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
    Count
};

enum class ToolState
{
    Idle,
    Capturing,
    Selecting,
    Selected,
    Resizing
};

enum class SavingOp
{
    Clipboard,
    File
};

enum class HandleHovered
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

enum class InputOwner
{
    kNone,
    Selection,
    Tools
};

enum class PrefTab
{
    kNone    = -1,
    Defaults = 0,
    Theme
};

enum class OcrDownloadError : size_t
{
    InvalidRepo,
    InvalidPath,
    FailedToDownload,
    COUNT
};

enum class OcrError : size_t
{
    InvalidModel,
    InvalidPath,
    FailedToScan,
    COUNT
};

enum class ZbarError : size_t
{
    FailedToScan,
    COUNT,
};

enum class GeneralError : size_t
{
    FailedToCopyText,
    COUNT,
};

struct plugin_entry_t
{
    dylib::library  lib;
    oshot_plugin_t* plugin;
    void*           state;
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

    float get_x() const { return std::min(start.x, end.x); }
    float get_y() const { return std::min(start.y, end.y); }
    float get_width() const { return std::abs(end.x - start.x); }
    float get_height() const { return std::abs(end.y - start.y); }
};

struct annotation_t
{
    ToolType             type = ToolType::kNone;
    point_t              start;
    point_t              end;
    std::string          text;                                       // For text tool
    std::uint8_t         count = 0;                                  // For CounterBubble tool
    std::vector<point_t> points;                                     // For pencil tool
    rgba_t               color     = rgba_t::from_rgba(0xFF0000FF);  // RGBA
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

template <typename Enum>
struct ErrorContext
{
    std::bitset<idx(Enum::COUNT)>             flags;
    std::array<std::string, idx(Enum::COUNT)> texts;

    void Set(Enum e, std::string_view msg = {})
    {
        flags.set(idx(e));
        texts[idx(e)] = msg;
    }

    void Clear(Enum e)
    {
        flags.reset(idx(e));
        texts[idx(e)].clear();
    }

    bool Has(Enum e) const { return flags.test(idx(e)); }

    template <typename... E>
    bool HasAny(E... e) const
    {
        return (Has(e) || ...);
    }

    const std::string& Get(Enum e) const { return texts[idx(e)]; }
};

class ScreenshotTool
{
public:
    Result<>             Start();
    Result<>             StartWindow();
    Result<ImTextureRef> CreateTexture(void* tex, std::span<const uint8_t> data, int w, int h);
    bool                 OpenImage(const std::string& path);
    bool                 IsActive() const { return m_state != ToolState::Idle; }
    capture_result_t&    GetRawScreenshot() { return m_screenshot; }
    void                 SetBackendTexture(void* tex) { m_texture_id._TexID = static_cast<ImTextureID>(size_t(tex)); }
    void                 SetToolTexture(ToolType type, void* tex)
    {
        m_tool_textures[idx(type)]._TexID = static_cast<ImTextureID>(size_t(tex));
    }

    auto& GetImGuiIDTexts() { return m_imgui_id_texts; }

    void SetOnImageReload(std::function<void(const capture_result_t&)> fn) { m_on_image_reload = std::move(fn); }

    capture_result_t GetFinalImage(bool is_text_tools = false);
    region_t         GetActiveRegion() const;

    ImFont* CacheAndGetFont(const std::string& font_name, const float font_size);

    void RenderOverlay();
    void Cancel();

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

    void SetOnComplete(const std::function<void(SavingOp, const Result<capture_result_t>&)>& cb)
    {
        m_on_complete = std::move(cb);
    }

    void SetOnCancel(const std::function<void()>& cb) { m_on_cancel = std::move(cb); }

private:
    static constexpr float HANDLE_DRAW_SIZE  = 4.0f;
    static constexpr float HANDLE_HOVER_SIZE = 20.0f;

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

    OcrAPI           m_ocr_api;
    ZbarAPI          m_zbar_api;
    capture_result_t m_screenshot;

    ImTextureRef  m_texture_id;
    ToolState     m_state           = ToolState::Idle;
    HandleHovered m_handle_hover    = HandleHovered::kNone;
    HandleHovered m_dragging_handle = HandleHovered::kNone;
    InputOwner    m_input_owner     = InputOwner::kNone;

    ErrorContext<OcrDownloadError> m_download_errors;
    ErrorContext<OcrError>         m_ocr_errors;
    ErrorContext<ZbarError>        m_zbar_errors;
    ErrorContext<GeneralError>     m_general_errors;

    selection_rect_t m_selection;
    selection_rect_t m_drag_start_selection;

    inputs_results_t m_inputs;
    bool             m_show_text_tools = true;

    ImVec2 m_drag_start_mouse;
    ImVec2 m_image_origin;
    ImVec2 m_image_end;

    std::shared_ptr<ocr_download_t>                                m_ocr_download;
    std::vector<std::string>                                       m_ocr_models_list;
    std::map<std::pair<std::string, float>, font_cache_t>          m_font_cache;
    std::function<void()>                                          m_on_cancel;
    std::function<void(const capture_result_t&)>                   m_on_image_reload;
    std::function<void(SavingOp, const Result<capture_result_t>&)> m_on_complete;

    std::array<ImTextureRef, idx(ToolType::Count)> m_tool_textures{};
    ToolType                                       m_current_tool = ToolType::kNone;
    std::vector<annotation_t>                      m_annotations;
    annotation_t                                   m_current_annotation;
    rgba_t                                         m_current_color;
    std::unordered_map<std::string, std::string*>  m_imgui_id_texts;
    std::array<float, idx(ToolType::Count)>        m_tool_thickness;
    bool                                           m_is_drawing       = false;
    bool                                           m_is_color_picking = false;
    bool                                           m_is_text_placing  = false;

    void CreateCopyTextButton(const std::string& text);
    void RefreshOcrModels();
    void NormalizeSelection();
    void SyncRuntimeFromConfig();

    void HandleShortcutsInput();
    void HandleSelectionInput();
    void HandleResizeInput();
    void HandleAnnotationInput();
    void HandleColorPickerInput();

    void DrawDarkOverlay();
    void DrawAnnotations();
    void DrawMenuItems();
    void DrawSelectionBorder();

    void DrawOcrTools();
    void DrawBarDecodeTools();
    void DrawAnnotationToolbar();
    void DrawPreferencesWindow();
    void DrawDownloadOCRWindow();

    void UpdateHandleHoverState();
    void UpdateCursor();
    void UpdateWindowBg();

    template <typename Enum>
    bool ShowIfError(const ErrorContext<Enum>& ctx, Enum e)
    {
        bool has = ctx.Has(e);
        if (has)
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", ctx.Get(e).c_str());
        return has;
    }
};

extern ScreenshotTool              g_ss_tool;
extern std::deque<std::string>     g_dropped_paths;
extern std::vector<plugin_entry_t> g_plugin_entries;

#endif  // !_SCREENSHOT_TOOL_HPP_
