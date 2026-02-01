#include "screenshot_tool.hpp"

#include <tesseract/publictypes.h>
#include <zbar.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>
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
#include "tinyfiledialogs.h"
#include "translation.hpp"
#include "util.hpp"

#ifndef GL_NO_ERROR
#  define GL_NO_ERROR 0
#endif

using namespace std::chrono_literals;

static ImVec2 origin(0, 0);

static std::unique_ptr<Translator> translator;

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
    Result<capture_result_t> result{ Err() };

    SessionType type = get_session_type();
    g_clipboard      = std::make_unique<Clipboard>(type);

    if (!g_config->Runtime.source_file.empty())
    {
        result = load_image_rgba(g_config->Runtime.source_file);
    }
    else
    {
        switch (type)
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
    return Ok();
}

Result<> ScreenshotTool::StartWindow()
{
    m_io                = ImGui::GetIO();
    m_state             = ToolState::Selecting;
    const Result<>& res = CreateTexture();
    if (!res.ok())
        return Err("Failed create openGL texture: " + res.error().value);

    fit_to_screen(m_screenshot);
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
    if (m_input_owner != InputOwner::Selection && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ui_blocks_selection())
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
    if (m_handle_hover != HandleHovered::kNone || m_dragging_handle != HandleHovered::kNone)
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
    else if ((m_state == ToolState::Selected || m_state == ToolState::Resizing))
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
        translated_text = "Failed to translate text: " + m_curr_err_text;
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
        m_barcode_text = "Failed to extract text from bar code: " + m_curr_err_text;
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

void ScreenshotTool::Cancel()
{
    m_state = ToolState::Idle;
    if (m_texture_id)
    {
        GLuint texture = (GLuint)(intptr_t)m_texture_id;
        glDeleteTextures(1, &texture);
        m_texture_id = nullptr;
    }

    // (just clears our references, not the actual ImGui fonts)
    m_font_cache.clear();

    if (m_on_cancel)
    {
        m_on_cancel();
    }
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
    if (!CreateTexture().ok() || !m_texture_id)
    {
        error("Failed create openGL texture");
        return false;
    }

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

Result<> ScreenshotTool::CreateTexture()
{
    // Delete old texture first
    if (m_texture_id)
    {
        GLuint old_texture = (GLuint)(intptr_t)m_texture_id;
        glDeleteTextures(1, &old_texture);
    }

    GLuint texture;
    glGenTextures(1, &texture);
    int err = glGetError();
    if (err != GL_NO_ERROR)
        return Err("glGetError() returned error: " + fmt::to_string(err));

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 m_screenshot.w,
                 m_screenshot.h,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 m_screenshot.view().data());

    m_texture_id = (void*)(intptr_t)texture;
    return Ok();
}
