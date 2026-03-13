#ifndef _SCREENSHOT_TOOL_HPP_
#define _SCREENSHOT_TOOL_HPP_

#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>

#include "config.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "screen_capture.hpp"
#include "text_extraction.hpp"

enum class ToolType : size_t
{
    kNone,
    Arrow,
    Rectangle,
    RectangleFilled,
    Circle,
    CircleFilled,
    Line,
    Text,
    Pencil,
    ToggleTextTools,
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

enum ErrorFlag : size_t
{
    kNone = 0,
    FailedToScanOcr,
    InvalidPath,
    InvalidModel,
    FailedToScanBarCode,
    FailedToCopyText,
    COUNT
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
    std::string          text;                    // For text tool
    std::vector<point_t> points;                  // For pencil tool
    uint32_t             color     = 0xFF0000FF;  // RGBA
    float                thickness = 3.0f;
};

class ScreenshotTool
{
public:
    ScreenshotTool() : m_io(dummy), m_inputs{ g_config->File.ocr_path, g_config->File.ocr_model, {}, "", {}, "" } {}

    Result<>          Start();
    Result<>          StartWindow();
    Result<void*>     CreateTexture(void* tex, std::span<const uint8_t> data, int w, int h);
    bool              OpenImage(const std::string& path);
    bool              IsActive() const { return m_state != ToolState::Idle; }
    capture_result_t& GetRawScreenshot() { return m_screenshot; }
    void              SetBackendTexture(void* tex) { m_texture_id = tex; }
    void              SetToolTexture(ToolType type, void* tex) { m_tool_textures[idx(type)] = tex; }

    void SetOnImageReload(std::function<void(const capture_result_t&)> fn) { m_on_image_reload = std::move(fn); }

    capture_result_t GetFinalImage(bool is_text_tools = false);

    ImFont* CacheAndGetFont(const std::string& font_name, const float font_size);

    void RenderOverlay();
    void Cancel();

    void SetError(ErrorFlag f, const std::string& err = "")
    {
        m_errors.set(static_cast<size_t>(f));
        m_err_texts[f] = err;
    }
    const std::string& GetError(ErrorFlag f) { return m_err_texts[f]; }
    void               ClearError(ErrorFlag f) { m_errors.reset(static_cast<size_t>(f)); }
    bool               HasError(ErrorFlag f) const { return m_errors.test(static_cast<size_t>(f)); }

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

    // Contains user inputs, APIs results,
    // and maybe some user settings
    struct inputs_results_t
    {
        std::string  ocr_path;
        std::string  ocr_model;
        ocr_result_t ocr_results;

        std::string   barcode_text;
        zbar_result_t zbar_scan_result;

        std::string ann_font;
    };

    ImGuiIO&         m_io;
    OcrAPI           m_ocr_api;
    ZbarAPI          m_zbar_api;
    capture_result_t m_screenshot;

    void*         m_texture_id      = nullptr;
    ToolState     m_state           = ToolState::Idle;
    HandleHovered m_handle_hover    = HandleHovered::kNone;
    HandleHovered m_dragging_handle = HandleHovered::kNone;
    InputOwner    m_input_owner     = InputOwner::kNone;

    std::bitset<idx(ErrorFlag::COUNT)> m_errors;

    selection_rect_t m_selection;
    selection_rect_t m_drag_start_selection;

    inputs_results_t m_inputs;
    bool             m_show_text_tools = true;
    bool             m_is_selecting    = false;

    ImVec2 m_drag_start_mouse;
    ImVec2 m_image_origin;
    ImVec2 m_image_end;

    std::vector<std::string>                                       m_ocr_models_list;
    std::unordered_map<ErrorFlag, std::string>                     m_err_texts;
    std::map<std::pair<std::string, float>, font_cache_t>          m_font_cache;
    std::function<void()>                                          m_on_cancel;
    std::function<void(const capture_result_t&)>                   m_on_image_reload;
    std::function<void(SavingOp, const Result<capture_result_t>&)> m_on_complete;

    ImGuiIO dummy;

    std::array<void*, idx(ToolType::Count)> m_tool_textures{};
    ToolType                                m_current_tool = ToolType::kNone;
    std::vector<annotation_t>               m_annotations;
    annotation_t                            m_current_annotation;
    uint32_t                                m_current_color = 0xFF0000FF;
    std::array<float, idx(ToolType::Count)> m_tool_thickness;
    ImVec4                                  m_picker_color{ 1, 0, 0, 1 };
    bool                                    m_is_drawing       = false;
    bool                                    m_is_color_picking = false;
    bool                                    m_is_text_placing  = false;

    void CreateCopyTextButton(const std::string& text);
    void RefreshOcrModels();

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

    void UpdateHandleHoverState();
    void UpdateCursor();
    void UpdateWindowBg();
};

extern std::deque<std::string> g_dropped_paths;

#endif  // !_SCREENSHOT_TOOL_HPP_
