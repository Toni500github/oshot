#include "screenshot_tool.hpp"

#include <tesseract/publictypes.h>
#include <zbar.h>

#include <algorithm>
#include <array>
#include <future>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "clipboard.hpp"
#include "config.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3_loader.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "langs.hpp"
#include "screen_capture.hpp"
#include "socket.hpp"
#include "tinyfiledialogs.h"
#include "tool_icons.h"
#include "translation.hpp"
#include "util.hpp"

#ifndef GL_NO_ERROR
#  define GL_NO_ERROR 0
#endif

using namespace std::chrono_literals;

static std::unique_ptr<Translator> translator;
static ImVec2                      origin(0, 0);

static std::array<void*, idx(ToolType::Count)> tool_textures;

static std::vector<std::string> get_training_data_list(const std::string& path)
{
    if (!std::filesystem::exists(path))
        return {};

    std::vector<std::string> list;
    for (auto const& dir_entry : std::filesystem::directory_iterator{ path })
        if (dir_entry.path().extension() == ".traineddata")
            list.push_back(dir_entry.path().stem().string());
    return list;
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

static ImVec4 get_confidence_color(const int confidence)
{
    if (confidence <= 45)
        return ImVec4(1, 0, 0, 1);  // red
    else if (confidence <= 80)
        return ImVec4(1, 1, 0, 1);  // yellow
    else
        return ImVec4(0, 1, 0, 1);  // green
}

Result<> ScreenshotTool::Start()
{
    translator = std::make_unique<Translator>();
    g_sender   = std::make_unique<SocketSender>();
    Result<capture_result_t> result{ Err() };

    if (!g_config->Runtime.source_file.empty())
    {
        result = load_image_rgba(g_config->Runtime.source_file);
    }
    else
    {
        if (g_config->File.delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(g_config->File.delay));

        switch (get_session_type())
        {
            case SessionType::X11:     result = capture_full_screen_x11(); break;
            case SessionType::Wayland: result = capture_full_screen_wayland(); break;
            case SessionType::Windows: result = capture_full_screen_windows(); break;
            default:                   return Err("Unknown platform");
        }
    }

    if (!result.ok())
        return Err("Failed to do data with screenshot: " + result.error().value);

    m_screenshot = std::move(result.get());
    m_tool_thickness.fill(3.0f);
    return Ok();
}

Result<> ScreenshotTool::StartWindow()
{
    m_io    = ImGui::GetIO();
    m_state = ToolState::Selecting;

    auto v = std::async(std::launch::async, [&] {
        return g_sender->Start();  // async because of blocking connect()
    });

    const Result<void*>& res = CreateTexture(m_texture_id, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    if (!res.ok())
        return Err("Failed create openGL texture: " + res.error().value);

    m_texture_id = res.get();
    fit_to_screen(m_screenshot);

    // Since the creation of the screenshot texture was fine, suppose the other too
    tool_textures[idx(ToolType::Rectangle)] =
        CreateTexture(nullptr, ICON_SQUARE_RGBA, ICON_SQUARE_W, ICON_SQUARE_H).get();
    tool_textures[idx(ToolType::RectangleFilled)] =
        CreateTexture(nullptr, ICON_RECT_FILLED_RGBA, ICON_RECT_FILLED_W, ICON_RECT_FILLED_H).get();
    tool_textures[idx(ToolType::Line)]   = CreateTexture(nullptr, ICON_LINE_RGBA, ICON_LINE_W, ICON_LINE_H).get();
    tool_textures[idx(ToolType::Circle)] = CreateTexture(nullptr, ICON_CIRCLE_RGBA, ICON_CIRCLE_W, ICON_CIRCLE_H).get();
    tool_textures[idx(ToolType::CircleFilled)] =
        CreateTexture(nullptr, ICON_CIRCLE_FILLED_RGBA, ICON_CIRCLE_FILLED_W, ICON_CIRCLE_FILLED_H).get();
    tool_textures[idx(ToolType::Arrow)]  = CreateTexture(nullptr, ICON_ARROW_RGBA, ICON_ARROW_W, ICON_ARROW_H).get();
    tool_textures[idx(ToolType::Pencil)] = CreateTexture(nullptr, ICON_PENCIL_RGBA, ICON_PENCIL_W, ICON_PENCIL_H).get();

    return Ok();
}

void ScreenshotTool::RenderOverlay()
{
    static constexpr int minimal_win_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoBackground;
    // Overlay window
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(m_io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, origin);
    ImGui::Begin("Screenshot Tool",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground);

    // Screenshot as a centered bg image
    UpdateWindowBg();
    ImGui::GetBackgroundDrawList()->AddImage(m_texture_id, m_image_origin, m_image_end);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        Cancel();
    }

    if (m_selection.get_width() == 0 || m_selection.get_height() == 0)
    {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::Begin("##select_area", nullptr, minimal_win_flags);
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Select an area");
        ImGui::End();
    }

    if (m_state == ToolState::Selecting || m_state == ToolState::Selected || m_state == ToolState::Resizing)
    {
        HandleSelectionInput();
        DrawDarkOverlay();
        DrawSelectionBorder();
        DrawAnnotations();
    }

    if (m_state == ToolState::Selected)
    {
        HandleAnnotationInput();
        DrawAnnotationToolbar();
    }

    ImGui::End();
    ImGui::PopStyleVar();

    if (m_state == ToolState::Selected)
    {
        ImGui::Begin("Text tools", nullptr, ImGuiWindowFlags_MenuBar);
        DrawMenuItems();
        DrawOcrTools();
        DrawTranslationTools();
        DrawBarDecodeTools();
        ImGui::End();
    }
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
    float         sel_x     = m_selection.get_x();
    float         sel_y     = m_selection.get_y();
    float         sel_w     = m_selection.get_width();
    float         sel_h     = m_selection.get_height();
    ImRect        selection_rect(ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h));

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_is_selecting)
    {
        m_input_owner  = InputOwner::Selection;
        m_is_selecting = true;

        // Check if we're starting to resize from a handle
        if (m_handle_hover != HandleHovered::kNone)
        {
            m_dragging_handle      = m_handle_hover;
            m_drag_start_mouse     = mouse_pos;
            m_drag_start_selection = m_selection;
            m_state                = ToolState::Resizing;
        }
        // Check if we're clicking inside the selection to move it
        else if (selection_rect.Contains(mouse_pos))
        {
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

    if (m_is_selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (m_state == ToolState::Resizing)
            HandleResizeInput();
        else  // ToolState::Selecting
            m_selection.end = { mouse_pos.x, mouse_pos.y };
    }

    if (m_is_selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_is_selecting    = false;
        m_dragging_handle = HandleHovered::kNone;
        m_input_owner     = InputOwner::kNone;

        if (m_selection.get_width() > 10 && m_selection.get_height() > 10)
            m_state = ToolState::Selected;
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
        case HandleHovered::Move:  // Move the entire selection
            m_selection.start.x = m_drag_start_selection.start.x + delta.x;
            m_selection.start.y = m_drag_start_selection.start.y + delta.y;
            m_selection.end.x   = m_drag_start_selection.end.x + delta.x;
            m_selection.end.y   = m_drag_start_selection.end.y + delta.y;
            break;
        default: break;
    }
}

void ScreenshotTool::HandleAnnotationInput()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ui_blocks_selection())
    {
        m_is_drawing                   = true;
        m_current_annotation.type      = m_current_tool;
        m_current_annotation.start     = { mouse_pos.x, mouse_pos.y };
        m_current_annotation.end       = m_current_annotation.start;
        m_current_annotation.color     = m_current_color;
        m_current_annotation.thickness = m_tool_thickness[idx(m_current_tool)];
        m_current_annotation.points.clear();

        if (m_current_tool == ToolType::Pencil)
            m_current_annotation.points.push_back(m_current_annotation.start);
    }

    if (m_is_drawing && ImGui::IsMouseDown(ImGuiMouseButton_Left))
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

    if (m_is_drawing && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_is_drawing = false;

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

void ScreenshotTool::UpdateHandleHoverState()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    m_handle_hover          = HandleHovered::kNone;

    if (m_state != ToolState::Selected && m_state != ToolState::Resizing)
        return;

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_w = m_selection.get_width();
    float sel_h = m_selection.get_height();

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

    for (const auto& handle : handles)
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
        float sel_x = m_selection.get_x();
        float sel_y = m_selection.get_y();
        float sel_w = m_selection.get_width();
        float sel_h = m_selection.get_height();

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

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_w = m_selection.get_width();
    float sel_h = m_selection.get_height();

    ImU32 dark_color = IM_COL32(0, 0, 0, 120);

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
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_w = m_selection.get_width();
    float sel_h = m_selection.get_height();

    UpdateHandleHoverState();

    UpdateCursor();

    // Draw selection border
    draw_list->AddRect(
        ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h), IM_COL32(0, 150, 255, 255), 0.0f, 0, 1.0f);

    if (!g_config->Runtime.enable_handles)
        return;

    // Draw handles
    const float handle_draw_half = HANDLE_DRAW_SIZE / 2.0f;
    auto        draw_handle      = [&](ImVec2 pos, HandleHovered type) {
        ImVec2 min = ImVec2(pos.x - handle_draw_half, pos.y - handle_draw_half);
        ImVec2 max = ImVec2(pos.x + handle_draw_half, pos.y + handle_draw_half);

        ImU32 color = IM_COL32(255, 255, 255, 255);
        if (m_handle_hover == type || m_dragging_handle == type)
            color = IM_COL32(255, 255, 0, 255);  // Yellow

        draw_list->AddRectFilled(min, max, color);
        draw_list->AddRect(min, max, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
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
}

void ScreenshotTool::DrawMenuItems()
{
    static bool show_about = false;

    if (ImGui::BeginMenuBar())
    {
        // Handle shortcuts FIRST, before drawing menus
        if (ImGui::Shortcut(ImGuiKey_E | ImGuiMod_Ctrl))
        {
            g_config->File.allow_ocr_edit = !g_config->File.allow_ocr_edit;
            ImGui::ClearActiveID();  // avoid flipping InputText flags while editing
        }

        if (ImGui::Shortcut(ImGuiKey_G | ImGuiMod_Ctrl))
            g_config->Runtime.enable_handles = !g_config->Runtime.enable_handles;

        if (ImGui::Shortcut(ImGuiKey_S | ImGuiMod_Ctrl))
            if (m_on_complete)
                m_on_complete(SavingOp::File, Ok(GetFinalImage()));

        if (ImGui::Shortcut(ImGuiKey_C | ImGuiMod_Ctrl | ImGuiMod_Shift))
            if (m_on_complete)
                m_on_complete(SavingOp::Clipboard, Ok(GetFinalImage()));

        // Now draw the menus
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open Image..."))
            {
                const char* filter[]  = { "*.png", "*.jpeg", "*.jpg", "*.bmp" };
                const char* open_path = tinyfd_openFileDialog("Open Image",
                                                              "",                // default path
                                                              4,                 // number of filter patterns
                                                              filter,            // file filters
                                                              "Images (*.png)",  // filter description
                                                              false              // allow multiple selections
                );

                if (open_path)
                    OpenImage(open_path);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Save Image", "CTRL+S"))
                if (m_on_complete)
                    m_on_complete(SavingOp::File, Ok(GetFinalImage()));

            if (ImGui::MenuItem("Copy Image", "CTRL+SHIFT+C"))
                if (m_on_complete)
                    m_on_complete(SavingOp::Clipboard, Ok(GetFinalImage()));

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
                ImGui::RadioButton("Block", &g_config->Runtime.preferred_psm, tesseract::PSM_SINGLE_BLOCK);
                ImGui::RadioButton("Big Region", &g_config->Runtime.preferred_psm, tesseract::PSM_AUTO);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::MenuItem("View Handles", "CTRL+G", &g_config->Runtime.enable_handles);
            if (ImGui::MenuItem("Allow OCR edit", "CTRL+E", &g_config->File.allow_ocr_edit))
                ImGui::ClearActiveID();

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About"))
                show_about = true;
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    if (show_about)
    {
        ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
        ImGui::Begin("About", &show_about, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("oshot");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("Screenshot tool to extract and translate text on the fly");

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::Text("Version: " VERSION);
        ImGui::Text("Created by: Toni500");
        ImGui::Text("Copyright © 2026");

        ImGui::Spacing();
        if (ImGui::Button("Close"))
            show_about = false;

        ImGui::End();
    }
}

void ScreenshotTool::DrawOcrTools()
{
    static std::string ocr_path{ g_config->File.ocr_path };
    static std::string ocr_model{ g_config->File.ocr_model };
    static size_t      item_selected_idx = 0;
    static bool        first_frame       = true;

    static std::vector<std::string> models_list;

    auto refresh_models = [&]() {
        models_list = get_training_data_list(ocr_path);
        if (models_list.empty())
        {
            SetError(InvalidPath);
        }
        else
        {
            ClearError(InvalidPath);
            // Find current model in list
            const auto& it    = std::find(models_list.begin(), models_list.end(), ocr_model);
            item_selected_idx = (it != models_list.end()) ? std::distance(models_list.begin(), it) : 0;
            if (it == models_list.end())
                SetError(InvalidModel);
            else
                ClearError(InvalidModel);
        }
    };

    if (first_frame)
    {
        refresh_models();
        first_frame = false;
    }

    float button_size = ImGui::GetFrameHeight();

    ImGui::PushID("OcrTools");
    ImGui::SeparatorText("OCR");

    // Snapshot of the error for not crashing the program
    bool invalid_path = HasError(InvalidPath);
    if (invalid_path)
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));

    ImGui::PushItemWidth(ImGui::CalcItemWidth() - button_size);
    if (ImGui::InputText("##ocr_path", &ocr_path))
        refresh_models();
    ImGui::PopItemWidth();

    // If user drops onto the input text, take it
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !g_dropped_paths.empty())
    {
        ocr_path = g_dropped_paths.back();
        g_dropped_paths.clear();
        refresh_models();
    }

    ImGui::SameLine(0, 0);
    if (ImGui::Button("...", ImVec2(button_size, button_size)))
    {
        const char* path = tinyfd_selectFolderDialog("Open model folder", nullptr);
        if (path)
        {
            ocr_path.assign(path);
            refresh_models();
        }
    }

    // Same thing with the button
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !g_dropped_paths.empty())
    {
        ocr_path = g_dropped_paths.back();
        g_dropped_paths.clear();
        refresh_models();
    }

    ImGui::SameLine(0, 3);
    ImGui::Text("Path");
    if (invalid_path)
    {
        ImGui::SameLine();
        ImGui::Text("Invalid!");
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    HelpMarker("Full-Path to the OCR models (.traineddata). Supports drag-and-drop too");

    if (!invalid_path)
    {
        bool invalid_model = HasError(InvalidModel);
        if (invalid_model)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));

        if (ImGui::BeginCombo("Model", ocr_model.c_str(), ImGuiComboFlags_HeightLarge))
        {
            static ImGuiTextFilter filter;
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
                filter.Clear();
            }

            ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F);
            filter.Draw("##Filter", -FLT_MIN);

            for (size_t i = 0; i < models_list.size(); ++i)
            {
                bool is_selected = (item_selected_idx == i);
                if (filter.PassFilter(models_list[i].c_str()))
                {
                    if (ImGui::Selectable(models_list[i].c_str(), is_selected))
                    {
                        item_selected_idx = i;
                        ocr_model         = models_list[i];
                        ClearError(InvalidModel);
                    }
                }
            }
            ImGui::EndCombo();
        }

        if (invalid_model)
        {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid!");
        }
    }

    if (!HasError(InvalidModel) && !HasError(InvalidPath) && ImGui::Button("Extract Text"))
    {
        const Result<>& res = m_ocr_api.Configure(ocr_path.c_str(), ocr_model.c_str());
        if (!res.ok())
        {
            SetError(FailedToInitOcr, res.error().value);
        }
        else
        {
            ClearError(FailedToInitOcr);
            const Result<ocr_result_t>& result = m_ocr_api.ExtractTextCapture(GetFinalImage());
            if (result.ok())
            {
                m_ocr_text = m_to_translate_text = result.get().data;
                m_ocr_confidence                 = result.get().confidence;
            }
        }
    }

    if (HasError(FailedToInitOcr))
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Failed to init OCR!");
    }

    if (!HasError(InvalidModel) && !HasError(InvalidPath))
    {
        ImGui::SameLine();
        HelpMarker("If the result seems off, you could try selecting an option in Edit > Optimize OCR for...");
    }

    if (m_ocr_confidence != -1)
    {
        ImGui::TextColored(get_confidence_color(m_ocr_confidence), "%d%%", m_ocr_confidence);
        ImGui::SameLine();
        HelpMarker("Confidence score");
    }

    ImGui::InputTextMultiline("##source",
                              &m_ocr_text,
                              ImVec2(-1, ImGui::GetTextLineHeight() * 10),
                              g_config->File.allow_ocr_edit ? 0 : ImGuiInputTextFlags_ReadOnly);

    if (!m_ocr_text.empty() && ImGui::Button("Copy Text"))
    {
        if (m_ocr_text.back() == '\n')
            m_ocr_text.pop_back();
        g_clipboard->CopyText(m_ocr_text);
    }

    ImGui::PopID();
}

void ScreenshotTool::DrawTranslationTools()
{
    static std::string lang_from{ g_config->File.lang_from };
    static std::string lang_to{ g_config->File.lang_to };
    static size_t      index_from  = 0;
    static size_t      index_to    = 0;
    static bool        first_frame = true;

    static std::string translated_text;

    static ImFont* font_from;
    static ImFont* font_to;

    if (first_frame)
    {
        if (getNameFromCode(lang_from) == "Unknown")
            SetError(InvalidLangFrom);
        else
            ClearError(InvalidLangFrom);

        if (getNameFromCode(lang_to) == "Unknown")
            SetError(InvalidLangTo);
        else
            ClearError(InvalidLangTo);

        font_from   = GetFontForLanguage(lang_from);
        font_to     = GetFontForLanguage(lang_to);
        first_frame = false;
    }

    ImGui::PushID("TranslationTools");
    ImGui::SeparatorText("Translation");

    auto createCombo =
        [&](const char* name, const ErrorFlag err, int start, std::string& lang, size_t& idx, ImFont* font) {
            ImGui::PushID(name);

            bool style_pushed = false;
            if (HasError(err))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                style_pushed = true;
            }

            if (ImGui::BeginCombo(name, getNameFromCode(lang).data(), ImGuiComboFlags_HeightLarge))
            {
                static ImGuiTextFilter filter;
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere();
                    filter.Clear();
                }
                ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F);
                filter.Draw("##Filter", -FLT_MIN);

                for (size_t i = start; i < GOOGLE_TRANSLATE_LANGUAGES_ARRAY.size(); ++i)
                {
                    const std::pair<const char*, const char*>& pair        = GOOGLE_TRANSLATE_LANGUAGES_ARRAY[i];
                    bool                                       is_selected = idx == i;
                    if (filter.PassFilter(pair.second))
                    {
                        if (ImGui::Selectable(pair.second, is_selected))
                        {
                            idx  = i;
                            lang = GOOGLE_TRANSLATE_LANGUAGES_ARRAY[idx].first;
                            font = GetFontForLanguage(lang);
                            (void)font;
                            ClearError(err);
                        }
                    }
                }
                ImGui::EndCombo();
            }

            if (style_pushed)
            {
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid Default Language!");
            }

            ImGui::PopID();
        };

    createCombo("From", InvalidLangFrom, 0, lang_from, index_from, font_from);

    ImGui::Spacing();

    // Ignore "Automatic" in To
    createCombo("To", InvalidLangTo, 1, lang_to, index_to, font_to);

    if (!(HasError(InvalidLangFrom) || HasError(InvalidLangTo)) && !m_to_translate_text.empty() &&
        ImGui::Button("Translate"))
    {
        const Result<std::string>& translation = translator->Translate(lang_from, lang_to, m_to_translate_text);
        if (!translation.ok())
        {
            SetError(FailedTranslation, translation.error().value);
        }
        else
        {
            translated_text = translation.get();
            ClearError(FailedTranslation);
        }
    }

    ImGui::SameLine();
    HelpMarker(
        "The translation is done by online services such as Google translate. It sucks at auto-detect and multi-line");

    static constexpr float spacing = 4.0f;   // Spacing between inputs
    static constexpr float padding = 10.0f;  // Padding on right side

    float available_width = ImGui::GetContentRegionAvail().x - spacing - padding;
    float width           = available_width / 2.0f;

    if (font_from)
    {
        ImGui::PushFont(font_from);
        ImGui::InputTextMultiline("##from", &m_to_translate_text, ImVec2(width, ImGui::GetTextLineHeight() * 10));
        ImGui::PopFont();
    }
    else
    {
        ImGui::InputTextMultiline("##from", &m_to_translate_text, ImVec2(width, ImGui::GetTextLineHeight() * 10));
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + spacing);

    if (HasError(FailedTranslation))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        translated_text = "Failed to translate text: " + m_err_texts[FailedTranslation];
        ImGui::InputTextMultiline(
            "##to", &translated_text, ImVec2(width, ImGui::GetTextLineHeight() * 10), ImGuiInputTextFlags_ReadOnly);

        ImGui::PopStyleColor();
    }

    else if (font_to)
    {
        ImGui::PushFont(font_to);
        ImGui::InputTextMultiline(
            "##to", &translated_text, ImVec2(width, ImGui::GetTextLineHeight() * 10), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopFont();
    }
    else
    {
        ImGui::InputTextMultiline(
            "##to", &translated_text, ImVec2(width, ImGui::GetTextLineHeight() * 10), ImGuiInputTextFlags_ReadOnly);
    }

    ImGui::PopID();
}

void ScreenshotTool::DrawBarDecodeTools()
{
    ImGui::PushID("BarDecodeTools");
    ImGui::SeparatorText("QR/Bar Decode");

    if (ImGui::Button("Extract Text"))
    {
        const Result<zbar_result_t>& scan = m_zbar_api.ExtractTextsCapture(GetFinalImage());
        if (!scan.ok())
        {
            SetError(FailedToExtractBarCode, scan.error().value);
        }
        else
        {
            m_zbar_scan = std::move(scan.get());
            for (const auto& data : m_zbar_scan.datas)
                m_barcode_text += data + "\n\n";
            ClearError(FailedToExtractBarCode);
        }
    }

    if (HasError(FailedToExtractBarCode))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        m_barcode_text = "Failed to extract text from bar code: " + m_err_texts[FailedToExtractBarCode];
        ImGui::InputTextMultiline("##barcode",
                                  &m_barcode_text,
                                  ImVec2(-1, ImGui::GetTextLineHeight() * 10),
                                  g_config->File.allow_ocr_edit ? 0 : ImGuiInputTextFlags_ReadOnly);

        ImGui::PopStyleColor();
    }
    else
    {
        if (!m_zbar_scan.datas.empty() && ImGui::TreeNode("Details"))
        {
            ImGui::Text("Detected barcodes:");
            for (const auto& [sym, count] : m_zbar_scan.symbologies)
                ImGui::BulletText("%s (x%d)", sym.c_str(), count);
            ImGui::TreePop();
        }
        ImGui::InputTextMultiline("##barcode",
                                  &m_barcode_text,
                                  ImVec2(-1, ImGui::GetTextLineHeight() * 10),
                                  g_config->File.allow_ocr_edit ? 0 : ImGuiInputTextFlags_ReadOnly);
    }

    if (!HasError(FailedToExtractBarCode) && !m_barcode_text.empty() && ImGui::Button("Copy Text"))
    {
        if (m_barcode_text.back() == '\n')
            m_barcode_text.pop_back();
        g_clipboard->CopyText(m_barcode_text);
    }

    ImGui::PopID();
}

void ScreenshotTool::DrawAnnotationToolbar()
{
    static int                   item_picker      = 0;
    static constexpr const char* color_pickers[2] = { "Bar - Square", "Wheel - Triangle" };

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_h = m_selection.get_height();

    // Position toolbar below the selection
    ImVec2 toolbar_pos(sel_x, sel_y + sel_h + 10);

    ImGui::SetNextWindowPos(toolbar_pos);
    ImGui::Begin("##annotation_toolbar",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);

    // Tool selection buttons
    auto DrawSetButton = [&](ToolType tool, const char* id, void* texture) {
        const bool  selected   = (m_current_tool == tool);
        ImTextureID texture_id = (ImTextureID)(intptr_t)texture;

        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));

        if (ImGui::ImageButton(id, texture_id, ImVec2(24, 24)))
            m_current_tool = selected ? ToolType::kNone : tool;

        if (selected)
            ImGui::PopStyleColor();

        // Right-click popup on this item
        if (selected && ImGui::BeginPopupContextItem())
        {
            m_tool_thickness[idx(m_current_tool)] = std::clamp(m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f);

            static ImVec4              color(1, 0, 0, 1);
            static ImGuiColorEditFlags color_picker_flags = ImGuiColorEditFlags_AlphaBar;

            ImGui::TextUnformatted("Annotation Settings");
            ImGui::Separator();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("##thickness", &m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f, "%.2f");
            ImGui::SameLine();
            ImGui::TextUnformatted("Thickness");

            ImGui::Combo("Color picker", &item_picker, color_pickers, IM_ARRAYSIZE(color_pickers));
            switch (item_picker)
            {
                case 0:
                    color_picker_flags |= ImGuiColorEditFlags_PickerHueBar;
                    color_picker_flags &= ~ImGuiColorEditFlags_PickerHueWheel;
                    break;
                case 1:
                    color_picker_flags |= ImGuiColorEditFlags_PickerHueWheel;
                    color_picker_flags &= ~ImGuiColorEditFlags_PickerHueBar;
                    break;
            }
            ImGui::CheckboxFlags("Disable alpha edit", &color_picker_flags, ImGuiColorEditFlags_NoAlpha);
            if (!(color_picker_flags & ImGuiColorEditFlags_NoAlpha))
                ImGui::CheckboxFlags("Show alpha bar", &color_picker_flags, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorPicker4("Color", reinterpret_cast<float*>(&color), color_picker_flags);

            m_current_color = ImGui::ColorConvertFloat4ToU32(color);
            ImGui::EndPopup();
        }

        ImGui::SameLine();
    };

    DrawSetButton(ToolType::Arrow, "##Arrow", tool_textures[idx(ToolType::Arrow)]);
    DrawSetButton(ToolType::Rectangle, "##Rectangle", tool_textures[idx(ToolType::Rectangle)]);
    DrawSetButton(ToolType::RectangleFilled, "##Rectangle_filled", tool_textures[idx(ToolType::RectangleFilled)]);
    DrawSetButton(ToolType::Circle, "##Circle", tool_textures[idx(ToolType::Circle)]);
    DrawSetButton(ToolType::CircleFilled, "##Circle_filled", tool_textures[idx(ToolType::CircleFilled)]);
    DrawSetButton(ToolType::Line, "##Line", tool_textures[idx(ToolType::Line)]);
    DrawSetButton(ToolType::Pencil, "##Pencil", tool_textures[idx(ToolType::Pencil)]);

    ImGui::Separator();

    ImGui::SameLine();
    if (ImGui::Button("Undo") && !m_annotations.empty())
        m_annotations.pop_back();

    ImGui::End();
}

void ScreenshotTool::DrawAnnotations()
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const float dpi       = m_io.DisplayFramebufferScale.x;

    auto draw_line = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        draw_list->AddLine(p1, p2, ann.color, t);
    };

    auto draw_rectangle = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        ImVec2 min(std::min(p1.x, p2.x), std::min(p1.y, p2.y));
        ImVec2 max(std::max(p1.x, p2.x), std::max(p1.y, p2.y));
        draw_list->AddRect(min, max, ann.color, 0.0f, 0, t);
    };

    auto draw_rectangle_filled = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2) {
        ImVec2 min(std::min(p1.x, p2.x), std::min(p1.y, p2.y));
        ImVec2 max(std::max(p1.x, p2.x), std::max(p1.y, p2.y));
        draw_list->AddRectFilled(min, max, ann.color, 0.0f, 0);
    };

    auto draw_circle = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        float dx     = p2.x - p1.x;
        float dy     = p2.y - p1.y;
        float radius = std::sqrt(dx * dx + dy * dy);
        draw_list->AddCircle(p1, radius, ann.color, 0, t);
    };

    auto draw_circle_filled = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2) {
        float dx     = p2.x - p1.x;
        float dy     = p2.y - p1.y;
        float radius = std::sqrt(dx * dx + dy * dy);
        draw_list->AddCircleFilled(p1, radius, ann.color, 0);
    };

    auto draw_pencil = [&](const annotation_t& ann, const float t) {
        if (ann.points.size() >= 2)
        {
            std::vector<ImVec2> pts;
            pts.reserve(ann.points.size());
            for (const auto& p : ann.points)
                pts.emplace_back(p.x, p.y);
            draw_list->AddPolyline(pts.data(), static_cast<int>(pts.size()), ann.color, ImDrawFlags_None, t);
        }
    };

    auto draw_arrow = [&](const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
        ImVec2 v(p2.x - p1.x, p2.y - p1.y);
        float  len = sqrtf(v.x * v.x + v.y * v.y);
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
        draw_list->AddLine(p1, base, ann.color, t);

        // head
        draw_list->AddTriangleFilled(p2, left, right, ann.color);
    };

    for (const auto& ann : m_annotations)
    {
        const ImVec2 p1(ann.start.x, ann.start.y);
        const ImVec2 p2(ann.end.x, ann.end.y);
        const float  t = ann.thickness * dpi;

        switch (ann.type)
        {
            case ToolType::Line:            draw_line(ann, p1, p2, t); break;
            case ToolType::Arrow:           draw_arrow(ann, p1, p2, t); break;
            case ToolType::Rectangle:       draw_rectangle(ann, p1, p2, t); break;
            case ToolType::RectangleFilled: draw_rectangle_filled(ann, p1, p2); break;
            case ToolType::Circle:          draw_circle(ann, p1, p2, t); break;
            case ToolType::CircleFilled:    draw_circle_filled(ann, p1, p2); break;
            case ToolType::Pencil:          draw_pencil(ann, t); break;

            default: break;
        }
    }

    // Render current annotation being drawn
    if (m_is_drawing)
    {
        ImVec2      p1(m_current_annotation.start.x, m_current_annotation.start.y);
        ImVec2      p2(m_current_annotation.end.x, m_current_annotation.end.y);
        const float t = m_current_annotation.thickness * dpi;

        switch (m_current_annotation.type)
        {
            case ToolType::Line:            draw_line(m_current_annotation, p1, p2, t); break;
            case ToolType::Arrow:           draw_arrow(m_current_annotation, p1, p2, t); break;
            case ToolType::Rectangle:       draw_rectangle(m_current_annotation, p1, p2, t); break;
            case ToolType::RectangleFilled: draw_rectangle_filled(m_current_annotation, p1, p2); break;
            case ToolType::Circle:          draw_circle(m_current_annotation, p1, p2, t); break;
            case ToolType::CircleFilled:    draw_circle_filled(m_current_annotation, p1, p2); break;
            case ToolType::Pencil:          draw_pencil(m_current_annotation, t); break;

            default: break;
        }
    }
}

void ScreenshotTool::Cancel()
{
    m_state = ToolState::Idle;

    auto delete_texture = [](void* tex) {
        if (tex)
        {
            GLuint texture = (GLuint)(intptr_t)tex;
            glDeleteTextures(1, &texture);
            tex = nullptr;
        }
    };

    delete_texture(m_texture_id);
    for (auto& tex : tool_textures)
        delete_texture(tex);

    // (just clears our references, not the actual ImGui fonts)
    m_font_cache.clear();

    if (m_on_cancel)
        m_on_cancel();
}

bool ScreenshotTool::OpenImage(const std::string& path)
{
    const Result<capture_result_t>& cap = load_image_rgba(path);
    if (!cap.ok())
    {
        error("Failed to load image: {}", cap.error());
        return false;
    }

    m_screenshot = std::move(cap.get());

    // Recreate texture (CreateTexture() already deletes the old ones)
    const Result<void*>& r = CreateTexture(m_texture_id, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    if (!r.ok())
    {
        error("Failed create openGL texture: " + r.error().value);
        return false;
    }

    m_texture_id = r.get();
    fit_to_screen(m_screenshot);

    // Reset everything
    m_state           = ToolState::Selecting;
    m_handle_hover    = HandleHovered::kNone;
    m_dragging_handle = HandleHovered::kNone;

    m_is_selecting         = false;
    m_selection            = {};
    m_drag_start_selection = {};
    m_drag_start_mouse     = {};
    m_image_origin         = {};
    m_image_end            = {};

    m_ocr_text.clear();
    m_to_translate_text.clear();
    m_barcode_text.clear();

    ClearError(FailedToInitOcr);
    ClearError(InvalidPath);
    ClearError(InvalidModel);
    ClearError(FailedTranslation);
    ClearError(InvalidLangFrom);
    ClearError(InvalidLangTo);
    ClearError(FailedToExtractBarCode);

    return true;
}

capture_result_t ScreenshotTool::GetFinalImage()
{
    UpdateWindowBg();

    region_t region{
        static_cast<int>(m_selection.get_x() - m_image_origin.x),
        static_cast<int>(m_selection.get_y() - m_image_origin.y),
        static_cast<int>(m_selection.get_width()),
        static_cast<int>(m_selection.get_height()),
    };

    capture_result_t result;
    result.w = region.width;
    result.h = region.height;
    result.data.resize(static_cast<size_t>(region.width) * region.height * 4);

    std::span<const uint8_t> src(m_screenshot.view());
    std::span<uint8_t>       dst(result.data);

    const int src_width = m_screenshot.w;
    const int dst_width = region.width;

    // Calculate bounds
    const int start_y = std::max(0, -region.y);
    const int end_y   = std::min(region.height, m_screenshot.h - region.y);
    const int start_x = std::max(0, -region.x);
    const int end_x   = std::min(region.width, m_screenshot.w - region.x);

    // Copy only the valid region
    const size_t bytes_to_copy = static_cast<size_t>(end_x - start_x) * 4;

    for (int y = start_y; y < end_y; ++y)
    {
        const int src_y = region.y + y;

        const size_t src_row_start = (static_cast<size_t>(src_y) * src_width + (region.x + start_x)) * 4;
        const size_t dst_row_start = (static_cast<size_t>(y) * dst_width + start_x) * 4;

        if (src_row_start + bytes_to_copy > src.size() || dst_row_start + bytes_to_copy > dst.size())
            return result;

        std::memcpy(dst.data() + dst_row_start, src.data() + src_row_start, bytes_to_copy);
    }

    // Render annotations to the final image
    float offset_x = m_selection.get_x();
    float offset_y = m_selection.get_y();

    auto SetPixel = [&](int x, int y, uint32_t color) {
        if (x >= 0 && x < result.w && y >= 0 && y < result.h)
        {
            size_t idx           = (static_cast<size_t>(y) * result.w + x) * 4;
            result.data[idx + 0] = (color >> 0) & 0xFF;   // R
            result.data[idx + 1] = (color >> 8) & 0xFF;   // G
            result.data[idx + 2] = (color >> 16) & 0xFF;  // B
            result.data[idx + 3] = (color >> 24) & 0xFF;  // A
        }
    };

    auto DrawLine = [&](int x0, int y0, int x1, int y1, uint32_t color, float thickness) {
        // Bresenham's line algorithm with thickness
        int dx     = std::abs(x1 - x0);
        int dy     = std::abs(y1 - y0);
        int sx     = x0 < x1 ? 1 : -1;
        int sy     = y0 < y1 ? 1 : -1;
        int err    = dx - dy;
        int radius = static_cast<int>(thickness / 2.0f);

        while (true)
        {
            // Draw thick point
            for (int oy = -radius; oy <= radius; ++oy)
                for (int ox = -radius; ox <= radius; ++ox)
                    if (ox * ox + oy * oy <= radius * radius)
                        SetPixel(x0 + ox, y0 + oy, color);

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

    for (const auto& ann : m_annotations)
    {
        int x1 = static_cast<int>(ann.start.x - offset_x);
        int y1 = static_cast<int>(ann.start.y - offset_y);
        int x2 = static_cast<int>(ann.end.x - offset_x);
        int y2 = static_cast<int>(ann.end.y - offset_y);

        switch (ann.type)
        {
            case ToolType::Line:
            case ToolType::Arrow:
                DrawLine(x1, y1, x2, y2, ann.color, ann.thickness);
                if (ann.type == ToolType::Arrow)
                {
                    // Draw arrowhead
                    float dx  = x2 - x1;
                    float dy  = y2 - y1;
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.1f)
                    {
                        dx /= len;
                        dy /= len;
                        float arrow_size = 15.0f + ann.thickness;
                        int   ax1        = static_cast<int>(x2 - arrow_size * dx + arrow_size * 0.5f * dy);
                        int   ay1        = static_cast<int>(y2 - arrow_size * dy - arrow_size * 0.5f * dx);
                        int   ax2        = static_cast<int>(x2 - arrow_size * dx - arrow_size * 0.5f * dy);
                        int   ay2        = static_cast<int>(y2 - arrow_size * dy + arrow_size * 0.5f * dx);
                        DrawLine(x2, y2, ax1, ay1, ann.color, ann.thickness);
                        DrawLine(x2, y2, ax2, ay2, ann.color, ann.thickness);
                    }
                }
                break;

            case ToolType::Rectangle:
                DrawLine(x1, y1, x2, y1, ann.color, ann.thickness);
                DrawLine(x2, y1, x2, y2, ann.color, ann.thickness);
                DrawLine(x2, y2, x1, y2, ann.color, ann.thickness);
                DrawLine(x1, y2, x1, y1, ann.color, ann.thickness);
                break;

            case ToolType::Circle:
            {
                int cx     = x1;
                int cy     = y1;
                int radius = static_cast<int>(std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)));

                // Midpoint circle algorithm
                int x       = radius;
                int y       = 0;
                int err     = 0;
                int thick_r = static_cast<int>(ann.thickness / 2.0f);

                while (x >= y)
                {
                    for (int oy = -thick_r; oy <= thick_r; ++oy)
                        for (int ox = -thick_r; ox <= thick_r; ++ox)
                            if (ox * ox + oy * oy <= thick_r * thick_r)
                            {
                                SetPixel(cx + x + ox, cy + y + oy, ann.color);
                                SetPixel(cx + y + ox, cy + x + oy, ann.color);
                                SetPixel(cx - y + ox, cy + x + oy, ann.color);
                                SetPixel(cx - x + ox, cy + y + oy, ann.color);
                                SetPixel(cx - x + ox, cy - y + oy, ann.color);
                                SetPixel(cx - y + ox, cy - x + oy, ann.color);
                                SetPixel(cx + y + ox, cy - x + oy, ann.color);
                                SetPixel(cx + x + ox, cy - y + oy, ann.color);
                            }

                    y += 1;
                    err += 1 + 2 * y;
                    if (2 * (err - x) + 1 > 0)
                    {
                        x -= 1;
                        err += 1 - 2 * x;
                    }
                }
                break;
            }

            case ToolType::Pencil:
                for (size_t i = 1; i < ann.points.size(); ++i)
                {
                    int px1 = static_cast<int>(ann.points[i - 1].x - offset_x);
                    int py1 = static_cast<int>(ann.points[i - 1].y - offset_y);
                    int px2 = static_cast<int>(ann.points[i].x - offset_x);
                    int py2 = static_cast<int>(ann.points[i].y - offset_y);
                    DrawLine(px1, py1, px2, py2, ann.color, ann.thickness);
                }
                break;

            default: break;
        }
    }

    return result;
}

void ScreenshotTool::UpdateWindowBg()
{
    // Calculate where the screenshot will be drawn (centered)
    // clang-format off
    auto* vp = ImGui::GetMainViewport();
    ImVec2 image_size(
        static_cast<float>(m_screenshot.w),
        static_cast<float>(m_screenshot.h)
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

ImFont* ScreenshotTool::GetFontForLanguage(const std::string& lang_code)
{
    // Check cache first
    auto it = m_font_cache.find(lang_code);
    if (it != m_font_cache.end())
    {
        debug("cached {}: {}", lang_code, it->second.font_path);
        return it->second.font;
    }

    const auto& font_path = get_lang_font_path(lang_code);
    debug("font_path {}: {}", lang_code, font_path.string());
    if (font_path.empty())
    {
        // Cache null result
        m_font_cache[lang_code] = { "", nullptr, true };
        return nullptr;
    }

    ImFont* font =
        m_io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 16.0f, nullptr, m_io.Fonts->GetGlyphRangesDefault());

    // Cache the result
    m_font_cache[lang_code] = { font_path.string(), font, true };

    // Rebuild font atlas - texture is handled automatically by backend
    if (font)
        m_io.Fonts->Build();

    return font;
}

Result<void*> ScreenshotTool::CreateTexture(void* tex, std::span<const uint8_t> data, int w, int h)
{
    // Delete old texture first
    if (tex)
    {
        GLuint old_texture = (GLuint)(intptr_t)tex;
        glDeleteTextures(1, &old_texture);
    }

    GLuint texture;
    glGenTextures(1, &texture);
    int err = glGetError();
    if (err != GL_NO_ERROR)
        return Err("glGetError() returned error: " + fmt::to_string(err));

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());

    tex = (void*)(intptr_t)texture;
    return Ok(tex);
}
