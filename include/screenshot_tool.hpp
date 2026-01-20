#ifndef _SCREENSHOT_TOOL_HPP_
#define _SCREENSHOT_TOOL_HPP_

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <future>
#include <unordered_map>
#include <utility>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "ocr.hpp"
#include "screen_capture.hpp"

#ifdef None
#  undef None
#endif

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
    float get_y() const { return std::min(start.y, start.y); }
    float get_width() const { return std::abs(end.x - start.x); }
    float get_height() const { return std::abs(end.y - start.y); }
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
    SAVE_CLIPBOARD,
    SAVE_FILE
};

enum class HandleHovered
{
    None,
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

enum ErrorState
{
    None                   = 0,
    FailedToInitOcr        = 1 << 1,
    InvalidPath            = 1 << 2,
    InvalidModel           = 1 << 3,
    FailedTranslation      = 1 << 4,
    InvalidLangFrom        = 1 << 5,
    InvalidLangTo          = 1 << 6,
    NoLauncher             = 1 << 7,
    WarnConnLauncher       = 1 << 8,
    FailedToExtractBarCode = 1 << 9
};

class ScreenshotTool
{
public:
    ScreenshotTool() : m_io(dummy) {}

    bool Start();
    bool StartWindow();
    bool CreateTexture();
    bool HasError(ErrorState err);
    bool IsActive() const { return m_state != ToolState::Idle; }

    capture_result_t GetFinalImage();

    ImFont* GetOrLoadFontForLanguage(const std::string& lang_code);

    void RenderOverlay();
    void Cancel();
    void ClearError(ErrorState err);
    void SetError(ErrorState err);

    void SetOnComplete(const std::function<void(SavingOp, const capture_result_t&)>& cb)
    {
        m_on_complete = std::move(cb);
    }

    void SetOnCancel(const std::function<void()>& cb) { m_on_cancel = std::move(cb); }

private:
    static constexpr float HANDLE_DRAW_SIZE  = 4.0f;
    static constexpr float HANDLE_HOVER_SIZE = 10.0f;

    struct FontCacheEntry
    {
        std::string font_path;
        ImFont*     font   = nullptr;
        bool        loaded = false;
    };

    struct HandleInfo
    {
        HandleHovered type;
        ImVec2        pos;
        ImRect        rect;
    };

    ImGuiIO&         m_io;
    OcrAPI           m_api;
    capture_result_t m_screenshot;

    void*         m_texture_id      = nullptr;
    ToolState     m_state           = ToolState::Idle;
    HandleHovered m_handle_hover    = HandleHovered::None;
    HandleHovered m_dragging_handle = HandleHovered::None;
    int           m_err_state       = ErrorState::None;

    selection_rect_t m_selection;
    selection_rect_t m_drag_start_selection;

    bool m_is_selecting{};
    bool m_is_hovering_ocr{};

    ImVec2 m_drag_start_mouse{};
    ImVec2 m_image_origin{};
    ImVec2 m_image_end{};

    std::future<bool> m_connect_future;
    std::atomic<bool> m_connect_done;

    std::string m_ocr_text;
    std::string m_to_translate_text;
    std::string m_barcode_text;

    std::unordered_map<std::string, FontCacheEntry>        m_font_cache;
    std::function<void()>                                  m_on_cancel;
    std::function<void(SavingOp, const capture_result_t&)> m_on_complete;

    ImGuiIO dummy;

    void HandleSelectionInput();
    void HandleResizeInput();
    void DrawDarkOverlay();
    void DrawMenuItems();
    void DrawSelectionBorder();
    void DrawSizeIndicator();
    void DrawOcrTools();
    void DrawTranslationTools();
    void DrawBarDecodeTools();
    void UpdateHandleHoverState();
    void UpdateCursor();
    void UpdateWindowBg();
};

#endif  // !_SCREENSHOT_TOOL_HPP_
