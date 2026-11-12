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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "config.hpp"
#include "helpers.hpp"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "util.hpp"
#include "util/color.hpp"
#include "util/image.hpp"

// ------------------------------
// Input
// ------------------------------

static HandleHovered flip_handle_x(HandleHovered handle)
{
    switch (handle)
    {
        case HandleHovered::TopLeft:     return HandleHovered::TopRight;
        case HandleHovered::TopRight:    return HandleHovered::TopLeft;
        case HandleHovered::BottomLeft:  return HandleHovered::BottomRight;
        case HandleHovered::BottomRight: return HandleHovered::BottomLeft;
        case HandleHovered::Left:        return HandleHovered::Right;
        case HandleHovered::Right:       return HandleHovered::Left;
        default:                         return handle;
    }
}

static HandleHovered flip_handle_y(HandleHovered handle)
{
    switch (handle)
    {
        case HandleHovered::TopLeft:     return HandleHovered::BottomLeft;
        case HandleHovered::TopRight:    return HandleHovered::BottomRight;
        case HandleHovered::BottomLeft:  return HandleHovered::TopLeft;
        case HandleHovered::BottomRight: return HandleHovered::TopRight;
        case HandleHovered::Top:         return HandleHovered::Bottom;
        case HandleHovered::Bottom:      return HandleHovered::Top;
        default:                         return handle;
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

bool ScreenshotTool::HandleAnnotationSelectionInput()
{
    annotation_t*& ann           = m_current_annotation_resize;
    const bool     had_selection = (ann != nullptr);

    const ImVec2& mouse_pos    = ImGui::GetMousePos();
    const bool    was_dragging = (m_anno_sel.dragging_handle != HandleHovered::kNone);

    // A click on the toolbar or a window must not (de)select the annotation under it
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ui_blocks_selection())
    {
        bool found = false;
        for (annotation_t& lann : m_annotations)
        {
            ImRect bbox = GetAnnotationBBox(lann);
            bbox.Expand(15.0f);

            if (bbox.Contains(mouse_pos) && m_current_tool == ToolType::kNone)
            {
                m_state = ToolState::AnnResizing;
                ann     = &lann;

                switch (ann->type)
                {
                    case ToolType::Circle:
                    case ToolType::CircleFilled:
                    case ToolType::CounterBubble:
                    case ToolType::Text:
                    {
                        // The padded box that gets drawn, so the handles line up with it
                        const ImRect handle_box = GetAnnotationHandleBox(*ann);
                        m_anno_sel.selection    = { .start = { .x = handle_box.Min.x, .y = handle_box.Min.y },
                                                    .end   = { .x = handle_box.Max.x, .y = handle_box.Max.y } };
                        break;
                    }

                    case ToolType::Pencil:
                        // The points' bounds, since they're scaled below and start/end don't draw a pencil
                        m_anno_sel.selection = { .start = { .x = bbox.Min.x, .y = bbox.Min.y },
                                                 .end   = { .x = bbox.Max.x, .y = bbox.Max.y } };
                        break;

                    default:  // Rectangle, RectangleFilled, Line, Arrow
                        // start/end as they are, since they give Line/Arrow their
                        // direction. Only the drawn border is normalized
                        m_anno_sel.selection = { .start = ann->start, .end = ann->end };
                        break;
                }

                m_anno_sel.preserve_direction = (ann->type == ToolType::Line || ann->type == ToolType::Arrow);

                found = true;
                break;
            }
        }

        // A click outside the selected annotation deselects it, and is swallowed so it doesn't also start or
        // move the main selection
        if (!found)
        {
            m_state       = ToolState::Selected;
            m_input_owner = InputOwner::Tools;
            ann           = nullptr;
            return had_selection;
        }
    }

    // The pencil's points before the drag, which the scaling below starts from
    // every frame. Kept for the whole drag
    if (ann && ann->type == ToolType::Pencil && !was_dragging)
        m_anno_sel.drag_start_points = ann->points;

    HandleSelectionInput(m_anno_sel);

    if (ann)
    {
        HandleResizeInput(m_anno_sel);

        // Read again, since HandleSelectionInput() may have changed ann
        const ToolType type  = ann->type;
        const float    sel_x = m_anno_sel.selection.get_x();
        const float    sel_y = m_anno_sel.selection.get_y();
        const float    sel_w = m_anno_sel.selection.get_width();
        const float    sel_h = m_anno_sel.selection.get_height();

        switch (type)
        {
            case ToolType::Circle:
            case ToolType::CircleFilled:
            case ToolType::CounterBubble:
            {
                // The selection is the padded GetAnnotationHandleBox(), so remove the padding before converting
                // it to center + radius
                constexpr float PIXEL   = 1.0f;
                const float     padding = ann->thickness - PIXEL;
                const float     box_x   = sel_x + padding;
                const float     box_y   = sel_y + padding;
                const float     box_w   = sel_w - padding * 2.0f;
                const float     box_h   = sel_h - padding * 2.0f;
                const float     radius  = std::min(box_w, box_h) * 0.5f;

                ann->start = { .x = box_x + box_w * 0.5f, .y = box_y + box_h * 0.5f };
                ann->end   = { .x = ann->start.x + radius, .y = ann->start.y };
                break;
            }

            case ToolType::Text:
            {
                // Same padding as above. The box comes from the font size,
                // so dragging a handle only moves the text
                constexpr float PIXEL   = 1.0f;
                const float     padding = ann->thickness - PIXEL;
                ann->start              = { .x = sel_x + padding, .y = sel_y + padding };
                break;
            }

            case ToolType::Pencil:
            {
                // Scale the points from the drag-start snapshot into the new box.
                // Only while dragging, drag_start_selection is stale otherwise
                if (was_dragging && !m_anno_sel.drag_start_points.empty())
                {
                    const selection_rect_t& old_box = m_anno_sel.drag_start_selection;
                    const float             old_w   = old_box.get_width();
                    const float             old_h   = old_box.get_height();
                    const float             scale_x = old_w > 0.0f ? sel_w / old_w : 1.0f;
                    const float             scale_y = old_h > 0.0f ? sel_h / old_h : 1.0f;

                    ann->points.resize(m_anno_sel.drag_start_points.size());
                    for (size_t i = 0; i < m_anno_sel.drag_start_points.size(); ++i)
                    {
                        const point_t& p = m_anno_sel.drag_start_points[i];
                        ann->points[i].x = sel_x + (p.x - old_box.get_x()) * scale_x;
                        ann->points[i].y = sel_y + (p.y - old_box.get_y()) * scale_y;
                    }
                    if (!ann->points.empty())
                    {
                        ann->start = ann->points.front();
                        ann->end   = ann->points.back();
                    }
                }
                break;
            }

            default:  // Rectangle, RectangleFilled, Line, Arrow
                ann->start = m_anno_sel.selection.start;
                ann->end   = m_anno_sel.selection.end;
                break;
        }
    }

    return false;
}

void ScreenshotTool::HandleShortcutsInput()
{
    if (ImGui::Shortcut(ImGuiKey_E | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
    {
        g_config->File.allow_out_edit = !g_config->File.allow_out_edit;
        ImGui::ClearActiveID();
    }

    if (ImGui::Shortcut(ImGuiKey_A | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
    {
        m_main_sel.selection.start = point_t{ .x = m_image_origin.x, .y = m_image_origin.y };
        m_main_sel.selection.end   = point_t{ .x = m_image_end.x, .y = m_image_end.y };
        m_state                    = ToolState::Selected;
        UpdateAnnsToolbarPos();
    }

    if (ImGui::Shortcut(ImGuiKey_Z | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal) && !m_annotations.empty())
        m_annotations.pop_back();

    if (ImGui::Shortcut(ImGuiKey_G | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
        g_config->Runtime.enable_handles = !g_config->Runtime.enable_handles;

    if (ImGui::Shortcut(ImGuiKey_S | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
        RequestComplete(SavingOp::File);

    if (ImGui::Shortcut(ImGuiKey_C | ImGuiMod_Ctrl | (g_config->File.ctrl_c_copy_img ? 0 : ImGuiMod_Shift),
                        ImGuiInputFlags_RouteGlobal) &&
        !ui_blocks_selection())
        RequestComplete(SavingOp::Clipboard);
}

void ScreenshotTool::HandleSelectionInput(selection_info_t& sel)
{
    // Only block new interactions. Never block an ongoing drag/resize.
    if ((m_input_owner != sel.main_input_owner && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
         ui_blocks_selection()) ||
        m_current_tool != ToolType::kNone)
    {
        m_input_owner = InputOwner::Tools;
        return;
    }

    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const float   sel_x     = sel.selection.get_x();
    const float   sel_y     = sel.selection.get_y();
    const float   sel_w     = sel.selection.get_width();
    const float   sel_h     = sel.selection.get_height();
    const ImRect  selection_rect(ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h));

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_input_owner != sel.main_input_owner)
    {
        m_input_owner = sel.main_input_owner;

        // Resizing from a handle
        if (sel.handle_hover != HandleHovered::kNone)
        {
            // HandleResizeInput() expects start <= end, except for Line/Arrow,
            // where the order is the direction
            if (!sel.preserve_direction)
                NormalizeSelection(sel);
            sel.dragging_handle      = sel.handle_hover;
            sel.drag_start_mouse     = mouse_pos;
            sel.drag_start_selection = sel.selection;
            m_state                  = sel.resizing_state;
        }
        // Moving the selection
        else if (selection_rect.Contains(mouse_pos))
        {
            if (!sel.preserve_direction)
                NormalizeSelection(sel);
            sel.dragging_handle      = HandleHovered::Move;
            sel.drag_start_mouse     = mouse_pos;
            sel.drag_start_selection = sel.selection;
            m_state                  = sel.resizing_state;
        }
        // A new selection, unless it's an annotation's
        else if (!sel.is_ann)
        {
            sel.selection.start = { .x = mouse_pos.x, .y = mouse_pos.y };
            sel.selection.end   = sel.selection.start;
            m_state             = ToolState::Selecting;
        }
    }

    if (m_input_owner == sel.main_input_owner && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (m_state == sel.resizing_state)
            HandleResizeInput(sel);
        else  // ToolState::Selecting
            sel.selection.end = { .x = mouse_pos.x, .y = mouse_pos.y };

        if (!sel.is_ann)
            ClampSelectionToImage(sel);
    }

    if (m_input_owner == sel.main_input_owner && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        sel.dragging_handle = HandleHovered::kNone;
        m_input_owner       = InputOwner::kNone;

        // Only a click or a slip of the mouse counts as too small, since a line of text can be 10 px high
        constexpr float min_size = 3.0f;
        if (sel.is_ann || (sel.selection.get_width() >= min_size && sel.selection.get_height() >= min_size))
            m_state = sel.idle_state;

        if (!sel.preserve_direction)
            NormalizeSelection(sel);

        if (!sel.is_ann)
            UpdateAnnsToolbarPos();
    }
}

void ScreenshotTool::HandleResizeInput(selection_info_t& sel)
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const ImVec2  delta(mouse_pos.x - sel.drag_start_mouse.x, mouse_pos.y - sel.drag_start_mouse.y);

    switch (sel.dragging_handle)
    {
        case HandleHovered::TopLeft:
            sel.selection.start.x = sel.drag_start_selection.start.x + delta.x;
            sel.selection.start.y = sel.drag_start_selection.start.y + delta.y;
            break;
        case HandleHovered::TopRight:
            sel.selection.end.x   = sel.drag_start_selection.end.x + delta.x;
            sel.selection.start.y = sel.drag_start_selection.start.y + delta.y;
            break;
        case HandleHovered::BottomLeft:
            sel.selection.start.x = sel.drag_start_selection.start.x + delta.x;
            sel.selection.end.y   = sel.drag_start_selection.end.y + delta.y;
            break;
        case HandleHovered::BottomRight:
            sel.selection.end.x = sel.drag_start_selection.end.x + delta.x;
            sel.selection.end.y = sel.drag_start_selection.end.y + delta.y;
            break;
        case HandleHovered::Top:    sel.selection.start.y = sel.drag_start_selection.start.y + delta.y; break;
        case HandleHovered::Bottom: sel.selection.end.y = sel.drag_start_selection.end.y + delta.y; break;
        case HandleHovered::Left:   sel.selection.start.x = sel.drag_start_selection.start.x + delta.x; break;
        case HandleHovered::Right:  sel.selection.end.x = sel.drag_start_selection.end.x + delta.x; break;
        case HandleHovered::Move:
            sel.selection.start.x = sel.drag_start_selection.start.x + delta.x;
            sel.selection.start.y = sel.drag_start_selection.start.y + delta.y;
            sel.selection.end.x   = sel.drag_start_selection.end.x + delta.x;
            sel.selection.end.y   = sel.drag_start_selection.end.y + delta.y;
            break;
        default: break;
    }

    // Don't flip with annotations
    if (sel.is_ann)
        return;

    // A handle dragged past the opposite edge inverts the selection, so swap the coordinates and the handle,
    // and reset the drag anchor
    if (sel.selection.start.x > sel.selection.end.x)
    {
        std::swap(sel.selection.start.x, sel.selection.end.x);
        sel.dragging_handle      = flip_handle_x(sel.dragging_handle);
        sel.drag_start_mouse     = mouse_pos;
        sel.drag_start_selection = sel.selection;
    }

    if (sel.selection.start.y > sel.selection.end.y)
    {
        std::swap(sel.selection.start.y, sel.selection.end.y);
        sel.dragging_handle      = flip_handle_y(sel.dragging_handle);
        sel.drag_start_mouse     = mouse_pos;
        sel.drag_start_selection = sel.selection;
    }
}

void ScreenshotTool::HandleAnnotationInput()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();

    // Cancel a text being placed when another tool is picked, or its input reappears at (0, 0) the next time
    // Text is selected
    if (m_current_actions.Has(CurrentAction::IsTextPlacing) && m_current_tool != ToolType::Text)
    {
        m_current_annotation = {};
        m_current_actions.Clear(CurrentAction::IsTextPlacing);
    }

    if (m_current_tool == ToolType::Text)
    {
        if (!m_current_actions.Has(CurrentAction::IsTextPlacing) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ui_blocks_selection())
        {
            m_current_actions.Set(CurrentAction::IsTextPlacing);
            m_current_annotation.type      = ToolType::Text;
            m_current_annotation.start     = { .x = mouse_pos.x, .y = mouse_pos.y };
            m_current_annotation.end       = m_current_annotation.start;
            m_current_annotation.color     = m_current_color;
            m_current_annotation.thickness = m_tool_thickness[idx(ToolType::Text)];
            m_current_annotation.text.clear();
        }

        if (m_current_actions.Has(CurrentAction::IsTextPlacing))
        {
            const float padding_y =
                std::max(2.0f, (m_current_annotation.thickness - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::SetNextWindowPos(ImVec2(m_current_annotation.start.x, m_current_annotation.start.y));
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, padding_y));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, 0x00000000_rgba);

            ImGui::Begin("##text_ann_input_win",
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar);

            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            ImFont* ann_font = CacheAndGetFont(m_inputs.resolved_ann_font_path, m_current_annotation.thickness);
            ImGui::PushFont(ann_font, ann_font->LegacySize);

            ImGui::PushStyleColor(ImGuiCol_Text, m_current_annotation.color.to_abgr());
            if (ImGui::InputText(
                    "##text_ann_input_text", &m_current_annotation.text, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (!m_current_annotation.text.empty())
                {
                    // Move start to where the text is drawn, past the FramePadding pushed above
                    const ImVec2& fp = ImGui::GetStyle().FramePadding;
                    m_current_annotation.start.x += fp.x;
                    m_current_annotation.start.y += fp.y;

                    m_annotations.push_back(m_current_annotation);
                }
                m_current_annotation = {};
                m_current_actions.Clear(CurrentAction::IsTextPlacing);
            }
            ImGui::PopFont();

            ImGui::TextUnformatted(_("Enter: Place | ESC: Cancel"));

            ImGui::PopStyleColor();

            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_current_annotation = {};
                m_current_actions.Clear(CurrentAction::IsTextPlacing);
            }

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
        }
        return;  // never fall through to the generic drag path
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ui_blocks_selection())
    {
        m_current_actions.Set(CurrentAction::IsDrawing);
        m_current_annotation.type      = m_current_tool;
        m_current_annotation.start     = { .x = mouse_pos.x, .y = mouse_pos.y };
        m_current_annotation.end       = m_current_annotation.start;
        m_current_annotation.color     = m_current_color;
        m_current_annotation.thickness = m_tool_thickness[idx(m_current_tool)];
        m_current_annotation.points.clear();

        if (m_current_tool == ToolType::Pencil)
        {
            m_current_annotation.points.push_back(m_current_annotation.start);
        }
        else if (m_current_tool == ToolType::CounterBubble)
        {
            uint8_t next = 1;
            for (const auto& ann : m_annotations)
                if (ann.type == ToolType::CounterBubble && ann.count >= next)
                    next = ann.count + 1;
            m_current_annotation.count = next;
        }
    }

    if (m_current_actions.Has(CurrentAction::IsDrawing) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        m_current_annotation.end = { .x = mouse_pos.x, .y = mouse_pos.y };

        if (m_current_tool == ToolType::Pencil)
        {
            // Add a point only if it's more than 2px from the last one
            if (!m_current_annotation.points.empty())
            {
                const point_t& last = m_current_annotation.points.back();
                float          dx   = mouse_pos.x - last.x;
                float          dy   = mouse_pos.y - last.y;
                if (dx * dx + dy * dy > 4.0f)
                    m_current_annotation.points.push_back({ .x = mouse_pos.x, .y = mouse_pos.y });
            }
        }
    }

    if (m_current_actions.Has(CurrentAction::IsDrawing) && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_current_actions.Clear(CurrentAction::IsDrawing);

        float dx         = m_current_annotation.end.x - m_current_annotation.start.x;
        float dy         = m_current_annotation.end.y - m_current_annotation.start.y;
        bool  should_add = (m_current_tool == ToolType::Pencil || (dx * dx + dy * dy) > 25.0f);  // at least 5px long

        // A drag sizes the bubble, a click places one of the default size
        if (m_current_tool == ToolType::CounterBubble && !should_add)
        {
            m_current_annotation.end = { .x = m_current_annotation.start.x + 20.0f, .y = m_current_annotation.start.y };
            should_add               = true;
        }
        if (should_add)
            m_annotations.push_back(m_current_annotation);

        m_current_annotation = annotation_t{};
    }
}

void ScreenshotTool::HandleColorPickerInput()
{
    // The loupe acts as our cursor
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const int     px        = int((mouse_pos.x - m_image_origin.x) * m_image_scale);
    const int     py        = int((mouse_pos.y - m_image_origin.y) * m_image_scale);

    const bool in_image = px >= 0 && px < m_screenshot.w && py >= 0 && py < m_screenshot.h;

    constexpr float k_loupe_px = 140.0f;  // loupe display size (square, pixels)
    constexpr float k_zoom     = 8.0f;    // magnification factor
    constexpr float k_padding  = 10.0f;   // inner window padding
    constexpr float k_offset   = 15.0f;   // distance from cursor to loupe corner
    constexpr float k_win_size = k_loupe_px + k_padding * 2.0f;

    constexpr uint32_t shadow_color      = (0x000000B4_rgba).to_abgr();
    constexpr uint32_t white_lines_color = (0xffffffE6_rgba).to_abgr();

    // Bottom-right of the cursor, flipped to stay on screen
    const ImVec2& display = ImGui::GetIO().DisplaySize;
    float         win_x   = mouse_pos.x + k_offset;
    float         win_y   = mouse_pos.y + k_offset;

    // The width is known for the horizontal flip. AlwaysAutoResize sets the height,
    // so the vertical flip uses the loupe's height plus a margin
    constexpr float k_approx_info_h = 50.0f;  // swatch row + spacing
    if (win_x + k_win_size > display.x)
        win_x = mouse_pos.x - k_offset - k_win_size;
    if (win_y + k_win_size + k_approx_info_h > display.y)
        win_y = mouse_pos.y - k_offset - k_win_size - k_approx_info_h;

    ImGui::SetNextWindowPos(ImVec2(win_x, win_y));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(k_padding, k_padding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::Begin("##color_loupe",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    if (!in_image)
    {
        ImGui::TextDisabled("%s", _("Outside of the image"));
    }
    else
    {
        // Sample the pixel under the cursor
        const size_t off = (size_t(py) * m_screenshot.w + px) * 4;
        const rgba_t c   = load_rgba(m_screenshot.data.data() + off);

        // UVs of the zoomed region
        const float half_src_px_x = (k_loupe_px / k_zoom) * 0.5f / float(m_screenshot.w);
        const float half_src_px_y = (k_loupe_px / k_zoom) * 0.5f / float(m_screenshot.h);
        const float uv_cx         = (float(px) + 0.5f) / float(m_screenshot.w);
        const float uv_cy         = (float(py) + 0.5f) / float(m_screenshot.h);

        const ImVec2 uv_min(uv_cx - half_src_px_x, uv_cy - half_src_px_y);
        const ImVec2 uv_max(uv_cx + half_src_px_x, uv_cy + half_src_px_y);

        ImGuiPlatformIO& platform_io  = ImGui::GetPlatformIO();
        ImDrawList*      dl           = ImGui::GetWindowDrawList();
        const ImVec2&    loupe_origin = ImGui::GetCursorScreenPos();

        if (platform_io.DrawCallback_SetSamplerNearest)
            dl->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);

        // Draw the magnified image
        ImGui::Image(m_texture_id, ImVec2(k_loupe_px, k_loupe_px), uv_min, uv_max);

        if (platform_io.DrawCallback_SetSamplerLinear)
            dl->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);

        // Crosshair
        const ImVec2    ctr = ImVec2(loupe_origin.x + k_loupe_px * 0.5f, loupe_origin.y + k_loupe_px * 0.5f);
        constexpr float arm = 10.0f;
        constexpr float gap = 3.0f;  // gap around the center dot

        // Shadow lines for contrast on any background
        dl->AddLine(ImVec2(ctr.x - arm, ctr.y), ImVec2(ctr.x - gap, ctr.y), shadow_color, 1.5f);
        dl->AddLine(ImVec2(ctr.x + gap, ctr.y), ImVec2(ctr.x + arm, ctr.y), shadow_color, 1.5f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - arm), ImVec2(ctr.x, ctr.y - gap), shadow_color, 1.5f);
        dl->AddLine(ImVec2(ctr.x, ctr.y + gap), ImVec2(ctr.x, ctr.y + arm), shadow_color, 1.5f);
        // White lines on top
        dl->AddLine(ImVec2(ctr.x - arm, ctr.y), ImVec2(ctr.x - gap, ctr.y), white_lines_color, 1.0f);
        dl->AddLine(ImVec2(ctr.x + gap, ctr.y), ImVec2(ctr.x + arm, ctr.y), white_lines_color, 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - arm), ImVec2(ctr.x, ctr.y - gap), white_lines_color, 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y + gap), ImVec2(ctr.x, ctr.y + arm), white_lines_color, 1.0f);
        // Center dot, filled with the hovered color so it's always visible
        dl->AddCircleFilled(ctr, gap - 0.5f, c.to_abgr());
        dl->AddCircle(ctr, gap - 0.5f, (0xffffffC8_rgba).to_abgr(), 12, 1.0f);

        // Outline around the entire loupe image
        dl->AddRect(loupe_origin,
                    ImVec2(loupe_origin.x + k_loupe_px, loupe_origin.y + k_loupe_px),
                    (0x505050DC_rgba).to_abgr(),
                    2.0f,
                    1.5f,
                    ImDrawFlags_None);

        ImGui::Spacing();
        ImGui::ColorButton("##loupe_swatch", c, ImGuiColorEditFlags_NoPicker, ImVec2(32, 32));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("#%02X%02X%02X", c.r, c.g, c.b);
        ImGui::TextColored(Colors::RED, "%-3d ", c.r);
        ImGui::SameLine();
        ImGui::TextColored(Colors::GREEN, "%-3d ", c.g);
        ImGui::SameLine();
        ImGui::TextColored(Colors::BLUE, "%-3d", c.b);
        ImGui::EndGroup();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_current_color = c;
            m_current_actions.Clear(CurrentAction::IsColorPicking);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();

    // A precise mark under the cursor, which stays visible outside the loupe
    {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddCircle(mouse_pos, 5.0f, shadow_color, 12, 2.0f);
        fg->AddCircleFilled(mouse_pos, 2.0f, Colors::WHITE.to_abgr());
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_current_actions.Clear(CurrentAction::IsColorPicking);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
}
