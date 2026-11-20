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
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "cache.hpp"
#include "config.hpp"
#include "helpers.hpp"
#include "i18n.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "theme.hpp"
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
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return false;

    // A modal leaves nothing hovered behind it, so a click there would reach the overlay
    if (ImGui::GetTopMostPopupModal())
        return true;

    // Looked up every time, since every capture makes a new ImGui context
    const ImGuiWindow* overlay_window = ImGui::FindWindowByName("Screenshot Tool");
    const ImGuiWindow* hovered        = ctx->HoveredWindow;
    if (!hovered || !overlay_window)
        return false;

    // Allow selection when hovering the overlay window itself
    if (hovered->RootWindow == overlay_window)
        return false;

    // Anything else (Text tools, menu popups, etc.) blocks starting selection
    return true;
}

static float dist_to_segment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    const ImVec2 c = ImLineClosestPoint(a, b, p);
    return std::sqrt(ImLengthSqr(p - c));
}

std::vector<annotation_t*> ScreenshotTool::GetAnnotationsAt(const ImVec2& pos)
{
    std::vector<annotation_t*> under;

    // How far off the drawn shape a click still hits it
    constexpr float slack = 4.0f;
    const float     dpi   = ImGui::GetIO().DisplayFramebufferScale.x;

    // Last drawn is on top
    for (annotation_t& ann : std::views::reverse(m_annotations))
    {
        const float  t       = ann.thickness * dpi;
        const float  reach   = t * 0.5f + slack;
        const ImVec2 p1      = ann.start;
        const ImVec2 p2      = ann.end;
        const float  dist_p1 = std::sqrt(ImLengthSqr(pos - p1));
        const float  radius  = std::sqrt(ImLengthSqr(p2 - p1));

        bool hit = false;
        switch (ann.type)
        {
            case ToolType::Line: hit = dist_to_segment(pos, p1, p2) <= reach; break;

            case ToolType::Arrow:
                // The head is 6t long and 4t wide at its base
                hit = dist_to_segment(pos, p1, p2) <= reach || ImLengthSqr(pos - p2) <= ImPow(t * 3.0f + slack, 2.0f);
                break;

            case ToolType::Rectangle:
            {
                // Only the outline, so a click inside still reaches what's under it
                ImRect outer = GetAnnotationBBox(ann);
                ImRect inner = outer;
                outer.Expand(reach);
                inner.Expand(-reach);
                hit = outer.Contains(pos) && !inner.Contains(pos);
                break;
            }

            case ToolType::Circle: hit = std::abs(dist_p1 - radius) <= reach; break;

            case ToolType::CircleFilled:
            case ToolType::CounterBubble: hit = dist_p1 <= radius + reach; break;

            case ToolType::Pencil:
                if (ann.points.size() == 1)
                    hit = std::sqrt(ImLengthSqr(pos - ann.points[0])) <= reach;
                for (size_t i = 1; i < ann.points.size() && !hit; ++i)
                    hit = dist_to_segment(pos, ann.points[i - 1], ann.points[i]) <= reach;
                break;

            default:  // RectangleFilled, Text
            {
                ImRect bbox = GetAnnotationBBox(ann);
                bbox.Expand(slack);
                hit = bbox.Contains(pos);
                break;
            }
        }

        if (hit)
            under.push_back(&ann);
    }

    // Then the outlines it's inside, smallest first, so a shape drawn inside a bigger one comes before it
    std::vector<annotation_t*> inside;
    for (annotation_t& ann : m_annotations)
    {
        if ((ann.type != ToolType::Rectangle && ann.type != ToolType::Circle) ||
            std::ranges::find(under, &ann) != under.end())
            continue;

        const ImRect bbox   = GetAnnotationBBox(ann);
        const float  radius = bbox.GetWidth() * 0.5f;
        const ImVec2 center = bbox.GetCenter();
        const bool   contains =
            ann.type == ToolType::Rectangle ? bbox.Contains(pos) : ImLengthSqr(pos - center) <= radius * radius;
        if (contains)
            inside.push_back(&ann);
    }
    std::ranges::stable_sort(inside, {}, [&](const annotation_t* a) { return GetAnnotationBBox(*a).GetArea(); });

    under.insert(under.end(), inside.begin(), inside.end());
    return under;
}

void ScreenshotTool::DeselectAnnotation()
{
    m_current_annotation_resize = nullptr;
    m_anno_sel.handle_hover     = HandleHovered::kNone;
    m_anno_sel.dragging_handle  = HandleHovered::kNone;
    if (m_input_owner == InputOwner::AnnSelection)
        m_input_owner = InputOwner::kNone;
}

void ScreenshotTool::ApplyAnnotationDrag()
{
    annotation_t&       ann  = *m_current_annotation_resize;
    const annotation_t& orig = m_ann_drag_start;
    selection_info_t&   sel  = m_anno_sel;

    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const ImVec2  delta     = mouse_pos - sel.drag_start_mouse;
    const auto    offset    = [&](const ImVec2& p) { return p + delta; };

    switch (sel.dragging_handle)
    {
        case HandleHovered::Move:
            ann.start = offset(orig.start);
            ann.end   = offset(orig.end);
            for (size_t i = 0; i < orig.points.size(); ++i)
                ann.points[i] = offset(orig.points[i]);
            return;

        case HandleHovered::LineStart: ann.start = offset(orig.start); return;
        case HandleHovered::LineEnd:   ann.end = offset(orig.end); return;
        default:                       break;
    }

    // The bounds with the dragged edges moved. An edge dragged past the opposite one mirrors the annotation
    sel.selection = sel.drag_start_selection;
    HandleResizeInput(sel);
    const selection_rect_t& from   = sel.drag_start_selection;
    const selection_rect_t& to     = sel.selection;
    const float             from_w = from.end.x - from.start.x;
    const float             from_h = from.end.y - from.start.y;
    const float             to_w   = to.end.x - to.start.x;
    const float             to_h   = to.end.y - to.start.y;

    const HandleHovered h = sel.dragging_handle;
    const bool moves_left = h == HandleHovered::TopLeft || h == HandleHovered::BottomLeft || h == HandleHovered::Left;
    const bool moves_top  = h == HandleHovered::TopLeft || h == HandleHovered::TopRight || h == HandleHovered::Top;
    const bool is_corner  = h == HandleHovered::TopLeft || h == HandleHovered::TopRight ||
                            h == HandleHovered::BottomLeft || h == HandleHovered::BottomRight;
    const bool moves_x    = is_corner || h == HandleHovered::Left || h == HandleHovered::Right;
    const bool moves_y    = is_corner || h == HandleHovered::Top || h == HandleHovered::Bottom;

    // A corner drag scales by how far the mouse went along the diagonal from the opposite corner, so moving
    // across the diagonal doesn't change the size
    const float diag_x  = moves_left ? -from_w : from_w;
    const float diag_y  = moves_top ? -from_h : from_h;
    const float diag_sq = diag_x * diag_x + diag_y * diag_y;
    const float diag_scale =
        diag_sq > 0.0f ? ((diag_x + delta.x) * diag_x + (diag_y + delta.y) * diag_y) / diag_sq : 1.0f;
    const ImVec2 anchor(moves_left ? from.end.x : from.start.x, moves_top ? from.end.y : from.start.y);

    switch (ann.type)
    {
        case ToolType::Circle:
        case ToolType::CircleFilled:
        case ToolType::CounterBubble:
        {
            // Stays round, against the edges that aren't dragged
            float cx   = 0.0f;
            float cy   = 0.0f;
            float side = 0.0f;
            if (is_corner && from_w > 0.0f)
            {
                side          = std::max(std::abs(diag_scale) * from_w, 4.0f);
                const float k = std::copysign(side / from_w, diag_scale);
                cx            = anchor.x + diag_x * k * 0.5f;
                cy            = anchor.y + diag_y * k * 0.5f;
            }
            else
            {
                side = std::max(std::abs(moves_x ? to_w : to_h), 4.0f);
                cx   = !moves_x     ? from.start.x + from_w * 0.5f
                       : moves_left ? to.end.x - std::copysign(side, to_w) * 0.5f
                                    : to.start.x + std::copysign(side, to_w) * 0.5f;
                cy   = !moves_y    ? from.start.y + from_h * 0.5f
                       : moves_top ? to.end.y - std::copysign(side, to_h) * 0.5f
                                   : to.start.y + std::copysign(side, to_h) * 0.5f;
            }

            ann.start = ImVec2(cx, cy);
            ann.end   = ImVec2(cx + side * 0.5f, cy);
            break;
        }

        case ToolType::Text:
        {
            // The corners scale the font, against the opposite corner
            if (from_w <= 0.0f || from_h <= 0.0f)
                break;

            const float size = std::clamp(std::round(orig.thickness * diag_scale), 8.0f, 144.0f);
            const float k    = size / orig.thickness;

            ann.thickness = size;
            ann.start =
                ImVec2(moves_left ? anchor.x - from_w * k : anchor.x, moves_top ? anchor.y - from_h * k : anchor.y);
            break;
        }

        default:  // Rectangle, RectangleFilled, Pencil
        {
            const auto map = [&](const ImVec2& p) {
                return ImVec2(
                    from_w > 0.0f ? to.start.x + (p.x - from.start.x) * to_w / from_w : p.x + to.start.x - from.start.x,
                    from_h > 0.0f ? to.start.y + (p.y - from.start.y) * to_h / from_h
                                  : p.y + to.start.y - from.start.y);
            };
            ann.start = map(orig.start);
            ann.end   = map(orig.end);
            for (size_t i = 0; i < orig.points.size(); ++i)
                ann.points[i] = map(orig.points[i]);
            break;
        }
    }
}

bool ScreenshotTool::HandleAnnotationSelectionInput()
{
    annotation_t*& ann       = m_current_annotation_resize;
    const ImVec2&  mouse_pos = ImGui::GetMousePos();
    const bool     dragging  = m_anno_sel.dragging_handle != HandleHovered::kNone;

    const auto sync_box = [&] { m_anno_sel.selection = selection_rect_t::from_rect(GetAnnotationHandleBox(*ann)); };

    // A tool picked from the toolbar takes the clicks
    if (ann && m_current_tool != ToolType::kNone)
        DeselectAnnotation();

    if (ann && !dragging)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            DeselectAnnotation();
        }
        else if (ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal) ||
                 ImGui::Shortcut(ImGuiKey_Backspace, ImGuiInputFlags_RouteGlobal))
        {
            m_annotations.erase(m_annotations.begin() + (ann - m_annotations.data()));
            DeselectAnnotation();
        }
        else
        {
            sync_box();
            UpdateHandleHoverState(m_anno_sel);
        }
    }

    // A handle of the main selection wins over the annotations under it
    const bool can_pick = m_current_tool == ToolType::kNone && !dragging && m_input_owner != InputOwner::Selection &&
                          !ui_blocks_selection();
    const std::vector<annotation_t*> under = can_pick && (ann || m_main_sel.handle_hover == HandleHovered::kNone)
                                                 ? GetAnnotationsAt(mouse_pos)
                                                 : std::vector<annotation_t*>{};

    // A double-click focuses the topmost one, or the one under the focused one, so each of a stack can be reached.
    // A focused text stays, and is edited instead
    annotation_t* next = nullptr;
    if (!under.empty())
    {
        const auto it = std::ranges::find(under, ann);
        if (it != under.end() && ann->type == ToolType::Text)
            next = ann;
        else
            next = it == under.end() || std::next(it) == under.end() ? under.front() : *std::next(it);
    }

    // Inside the main selection a click moves it, so an annotation there takes a double-click
    const bool in_main = m_main_sel.selection.rect().Contains(mouse_pos);

    m_hovered_annotation.reset();
    m_hover_hint = nullptr;
    if (next && next != ann)
    {
        m_hovered_annotation = size_t(next - m_annotations.data());
        if (in_main)
            m_hover_hint = _("Double-click to select");
    }
    else if (next && next->type == ToolType::Text)
    {
        m_hovered_annotation = size_t(next - m_annotations.data());
        m_hover_hint         = _("Double-click to edit");
    }

    const bool double_click = can_pick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (double_click && ann && next == ann && ann->type == ToolType::Text)
    {
        EditTextAnnotation(size_t(ann - m_annotations.data()));
        return true;
    }

    if (can_pick && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        // The focused one's handles and box come first, then outside the main selection one click focuses another
        HandleHovered handle = HandleHovered::kNone;
        annotation_t* focus  = nullptr;
        const bool    on_ann = ann && m_anno_sel.handle_hover != HandleHovered::kNone;

        // A stroke's box is mostly empty, so only the stroke grabs it
        const bool is_stroke =
            ann && (ann->type == ToolType::Line || ann->type == ToolType::Arrow || ann->type == ToolType::Pencil);
        const bool in_ann = ann && (is_stroke ? std::ranges::find(under, ann) != under.end()
                                              : m_anno_sel.selection.rect().Contains(mouse_pos));
        if (double_click && next && next != ann)
            focus = next;
        else if (on_ann)
            handle = m_anno_sel.handle_hover;
        else if (in_ann)
            handle = HandleHovered::Move;
        else if (!in_main && !under.empty())
            focus = under.front();
        else if (ann)
        {
            // Swallowed, so it doesn't also start or move the main selection
            DeselectAnnotation();
            return true;
        }

        // Focused and grabbed with the same press
        if (focus)
        {
            DeselectAnnotation();
            ann                     = focus;
            handle                  = HandleHovered::Move;
            m_main_sel.handle_hover = HandleHovered::kNone;
        }

        if (handle != HandleHovered::kNone)
        {
            m_ann_drag_start = *ann;
            m_input_owner    = InputOwner::AnnSelection;

            m_anno_sel.dragging_handle      = handle;
            m_anno_sel.drag_start_mouse     = mouse_pos;
            m_anno_sel.drag_start_selection = selection_rect_t::from_rect(GetAnnotationBBox(*ann));
        }
    }

    if (ann && m_anno_sel.dragging_handle != HandleHovered::kNone)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            ApplyAnnotationDrag();
        }
        else
        {
            m_anno_sel.dragging_handle = HandleHovered::kNone;
            m_input_owner              = InputOwner::kNone;
        }
    }

    if (ann)
        sync_box();

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
        m_main_sel.selection.start = m_image_origin;
        m_main_sel.selection.end   = m_image_end;
        m_state                    = ToolState::Selected;
        UpdateAnnsToolbarPos();
    }

    // Not while a text is typed, whose edit holds an index into m_annotations
    if (ImGui::Shortcut(ImGuiKey_Z | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal) && !m_annotations.empty() &&
        !m_current_actions.Has(CurrentAction::IsTextPlacing))
    {
        DeselectAnnotation();
        m_annotations.pop_back();
    }

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

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_input_owner != sel.main_input_owner)
    {
        m_input_owner = sel.main_input_owner;

        // Resizing from a handle
        if (sel.handle_hover != HandleHovered::kNone)
        {
            // HandleResizeInput() expects start <= end
            sel.selection.normalize();
            sel.dragging_handle      = sel.handle_hover;
            sel.drag_start_mouse     = mouse_pos;
            sel.drag_start_selection = sel.selection;
            m_state                  = sel.resizing_state;
        }
        // Moving the selection
        else if (sel.selection.rect().Contains(mouse_pos))
        {
            sel.selection.normalize();
            sel.dragging_handle      = HandleHovered::Move;
            sel.drag_start_mouse     = mouse_pos;
            sel.drag_start_selection = sel.selection;
            m_state                  = sel.resizing_state;
        }
        // A new selection
        else
        {
            sel.selection.start = mouse_pos;
            sel.selection.end   = sel.selection.start;
            m_state             = ToolState::Selecting;
        }
    }

    if (m_input_owner == sel.main_input_owner && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (m_state == sel.resizing_state)
            HandleResizeInput(sel);
        else  // ToolState::Selecting
            sel.selection.end = mouse_pos;

        ClampSelectionToImage(sel);
    }

    if (m_input_owner == sel.main_input_owner && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        sel.dragging_handle = HandleHovered::kNone;
        m_input_owner       = InputOwner::kNone;

        // Only a click or a slip of the mouse counts as too small, since a line of text can be 10 px high
        constexpr float min_size = 3.0f;
        if (sel.selection.get_width() >= min_size && sel.selection.get_height() >= min_size)
            m_state = sel.idle_state;

        sel.selection.normalize();
        UpdateAnnsToolbarPos();
    }
}

void ScreenshotTool::HandleResizeInput(selection_info_t& sel)
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const ImVec2  delta     = mouse_pos - sel.drag_start_mouse;

    // The edges the dragged handle moves, all four for Move
    using enum HandleHovered;
    const HandleHovered h     = sel.dragging_handle;
    const auto          moves = [h](const HandleHovered a, const HandleHovered b, const HandleHovered c) {
        return h == a || h == b || h == c || h == Move;
    };
    const selection_rect_t& from = sel.drag_start_selection;
    if (moves(Left, TopLeft, BottomLeft))
        sel.selection.start.x = from.start.x + delta.x;
    if (moves(Right, TopRight, BottomRight))
        sel.selection.end.x = from.end.x + delta.x;
    if (moves(Top, TopLeft, TopRight))
        sel.selection.start.y = from.start.y + delta.y;
    if (moves(Bottom, BottomLeft, BottomRight))
        sel.selection.end.y = from.end.y + delta.y;

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

    // Picking another tool keeps the text being typed
    if (m_current_actions.Has(CurrentAction::IsTextPlacing) && m_current_tool != ToolType::Text)
        CommitTextAnnotation();

    // Without a tool, clicks select annotations or move the selection
    if (m_current_tool == ToolType::kNone)
        return;

    if (m_current_tool == ToolType::Text)
    {
        // A click outside the input ends the text. On a text it edits that one, anywhere else it starts a new one
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ui_blocks_selection())
        {
            CommitTextAnnotation();

            std::optional<size_t> hit;
            for (size_t i = m_annotations.size(); i-- > 0;)
            {
                ImRect bbox = GetAnnotationBBox(m_annotations[i]);
                bbox.Expand(TEXT_HANDLE_PADDING);
                if (m_annotations[i].type == ToolType::Text && bbox.Contains(mouse_pos))
                {
                    hit = i;
                    break;
                }
            }

            if (hit)
            {
                EditTextAnnotation(*hit);
            }
            else
            {
                m_current_annotation       = {};
                m_current_annotation.type  = ToolType::Text;
                m_current_annotation.start = mouse_pos;
                m_current_annotation.end   = m_current_annotation.start;
                m_current_actions.Set(CurrentAction::IsTextPlacing);
                m_focus_text_input = true;
                ++m_text_input_id;
            }
        }

        if (m_current_actions.Has(CurrentAction::IsTextPlacing))
        {
            annotation_t& ann = m_current_annotation;

            // Follows the Text tool's settings, which EditTextAnnotation() set from the edited text
            ann.color     = m_current_color;
            ann.thickness = m_tool_thickness[idx(ToolType::Text)];
            ann.font      = m_inputs.resolved_ann_font_path;

            ImFont*      font = CacheAndGetFont(ann.font);
            const ImVec2 pad(4.0f, 2.0f);

            // As big as the text with room for the next character, so it doesn't scroll. CalcTextSizeA() leaves
            // out a trailing empty line
            const ImVec2 text_size = font->CalcTextSizeA(ann.thickness, FLT_MAX, 0.0f, ann.text.c_str());
            const float  lines     = float(std::ranges::count(ann.text, '\n') + 1);
            const ImVec2 input_size(text_size.x + ann.thickness + pad.x * 2.0f, lines * ann.thickness + pad.y * 2.0f);

            bool   entered;
            ImRect input_rect;
            {
                ui::ScopedStyle style;
                style.Var(ImGuiStyleVar_WindowPadding, ImVec2(0, 0))
                    .Var(ImGuiStyleVar_FramePadding, pad)
                    .Var(ImGuiStyleVar_FrameBorderSize, 1.0f)
                    .Color(ImGuiCol_FrameBg, 0x00000000_rgba)
                    .Color(ImGuiCol_Border, theme::color(AppCol::SelectionBorder))
                    .Color(ImGuiCol_Text, ann.color.to_abgr())
                    // The input has its own border, and the keyboard focus would add the nav cursor's around it
                    .Color(ImGuiCol_NavCursor, 0x00000000_rgba);

                // Placed so the text lines up with where it's drawn once added
                ImGui::SetNextWindowBgAlpha(0.0f);
                const ui::OverlayWindow win("##text_ann_input_win", ann.start - pad);

                // Asked again until it takes, since the click that opened it focuses the overlay at the end of the
                // frame
                if (m_focus_text_input)
                    ImGui::SetKeyboardFocusHere();

                ImGui::PushFont(font, ann.thickness);
                ImGui::PushID(int(m_text_input_id));
                // An edited text gets the caret at its end, not its start
                auto caret_to_end = [](ImGuiInputTextCallbackData* data) {
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
                    return 0;
                };
                entered = ImGui::InputTextMultiline(
                    "##text_ann_input_text",
                    &ann.text,
                    input_size,
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine |
                        ImGuiInputTextFlags_NoHorizontalScroll |
                        (m_focus_text_input ? ImGuiInputTextFlags_CallbackAlways : ImGuiInputTextFlags_None),
                    caret_to_end);
                ImGui::PopID();
                ImGui::PopFont();
                input_rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (ImGui::IsItemActive())
                    m_focus_text_input = false;
            }

            // The keys, below the input in the selection size label's colors so they read on any image
            const char*  hint = _("Enter: add  Shift+Enter: new line  Esc: cancel");
            const ImVec2 hint_pad(6.0f, 3.0f);
            const ImVec2 hint_size(ImGui::CalcTextSize(hint).x + hint_pad.x * 2.0f,
                                   ImGui::GetTextLineHeight() + hint_pad.y * 2.0f);
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImVec2       hint_pos(input_rect.Min.x, input_rect.Max.y + 4.0f);
            if (hint_pos.y + hint_size.y > display.y)
                hint_pos.y = input_rect.Min.y - 4.0f - hint_size.y;
            hint_pos.x = std::clamp(hint_pos.x, 0.0f, std::max(0.0f, display.x - hint_size.x));

            {
                ui::ScopedStyle style;
                style.Var(ImGuiStyleVar_WindowPadding, hint_pad)
                    .Var(ImGuiStyleVar_WindowRounding, 4.0f)
                    .Var(ImGuiStyleVar_WindowBorderSize, 1.0f)
                    .Color(ImGuiCol_WindowBg, theme::color(AppCol::SizeLabelBg))
                    .Color(ImGuiCol_Border, theme::color(AppCol::SizeLabelBorder))
                    .Color(ImGuiCol_Text, theme::color(AppCol::SizeLabelText));
                const ui::OverlayWindow win(
                    "##text_ann_hint", hint_pos, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
                ImGui::TextUnformatted(hint);
            }

            if (entered)
            {
                CommitTextAnnotation();
            }
            // Not the Esc that closed the settings popup
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_popup_open)
            {
                m_current_annotation = {};
                m_edited_text.reset();
                m_current_actions.Clear(CurrentAction::IsTextPlacing);
                ImGui::SetWindowFocus(nullptr);
            }
        }
        return;  // never fall through to the generic drag path
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ui_blocks_selection())
    {
        m_current_actions.Set(CurrentAction::IsDrawing);
        m_current_annotation.type      = m_current_tool;
        m_current_annotation.start     = mouse_pos;
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
        m_current_annotation.end = mouse_pos;

        if (m_current_tool == ToolType::Pencil)
        {
            // Add a point only if it's more than 2px from the last one
            if (!m_current_annotation.points.empty())
            {
                const ImVec2& last = m_current_annotation.points.back();
                float         dx   = mouse_pos.x - last.x;
                float         dy   = mouse_pos.y - last.y;
                if (dx * dx + dy * dy > 4.0f)
                    m_current_annotation.points.push_back(mouse_pos);
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
            m_current_annotation.end = ImVec2(m_current_annotation.start.x + 20.0f, m_current_annotation.start.y);
            should_add               = true;
        }
        if (should_add)
            m_annotations.push_back(m_current_annotation);

        m_current_annotation = annotation_t{};
    }
}

void ScreenshotTool::CommitTextAnnotation()
{
    if (!m_current_actions.Has(CurrentAction::IsTextPlacing))
        return;
    m_current_actions.Clear(CurrentAction::IsTextPlacing);

    const bool empty = m_current_annotation.text.find_first_not_of(" \t\n") == std::string::npos;
    if (m_edited_text && *m_edited_text < m_annotations.size())
    {
        if (empty)
            m_annotations.erase(m_annotations.begin() + std::ptrdiff_t(*m_edited_text));
        else
            m_annotations[*m_edited_text] = m_current_annotation;
    }
    else if (!empty)
    {
        m_annotations.push_back(m_current_annotation);
    }

    m_edited_text.reset();
    m_current_annotation = {};

    // The focus would go back to the toolbar with the nav cursor on a button, which arrows, Enter and Space then use
    ImGui::SetWindowFocus(nullptr);
}

void ScreenshotTool::EditTextAnnotation(const size_t i)
{
    const annotation_t& ann = m_annotations[i];

    m_current_annotation                  = ann;
    m_edited_text                         = i;
    m_current_color                       = ann.color;
    m_tool_thickness[idx(ToolType::Text)] = ann.thickness;
    m_inputs.ann_font                     = ann.font;
    m_inputs.resolved_ann_font_path       = ann.font;

    m_current_tool = ToolType::Text;
    m_state        = ToolState::Selected;
    DeselectAnnotation();
    m_current_actions.Set(CurrentAction::IsTextPlacing);
    m_focus_text_input = true;
    ++m_text_input_id;
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

    {
        ui::ScopedStyle style;
        style.Var(ImGuiStyleVar_WindowPadding, ImVec2(k_padding, k_padding)).Var(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::SetNextWindowBgAlpha(0.9f);
        const ui::OverlayWindow win("##color_loupe", ImVec2(win_x, win_y), ImGuiWindowFlags_NoInputs);
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
                g_cache->SetValue(CacheEntry::AnnColor, m_current_color.to_rgba());
                m_current_actions.Clear(CurrentAction::IsColorPicking);
            }
        }
    }

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
