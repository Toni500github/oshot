#include "screenshot_tool.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_impl_opengl3_loader.h"

static ImVec2 origin(0, 0);

bool ScreenshotInteraction::Start()
{
    switch (get_session_type())
    {
        case X11:     m_screenshot = capture_full_screen_x11(); break;
        case WAYLAND: m_screenshot = capture_full_screen_wayland(); break;
        default:      ;
    }

    if (!m_screenshot.success || m_screenshot.data.empty() || !m_screenshot.error_msg.empty())
    {
        m_state = ToolState::Idle;
        std::cerr << "Failed to do data screenshot: " << m_screenshot.error_msg << std::endl;
        return false;
    }
    m_state = ToolState::Selecting;
    CreateTexture();
    return true;
}

bool ScreenshotInteraction::RenderOverlay()
{
    if (!IsActive())
        return false;

    // Create fullscreen overlay window
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(m_io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, origin);
    ImGui::Begin("Screenshot Tool",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollbar);

    if (m_screenshot.data.empty() || !m_screenshot.success || !m_texture_id)
        return false;

    // Draw the screenshot as background
    ImGui::GetBackgroundDrawList()->AddImage(
        m_texture_id, origin, ImVec2(m_screenshot.region.width, m_screenshot.region.height));

    if (m_state == ToolState::Selecting || m_state == ToolState::Selected)
    {
        HandleSelectionInput();
        DrawDarkOverlay();
        DrawSelectionBorder();
        DrawSizeIndicator();
    }

    ImGui::End();
    ImGui::PopStyleVar();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        Cancel();
        return false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_state == ToolState::Selected)
    {
        if (m_on_complete)
            m_on_complete(GetFinalImage());

        m_state = ToolState::Idle;
        return false;
    }

    return true;
}

void ScreenshotInteraction::HandleSelectionInput()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_is_selecting)
    {
        m_selection.start = { mouse_pos.x, mouse_pos.y };
        m_selection.end   = m_selection.start;
        m_is_selecting    = true;
        m_state           = ToolState::Selecting;
    }

    if (m_is_selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        m_selection.end = { mouse_pos.x, mouse_pos.y };

    if (m_is_selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_is_selecting = false;
        if (m_selection.get_width() > 10 && m_selection.get_height() > 10)
            m_state = ToolState::Selected;
    }
}

void ScreenshotInteraction::DrawDarkOverlay()
{
    ImDrawList* draw_list   = ImGui::GetForegroundDrawList();
    ImVec2      screen_size = m_io.DisplaySize;

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_w = m_selection.get_width();
    float sel_h = m_selection.get_height();

    ImU32 dark_color = IM_COL32(0, 0, 0, 120);

    // Top rectangle
    draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(screen_size.x, sel_y), dark_color);

    // Bottom rectangle
    draw_list->AddRectFilled(ImVec2(0, sel_y + sel_h), screen_size, dark_color);

    // Left rectangle
    draw_list->AddRectFilled(ImVec2(0, sel_y), ImVec2(sel_x, sel_y + sel_h), dark_color);

    // Right rectangle
    draw_list->AddRectFilled(ImVec2(sel_x + sel_w, sel_y), ImVec2(screen_size.x, sel_y + sel_h), dark_color);
}

void ScreenshotInteraction::DrawSelectionBorder()
{
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_w = m_selection.get_width();
    float sel_h = m_selection.get_height();

    draw_list->AddRect(
        ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h), IM_COL32(0, 150, 255, 255), 0.0f, 0, 2.0f);

    float handle_size  = 4.0f;
    ImU32 handle_color = 0xffffffff;

    // Top-left
    draw_list->AddRectFilled(ImVec2(sel_x - handle_size / 2, sel_y - handle_size / 2),
                             ImVec2(sel_x + handle_size / 2, sel_y + handle_size / 2),
                             handle_color);

    // Top-right
    draw_list->AddRectFilled(ImVec2(sel_x + sel_w - handle_size / 2, sel_y - handle_size / 2),
                             ImVec2(sel_x + sel_w + handle_size / 2, sel_y + handle_size / 2),
                             handle_color);

    // Bottom-left
    draw_list->AddRectFilled(ImVec2(sel_x - handle_size / 2, sel_y + sel_h - handle_size / 2),
                             ImVec2(sel_x + handle_size / 2, sel_y + sel_h + handle_size / 2),
                             handle_color);

    // Bottom-right
    draw_list->AddRectFilled(ImVec2(sel_x + sel_w - handle_size / 2, sel_y + sel_h - handle_size / 2),
                             ImVec2(sel_x + sel_w + handle_size / 2, sel_y + sel_h + handle_size / 2),
                             handle_color);
}

void ScreenshotInteraction::DrawSizeIndicator() {}

void ScreenshotInteraction::Cancel(bool on_cancel)
{
    m_state = ToolState::Idle;
    if (m_texture_id)
    {
        GLuint texture = (GLuint)(intptr_t)m_texture_id;
        glDeleteTextures(1, &texture);
        m_texture_id = nullptr;
    }
    if (on_cancel && m_on_cancel)
    {
        m_on_cancel();
    }
}

capture_result_t ScreenshotInteraction::GetFinalImage()
{
    region_t region{
        static_cast<int>(m_selection.get_x()),
        static_cast<int>(m_selection.get_y()),
        static_cast<int>(m_selection.get_width()),
        static_cast<int>(m_selection.get_height()),
    };

    capture_result_t result;
    result.region  = region;
    result.success = true;

    result.data.assign(region.width * region.height * 4, 0);

    // Calculate overlap between selection and screenshot
    int overlap_x1 = std::max(region.x, 0);
    int overlap_y1 = std::max(region.y, 0);
    int overlap_x2 = std::min(region.x + region.width, m_screenshot.region.width);
    int overlap_y2 = std::min(region.y + region.height, m_screenshot.region.height);

    // Copy overlapping region
    for (int y = overlap_y1; y < overlap_y2; ++y)
    {
        for (int x = overlap_x1; x < overlap_x2; ++x)
        {
            int src_x = x;
            int src_y = y;
            int dst_x = x - region.x;
            int dst_y = y - region.y;

            int src_idx = (src_y * m_screenshot.region.width + src_x) * 4;
            int dst_idx = (dst_y * region.width + dst_x) * 4;

            // Copy RGBA pixel
            std::copy_n(&m_screenshot.data[src_idx], 4, &result.data[dst_idx]);
        }
    }

    return result;
}

void ScreenshotInteraction::CreateTexture()
{
    if (m_screenshot.data.empty() || !m_screenshot.success)
        return;

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
}
