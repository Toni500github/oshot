#ifndef _SCREENSHOT_TOOL_HPP_
#define _SCREENSHOT_TOOL_HPP_

#include <algorithm>
#include <cstdlib>
#include <functional>

#include "imgui/imgui.h"
#include "ocr.hpp"
#include "screen_capture.hpp"

#ifdef None
#undef None
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
    float get_y() const { return std::min(start.y, end.y); }
    float get_width() const { return std::abs(end.x - start.x); }
    float get_height() const { return std::abs(end.y - start.y); }
};

enum class ToolState
{
    Idle,
    Capturing,
    Selecting,
    Selected
};

enum ErrorState
{
    None              = 0,
    InitOcr           = 1 << 1,
    InvalidPath       = 1 << 2,
    InvalidModel      = 1 << 3,
    FailedTranslation = 1 << 4,
    InvalidLangFrom   = 1 << 5,
    InvalidLangTo     = 1 << 6
};

class ScreenshotTool
{
public:
    ScreenshotTool(ImGuiIO& io) : m_io(io) {}
    ~ScreenshotTool() { m_texture_id = nullptr; }

    bool Start();

    // Returns true if active, else false if finished
    bool RenderOverlay();

    void             CreateTexture();
    capture_result_t GetFinalImage();

    void Cancel();

    bool IsActive() const { return m_state != ToolState::Idle; }
    void SetOnComplete(const std::function<void(capture_result_t)>& cb) { m_on_complete = cb; }
    void SetOnCancel(const std::function<void()>& cb) { m_on_cancel = cb; }

private:
    struct FontCacheEntry
    {
        std::string font_path;
        ImFont*     font   = nullptr;
        bool        loaded = false;
    };

    void HandleSelectionInput();
    void DrawDarkOverlay();
    void DrawMenuItems();
    void DrawSelectionBorder();
    void DrawSizeIndicator();
    void DrawOcrTools();
    void DrawTranslationTools();

    ImFont* GetOrLoadFontForLanguage(const std::string& lang_code);
    bool    HasError(ErrorState err);
    bool    HasErrors();
    void    ClearError(ErrorState err);
    void    SetError(ErrorState err);

    OcrAPI           m_api;
    ImGuiIO&         m_io;
    capture_result_t m_screenshot;
    void*            m_texture_id = nullptr;  // ImGui texture ID
    ToolState        m_state      = ToolState::Idle;
    int              m_err_state  = ErrorState::None;
    selection_rect_t m_selection;
    bool             m_is_selecting{};
    bool             m_is_hovering_ocr{};

    std::unordered_map<std::string, FontCacheEntry> m_font_cache;

    std::string m_ocr_text;
    std::string m_to_translate_text;

    std::function<void(const capture_result_t&)> m_on_complete;
    std::function<void()>                        m_on_cancel;
};

#endif  // !_SCREENSHOT_TOOL_HPP_
