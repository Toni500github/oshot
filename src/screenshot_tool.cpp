#include "screenshot_tool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "config.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3_loader.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "langs.hpp"
#include "screen_capture.hpp"
#include "socket.hpp"
#include "translation.hpp"
#include "util.hpp"

#ifdef None
#undef None
#endif

using namespace std::chrono_literals;

static ImVec2 origin(0, 0);

static std::unique_ptr<Translator> translator;
std::unique_ptr<SocketSender>      sender;

#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>

static bool stdin_has_data()
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    timeval tv{};
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}
#else
#include <io.h>
#include <windows.h>

static bool stdin_has_data()
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD available = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr))
        return false;

    return available > 0;
}
#endif

static std::vector<std::string> GetTrainingDataList(const std::string& path)
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

bool ScreenshotTool::Start()
{
    translator = std::make_unique<Translator>();
    sender     = std::make_unique<SocketSender>();

    SetError(WarnConnLauncher);
    bool stdin_data_exist = stdin_has_data();
    if (config->_source_file.empty() && !stdin_data_exist)
    {
        switch (get_session_type())
        {
            case X11:        m_screenshot = capture_full_screen_x11(); break;
            case WAYLAND:    m_screenshot = capture_full_screen_wayland(); break;
            case OS_WINDOWS: m_screenshot = capture_full_screen_windows(); break;
            default:         ;
        }
    }
    else
    {
        m_screenshot = load_image_rgba(stdin_data_exist, config->_source_file);
    }

    if (!m_screenshot.success || m_screenshot.data.empty() || !m_screenshot.error_msg.empty())
    {
        m_state = ToolState::Idle;
        error("Failed to do data screenshot: {}", m_screenshot.error_msg);
        return false;
    }

    return true;
}

bool ScreenshotTool::StartWindow()
{
    m_io             = ImGui::GetIO();
    m_connect_future = std::async(std::launch::async, [&] {
        return sender->Start();  // blocking connect()
    });

    m_state = ToolState::Selecting;
    CreateTexture();
    fit_to_screen(m_screenshot);
    if (!m_screenshot.success || m_screenshot.data.empty() || !m_screenshot.error_msg.empty() || !m_texture_id)
    {
        m_state = ToolState::Idle;
        error("Failed to do data screenshot: {}", m_screenshot.error_msg);
        return false;
    }
    return true;
}

void ScreenshotTool::RenderOverlay()
{
    if (!m_connect_done)
    {
        if (m_connect_future.valid() && m_connect_future.wait_for(0ms) == std::future_status::ready)
        {
            bool success   = m_connect_future.get();
            m_connect_done = true;

            ClearError(WarnConnLauncher);

            if (!success)
                SetError(NoLauncher);
        }
    }

    // Create fullscreen overlay window
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(m_io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, origin);
    ImGui::Begin("Screenshot Tool",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground);

    // Draw the screenshot as background
    UpdateWindowBg();
    ImGui::GetBackgroundDrawList()->AddImage(m_texture_id, m_image_origin, m_image_end);

    if (m_state == ToolState::Selecting || m_state == ToolState::Selected || m_state == ToolState::Resizing)
    {
        if (!m_is_hovering_ocr)
        {
            HandleSelectionInput();
            DrawDarkOverlay();
        }
        DrawSelectionBorder();
    }

    ImGui::End();
    ImGui::PopStyleVar();

    if (m_state == ToolState::Selected)
    {
        ImGui::Begin("Text tools", nullptr, ImGuiWindowFlags_MenuBar);
        ImVec2 window_pos  = ImGui::GetWindowPos();
        ImVec2 window_size = ImGui::GetWindowSize();
        m_is_hovering_ocr  = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ||
                            (ImGui::IsMouseHoveringRect(
                                window_pos, ImVec2(window_pos.x + window_size.x, window_pos.y + window_size.y)));
        DrawMenuItems();
        DrawOcrTools();
        DrawTranslationTools();
        ImGui::End();
    }
}

void ScreenshotTool::HandleSelectionInput()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    float         sel_x     = m_selection.get_x();
    float         sel_y     = m_selection.get_y();
    float         sel_w     = m_selection.get_width();
    float         sel_h     = m_selection.get_height();
    ImRect        selection_rect(ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h));

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_is_selecting)
    {
        // Check if we're starting to resize from a handle
        if (m_handle_hover != HandleHovered::None)
        {
            m_dragging_handle      = m_handle_hover;
            m_drag_start_mouse     = mouse_pos;
            m_drag_start_selection = m_selection;
            m_state                = ToolState::Resizing;
            m_is_selecting         = true;
        }
        // Check if we're clicking inside the selection to move it
        else if (selection_rect.Contains(mouse_pos))
        {
            m_dragging_handle      = HandleHovered::Move;
            m_drag_start_mouse     = mouse_pos;
            m_drag_start_selection = m_selection;
            m_state                = ToolState::Resizing;
            m_is_selecting         = true;
        }
        else
        {
            // Start new selection
            m_selection.start = { mouse_pos.x, mouse_pos.y };
            m_selection.end   = m_selection.start;
            m_is_selecting    = true;
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
        m_dragging_handle = HandleHovered::None;

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
    const ImVec2 mouse_pos = ImGui::GetMousePos();
    m_handle_hover         = HandleHovered::None;
    m_handle_pos           = ImVec2(0, 0);

    if (m_state != ToolState::Selected && m_state != ToolState::Resizing)
        return;

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_w = m_selection.get_width();
    float sel_h = m_selection.get_height();

    const float hover_half = HANDLE_HOVER_SIZE / 2.0f;

    const std::array<HandleInfo, 8> handles = {
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
            m_handle_pos   = { handle.pos.x * 2, handle.pos.y * 2 };
            break;
        }
    }
}

void ScreenshotTool::UpdateCursor()
{
    if (m_handle_hover != HandleHovered::None || m_dragging_handle != HandleHovered::None)
    {
        HandleHovered handle = (m_dragging_handle != HandleHovered::None) ? m_dragging_handle : m_handle_hover;

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
    else if ((m_state == ToolState::Selected || m_state == ToolState::Resizing) && !m_is_hovering_ocr)
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
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

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

    if (!config->_enable_handles)
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

// from ImGui::GetShortcutRoutingData(ImGuiKeyChord key_chord)
// Majority of shortcuts will be Key + any number of Mods
// We accept _Single_ mod with ImGuiKey_None.
//  - Shortcut(ImGuiKey_S | ImGuiMod_Ctrl);                    // Legal
//  - Shortcut(ImGuiKey_S | ImGuiMod_Ctrl | ImGuiMod_Shift);   // Legal
//  - Shortcut(ImGuiMod_Ctrl);                                 // Legal
//  - Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift);                // Not legal
void ScreenshotTool::DrawMenuItems()
{
    static bool show_about = false;

    if (ImGui::BeginMenuBar())
    {
        // Handle shortcuts FIRST, before drawing menus
        if (ImGui::Shortcut(ImGuiKey_Escape))
            Cancel();

        if (ImGui::Shortcut(ImGuiKey_E | ImGuiMod_Ctrl))
            config->allow_ocr_edit = !config->allow_ocr_edit;

        if (ImGui::Shortcut(ImGuiKey_G | ImGuiMod_Ctrl))
            config->_enable_handles = !config->_enable_handles;

        if (ImGui::Shortcut(ImGuiKey_S | ImGuiMod_Ctrl))
            if (m_on_complete)
                m_on_complete(SavingOp::SAVE_FILE, GetFinalImage());

        if (ImGui::Shortcut(ImGuiKey_C | ImGuiMod_Ctrl | ImGuiMod_Shift))
            if (!HasError(NoLauncher) && m_on_complete)
                m_on_complete(SavingOp::SAVE_CLIPBOARD, GetFinalImage());

        // Now draw the menus
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Save Image", "CTRL+S"))
                if (m_on_complete)
                    m_on_complete(SavingOp::SAVE_FILE, GetFinalImage());
            if (ImGui::MenuItem("Copy Image", "CTRL+SHIFT+C"))
            {
                if (HasError(NoLauncher))
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                       "Please launch oshot with its launcher, in order to copy text/images");
                else if (m_on_complete)
                    m_on_complete(SavingOp::SAVE_CLIPBOARD, GetFinalImage());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "ESC"))
                Cancel();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            ImGui::MenuItem("View Handles", "CTRL+G", &config->_enable_handles);
            ImGui::MenuItem("Allow OCR edit", "CTRL+E", &config->allow_ocr_edit);
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
        ImVec2 window_pos  = ImGui::GetWindowPos();
        ImVec2 window_size = ImGui::GetWindowSize();
        m_is_hovering_ocr  = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootWindow) ||
                            (ImGui::IsMouseHoveringRect(
                                window_pos, ImVec2(window_pos.x + window_size.x, window_pos.y + window_size.y)));

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
    static std::string ocr_path{ config->ocr_path };
    static std::string ocr_model{ config->ocr_model };
    static size_t      item_selected_idx = 0;
    static bool        first_frame       = true;

    ImGui::SeparatorText("OCR");

    static std::vector<std::string> list{ "" };
    auto                            check_list = [&]() {
        list = GetTrainingDataList(ocr_path);
        if (list.empty())
            SetError(InvalidPath);
        else
            ClearError(InvalidPath);
    };

    if (first_frame)
    {
        check_list();
        if (std::find(list.begin(), list.end(), ocr_model) == list.end())
            SetError(InvalidModel);
        first_frame = false;
    }

    if (HasError(InvalidPath))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));

        if (ImGui::InputText("Path", &ocr_path))
            check_list();

        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid/Empty!");

        ImGui::TextDisabled("Model: %s", ocr_model.empty() ? "Select path first" : ocr_model.c_str());

        goto end;
    }
    else
    {
        if (ImGui::InputText("Path", &ocr_path))
            check_list();
    }

    if (HasError(InvalidModel))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
    }

    if (!HasError(InvalidPath) && ImGui::BeginCombo("Model", ocr_model.c_str(), ImGuiComboFlags_HeightLarge))
    {
        static ImGuiTextFilter filter;
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
            filter.Clear();
        }

        ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F);
        filter.Draw("##Filter", -FLT_MIN);

        for (size_t i = 0; i < list.size(); ++i)
        {
            bool is_selected = item_selected_idx == i;
            if (filter.PassFilter(list[i].c_str()))
                if (ImGui::Selectable(list[i].c_str(), is_selected))
                    item_selected_idx = i;
        }
        ocr_model = list[item_selected_idx];
        ClearError(InvalidModel);
        ImGui::EndCombo();
    }
    else if (HasError(InvalidPath))
    {
        // If combo is not open, we might need to update ocr_model from item_selected_idx
        if (!list.empty() && item_selected_idx < list.size())
            ocr_model = list[item_selected_idx];
    }

    if (HasError(InvalidModel))
    {
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid!");
    }

end:
    if (!HasError(InvalidModel) && !HasError(InvalidPath) && ImGui::Button("Extract Text"))
    {
        if (std::find(list.begin(), list.end(), ocr_model) == list.end())
        {
            SetError(InvalidModel);
        }
        else if (!m_api.Configure(ocr_path.c_str(), ocr_model.c_str()))
        {
            SetError(InitOcr);
        }
        else
        {
            const auto& text = m_api.RecognizeCapture(GetFinalImage());
            if (text)
                m_ocr_text = m_to_translate_text = *text;
            ClearError(InitOcr);
        }
    }

    if (HasError(InitOcr))
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Failed to init OCR!");
    }

    ImGui::InputTextMultiline("##source",
                              &m_ocr_text,
                              ImVec2(-1, ImGui::GetTextLineHeight() * 10),
                              config->allow_ocr_edit ? 0 : ImGuiInputTextFlags_ReadOnly);

    if (HasError(WarnConnLauncher))
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Connecting to launcher...");
    }
    else if (HasError(NoLauncher))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                           "Please launch oshot with its launcher, in order to copy text/images");
    }
    else if (!m_ocr_text.empty() && ImGui::Button("Copy Text"))
    {
        if (m_ocr_text.back() == '\n')
            m_ocr_text.pop_back();
        sender->Send(m_ocr_text);
        ClearError(NoLauncher);
    }
}

void ScreenshotTool::DrawTranslationTools()
{
    static std::string lang_from{ config->lang_from };
    static std::string lang_to{ config->lang_to };
    static size_t      index_from  = 0;
    static size_t      index_to    = 0;
    static bool        first_frame = true;

    static std::string translated_text;

    static ImFont* font_from;
    static ImFont* font_to;

    ImGui::SeparatorText("Translation");

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

        font_from   = GetOrLoadFontForLanguage(lang_from);
        font_to     = GetOrLoadFontForLanguage(lang_to);
        first_frame = false;
    }

    auto createCombo =
        [&](const char* name, const ErrorState err, int start, std::string& lang, size_t& idx, ImFont* font) {
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
                            font = GetOrLoadFontForLanguage(lang);
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
        const auto& translation = translator->Translate(lang_from, lang_to, m_to_translate_text);
        if (translation)
        {
            translated_text = *translation;
            ClearError(FailedTranslation);
        }
        else
        {
            SetError(FailedTranslation);
        }
    }

    ImGui::SameLine();
    HelpMarker(
        "The translation is done by online services such as Google translate. It sucks at auto-detect and multi-line");

    static constexpr float spacing = 4.0f;   // Spacing between inputs
    static constexpr float padding = 10.0f;  // Padding on right side

    // Calculate available width minus spacing and padding
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
        translated_text = "Failed to translate text";
        ImGui::InputTextMultiline(
            "##to", &translated_text, ImVec2(width, ImGui::GetTextLineHeight() * 10), ImGuiInputTextFlags_ReadOnly);

        ImGui::PopStyleColor();
        return;
    }

    if (font_to)
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
    result.region  = region;
    result.success = true;
    result.data.resize(region.width * region.height * 4);

    const uint8_t* src_data = m_screenshot.data.data();
    uint8_t*       dst_data = result.data.data();

    int src_width = m_screenshot.region.width;
    int dst_width = region.width;

    // Calculate bounds
    int start_y = std::max(0, -region.y);
    int end_y   = std::min(region.height, m_screenshot.region.height - region.y);
    int start_x = std::max(0, -region.x);
    int end_x   = std::min(region.width, m_screenshot.region.width - region.x);

    std::fill(result.data.begin(), result.data.end(), 0);

    // Copy only the valid region
    for (int y = start_y; y < end_y; ++y)
    {
        int src_y         = region.y + y;
        int src_row_start = (src_y * src_width + region.x + start_x) * 4;
        int dst_row_start = (y * dst_width + start_x) * 4;

        // Copy the entire valid row segment
        int bytes_to_copy = (end_x - start_x) * 4;
        std::memcpy(&dst_data[dst_row_start], &src_data[src_row_start], bytes_to_copy);
    }

    return result;
}

bool ScreenshotTool::HasError(ErrorState err)
{
    return m_err_state & err;
}

void ScreenshotTool::SetError(ErrorState err)
{
    m_err_state |= err;
}

void ScreenshotTool::ClearError(ErrorState err)
{
    m_err_state &= ~err;
}

void ScreenshotTool::UpdateWindowBg()
{
    // Calculate where the screenshot will be drawn (centered)
    // clang-format off
    auto* vp = ImGui::GetMainViewport();
    ImVec2 image_size(
        static_cast<float>(m_screenshot.region.width),
        static_cast<float>(m_screenshot.region.height)
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

ImFont* ScreenshotTool::GetOrLoadFontForLanguage(const std::string& lang_code)
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

bool ScreenshotTool::CreateTexture()
{
    if (m_screenshot.data.empty() || !m_screenshot.success)
        return false;

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 m_screenshot.region.width,
                 m_screenshot.region.height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 m_screenshot.data.data());

    m_texture_id = (void*)(intptr_t)texture;
    return true;
}
