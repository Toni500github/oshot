#include "screenshot_tool.hpp"

#include <tesseract/publictypes.h>
#include <zbar.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
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
#include "screen_capture.hpp"
#include "tinyfiledialogs.h"
#include "tool_icons.h"
#include "util.hpp"

#ifndef GL_NO_ERROR
#  define GL_NO_ERROR 0
#endif

using namespace std::chrono_literals;

static constexpr ImVec4 k_error_color(1.0f, 0.0f, 0.0f, 1.0f);
static constexpr ImVec2 origin(0, 0);
static void*            logo_texture = nullptr;

static std::vector<std::string> get_training_data_list(const std::string& path)
{
    if (!fs::exists(path))
        return {};

    std::vector<std::string> list;
    for (auto const& dir_entry : fs::directory_iterator{ path })
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

static void draw_input_text_path(const char*                  label,
                                 const char*                  input_id,
                                 const bool                   is_file,
                                 const char*                  filters[],
                                 int                          filter_count,
                                 const std::function<void()>& if_edited,
                                 std::string&                 path)
{
    auto handle_drop = [&]() {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !g_dropped_paths.empty())
        {
            path = g_dropped_paths.back();
            g_dropped_paths.clear();
            if_edited();
        }
    };

    const float button_size = ImGui::GetFrameHeight();
    ImGui::PushItemWidth(ImGui::CalcItemWidth() - button_size);
    if (ImGui::InputText(input_id, &path))
        if_edited();
    ImGui::PopItemWidth();
    handle_drop();

    ImGui::SameLine(0, 0);
    if (ImGui::Button("...", ImVec2(button_size, button_size)))
    {
        minimize_window();

        const char* dialog_path = is_file
                                      ? tinyfd_openFileDialog("Open file", nullptr, filter_count, filters, nullptr, 0)
                                      : tinyfd_selectFolderDialog("Open folder", nullptr);

        maximize_window();

        if (dialog_path)
        {
            path.assign(dialog_path);
            if_edited();
        }
    }
    handle_drop();

    ImGui::SameLine(0, 3);
    ImGui::Text("%s", label);
}

static void draw_input_text_file(const char*                  label,
                                 const char*                  input_id,
                                 const char*                  filters[],
                                 int                          filter_count,
                                 const std::function<void()>& if_edited,
                                 std::string&                 path)
{
    draw_input_text_path(label, input_id, true, filters, filter_count, if_edited, path);
}

static void draw_input_text_folder(const char*                  label,
                                   const char*                  input_id,
                                   const std::function<void()>& if_edited,
                                   std::string&                 path)
{
    draw_input_text_path(label, input_id, false, nullptr, 0, if_edited, path);
}

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

static ImVec4 get_confidence_color(const int confidence)
{
    if (confidence <= 45)
        return ImVec4(1, 0, 0, 1);  // red
    else if (confidence <= 70)
        return ImVec4(1, 1, 0, 1);  // yellow

    return ImVec4(0, 1, 0, 1);  // green
}

Result<> ScreenshotTool::Start()
{
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
            case SessionType::KDE:     result = capture_full_screen_spectacle(); break;
            case SessionType::Windows: result = capture_full_screen_windows(); break;
            case SessionType::MacOS:   result = capture_full_screen_macos(); break;
            default:                   return Err("Unknown platform");
        }
    }

    if (!result.ok())
        return Err("Failed to load image: " + result.error_v());

    m_screenshot = std::move(result.get());
    m_tool_thickness.fill(3.0f);
    m_tool_thickness[idx(ToolType::Text)] = 16.0f;
    return Ok();
}

Result<> ScreenshotTool::StartWindow()
{
    m_io    = ImGui::GetIO();
    m_state = ToolState::Selecting;

    m_inputs.ann_font = g_config->File.fonts.size() > 0 ? g_config->File.fonts[0] : "";
    m_show_text_tools = g_config->File.show_text_tools;

    fit_to_screen(m_screenshot);
    RefreshOcrModels();

#ifdef __APPLE__
    m_texture_id = nullptr;  // will be set by backend
#else
    const Result<void*>& res = CreateTexture(m_texture_id, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    if (!res.ok())
        return Err("Failed to create openGL texture: " + res.error_v());

    m_texture_id = res.get();

    // Since the creation of the screenshot texture was fine, suppose the other too
    m_tool_textures[idx(ToolType::Rectangle)] =
        CreateTexture(nullptr, ICON_SQUARE_RGBA, ICON_SQUARE_W, ICON_SQUARE_H).get();
    m_tool_textures[idx(ToolType::RectangleFilled)] =
        CreateTexture(nullptr, ICON_RECT_FILLED_RGBA, ICON_RECT_FILLED_W, ICON_RECT_FILLED_H).get();
    m_tool_textures[idx(ToolType::CircleFilled)] =
        CreateTexture(nullptr, ICON_CIRCLE_FILLED_RGBA, ICON_CIRCLE_FILLED_W, ICON_CIRCLE_FILLED_H).get();
    m_tool_textures[idx(ToolType::ToggleTextTools)] =
        CreateTexture(nullptr, ICON_TEXT_TOOLS_RGBA, ICON_TEXT_TOOLS_W, ICON_TEXT_TOOLS_H).get();
    m_tool_textures[idx(ToolType::Circle)] =
        CreateTexture(nullptr, ICON_CIRCLE_RGBA, ICON_CIRCLE_W, ICON_CIRCLE_H).get();
    m_tool_textures[idx(ToolType::Pencil)] =
        CreateTexture(nullptr, ICON_PENCIL_RGBA, ICON_PENCIL_W, ICON_PENCIL_H).get();

    m_tool_textures[idx(ToolType::Arrow)] = CreateTexture(nullptr, ICON_ARROW_RGBA, ICON_ARROW_W, ICON_ARROW_H).get();
    m_tool_textures[idx(ToolType::Text)]  = CreateTexture(nullptr, ICON_TEXT_RGBA, ICON_TEXT_W, ICON_TEXT_H).get();
    m_tool_textures[idx(ToolType::Line)]  = CreateTexture(nullptr, ICON_LINE_RGBA, ICON_LINE_W, ICON_LINE_H).get();
    logo_texture                          = CreateTexture(nullptr, OSHOT_LOGO_RGBA, OSHOT_LOGO_W, OSHOT_LOGO_H).get();
#endif

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

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_is_color_picking && !m_is_text_placing)
        Cancel();

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
        if (m_is_color_picking)
            HandleColorPickerInput();
        else
            HandleAnnotationInput();

        DrawAnnotationToolbar();
    }

    ImGui::End();
    ImGui::PopStyleVar();

    if (m_state == ToolState::Selected && m_show_text_tools)
    {
        ImGui::Begin("Text tools", &m_show_text_tools, ImGuiWindowFlags_MenuBar);
        DrawMenuItems();
        DrawOcrTools();
        DrawBarDecodeTools();
        ImGui::End();
    }

    HandleShortcutsInput();
}

void ScreenshotTool::HandleShortcutsInput()
{
    if (ImGui::Shortcut(ImGuiKey_E | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
    {
        g_config->File.allow_out_edit = !g_config->File.allow_out_edit;
        ImGui::ClearActiveID();
    }

    if (ImGui::Shortcut(ImGuiKey_Z | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal) && !m_annotations.empty())
        m_annotations.pop_back();

    if (ImGui::Shortcut(ImGuiKey_G | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
        g_config->Runtime.enable_handles = !g_config->Runtime.enable_handles;

    if (ImGui::Shortcut(ImGuiKey_S | ImGuiMod_Ctrl, ImGuiInputFlags_RouteGlobal))
        if (m_on_complete)
            m_on_complete(SavingOp::File, Ok(GetFinalImage()));

    if (ImGui::Shortcut(ImGuiKey_C | ImGuiMod_Ctrl | ImGuiMod_Shift, ImGuiInputFlags_RouteGlobal))
        if (m_on_complete)
            m_on_complete(SavingOp::Clipboard, Ok(GetFinalImage()));
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
        case HandleHovered::Move:
            m_selection.start.x = m_drag_start_selection.start.x + delta.x;
            m_selection.start.y = m_drag_start_selection.start.y + delta.y;
            m_selection.end.x   = m_drag_start_selection.end.x + delta.x;
            m_selection.end.y   = m_drag_start_selection.end.y + delta.y;
            break;
        default: break;
    }

    // When a handle is dragged past the opposite edge, the selection inverts.
    // Normalize it by swapping coordinates, flipping the active handle, and
    // resetting the drag anchor so the delta stays correct next frame.
    if (m_selection.start.x > m_selection.end.x)
    {
        std::swap(m_selection.start.x, m_selection.end.x);
        m_dragging_handle      = flip_handle_x(m_dragging_handle);
        m_drag_start_mouse     = mouse_pos;
        m_drag_start_selection = m_selection;
    }

    if (m_selection.start.y > m_selection.end.y)
    {
        std::swap(m_selection.start.y, m_selection.end.y);
        m_dragging_handle      = flip_handle_y(m_dragging_handle);
        m_drag_start_mouse     = mouse_pos;
        m_drag_start_selection = m_selection;
    }
}

void ScreenshotTool::HandleAnnotationInput()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();

    // If the user switched away from the Text tool while a text annotation was
    // in progress (but not yet committed), cancel it so the placement position
    // is not left stale.
    // Without this, the generic drawing code can later
    // overwrite m_current_annotation (including start.x/y = 0) while
    // m_is_text_placing stays true, causing the input window to reappear at
    // position (0, 0) the next time Text is selected.
    if (m_is_text_placing && m_current_tool != ToolType::Text)
    {
        m_current_annotation = {};
        m_is_text_placing    = false;
    }

    if (m_current_tool == ToolType::Text)
    {
        if (!m_is_text_placing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ui_blocks_selection())
        {
            m_is_text_placing              = true;
            m_current_annotation.type      = ToolType::Text;
            m_current_annotation.start     = { mouse_pos.x, mouse_pos.y };
            m_current_annotation.end       = m_current_annotation.start;
            m_current_annotation.color     = m_current_color;
            m_current_annotation.thickness = m_tool_thickness[idx(ToolType::Text)];
            m_current_annotation.text.clear();
        }

        if (m_is_text_placing)
        {
            const float padding_y =
                std::max(2.0f, (m_current_annotation.thickness - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::SetNextWindowPos(ImVec2(m_current_annotation.start.x, m_current_annotation.start.y));
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, padding_y));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));

            ImGui::Begin("##text_ann_input_win",
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar);

            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            ImFont* ann_font =
                CacheAndGetFont(get_font_path(m_inputs.ann_font).string(), m_current_annotation.thickness);
            ImGui::PushFont(ann_font);

            ImGui::PushStyleColor(ImGuiCol_Text, m_current_annotation.color);
            if (ImGui::InputText(
                    "##text_ann_input_text", &m_current_annotation.text, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (!m_current_annotation.text.empty())
                {
                    // Shift start to where the text actually appears visually,
                    // accounting for the FramePadding we pushed above
                    const ImVec2& fp = ImGui::GetStyle().FramePadding;
                    m_current_annotation.start.x += fp.x;
                    m_current_annotation.start.y += fp.y;

                    m_annotations.push_back(m_current_annotation);
                }
                m_current_annotation = {};
                m_is_text_placing    = false;
            }
            ImGui::PopFont();

            ImGui::Text("Enter: Place | ESC: Cancel");

            ImGui::PopStyleColor();

            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_current_annotation = {};
                m_is_text_placing    = false;
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

void ScreenshotTool::HandleColorPickerInput()
{
    // The loupe acts as our cursor
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    const ImVec2& mouse_pos = ImGui::GetMousePos();
    const int     px        = static_cast<int>(mouse_pos.x - m_image_origin.x);
    const int     py        = static_cast<int>(mouse_pos.y - m_image_origin.y);

    const bool in_image = px >= 0 && px < m_screenshot.w && py >= 0 && py < m_screenshot.h;

    constexpr float k_loupe_px = 140.0f;  // loupe display size (square, pixels)
    constexpr float k_zoom     = 8.0f;    // magnification factor
    constexpr float k_padding  = 10.0f;   // inner window padding
    constexpr float k_offset   = 15.0f;   // distance from cursor to loupe corner
    constexpr float k_win_size = k_loupe_px + k_padding * 2.0f;

    // Position loupe window: prefer bottom-right, flip to stay on screen
    const ImVec2& display = ImGui::GetIO().DisplaySize;
    float         win_x   = mouse_pos.x + k_offset;
    float         win_y   = mouse_pos.y + k_offset;

    // For the horizontal flip we know the exact width; for vertical, use loupe
    // height + a comfortable margin since AlwaysAutoResize determines final height.
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
        ImGui::TextDisabled("Outside of the image");
    }
    else
    {
        // Sample the pixel under the cursor
        const size_t  off = (static_cast<size_t>(py) * m_screenshot.w + px) * 4;
        const uint8_t r   = m_screenshot.data[off + 0];
        const uint8_t g   = m_screenshot.data[off + 1];
        const uint8_t b   = m_screenshot.data[off + 2];
        const uint8_t a   = m_screenshot.data[off + 3];
        const ImVec4  hovered_color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);

        // Compute UV window for the zoomed region
        const float half_src_px_x = (k_loupe_px / k_zoom) * 0.5f / static_cast<float>(m_screenshot.w);
        const float half_src_px_y = (k_loupe_px / k_zoom) * 0.5f / static_cast<float>(m_screenshot.h);
        const float uv_cx         = (static_cast<float>(px) + 0.5f) / static_cast<float>(m_screenshot.w);
        const float uv_cy         = (static_cast<float>(py) + 0.5f) / static_cast<float>(m_screenshot.h);

        const ImVec2 uv_min(uv_cx - half_src_px_x, uv_cy - half_src_px_y);
        const ImVec2 uv_max(uv_cx + half_src_px_x, uv_cy + half_src_px_y);

        // Draw the magnified image
        const ImVec2& loupe_origin = ImGui::GetCursorScreenPos();
        ImGui::Image(m_texture_id, ImVec2(k_loupe_px, k_loupe_px), uv_min, uv_max);

        // Draw crosshair over the loupe
        ImDrawList*     dl  = ImGui::GetWindowDrawList();
        const ImVec2    ctr = ImVec2(loupe_origin.x + k_loupe_px * 0.5f, loupe_origin.y + k_loupe_px * 0.5f);
        constexpr float arm = 10.0f;
        constexpr float gap = 3.0f;  // gap around the centre dot

        // Shadow lines for contrast on any background
        dl->AddLine(ImVec2(ctr.x - arm, ctr.y), ImVec2(ctr.x - gap, ctr.y), IM_COL32(0, 0, 0, 180), 1.5f);
        dl->AddLine(ImVec2(ctr.x + gap, ctr.y), ImVec2(ctr.x + arm, ctr.y), IM_COL32(0, 0, 0, 180), 1.5f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - arm), ImVec2(ctr.x, ctr.y - gap), IM_COL32(0, 0, 0, 180), 1.5f);
        dl->AddLine(ImVec2(ctr.x, ctr.y + gap), ImVec2(ctr.x, ctr.y + arm), IM_COL32(0, 0, 0, 180), 1.5f);
        // White lines on top
        dl->AddLine(ImVec2(ctr.x - arm, ctr.y), ImVec2(ctr.x - gap, ctr.y), IM_COL32(255, 255, 255, 230), 1.0f);
        dl->AddLine(ImVec2(ctr.x + gap, ctr.y), ImVec2(ctr.x + arm, ctr.y), IM_COL32(255, 255, 255, 230), 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - arm), ImVec2(ctr.x, ctr.y - gap), IM_COL32(255, 255, 255, 230), 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y + gap), ImVec2(ctr.x, ctr.y + arm), IM_COL32(255, 255, 255, 230), 1.0f);
        // Centre dot, filled with the hovered colour so it's always visible
        dl->AddCircleFilled(ctr, gap - 0.5f, IM_COL32(r, g, b, 255));
        dl->AddCircle(ctr, gap - 0.5f, IM_COL32(255, 255, 255, 200), 12, 1.0f);

        // Outline around the entire loupe image
        dl->AddRect(loupe_origin,
                    ImVec2(loupe_origin.x + k_loupe_px, loupe_origin.y + k_loupe_px),
                    IM_COL32(80, 80, 80, 220),
                    2.0f,
                    0,
                    1.5f);

        ImGui::Spacing();
        ImGui::ColorButton("##loupe_swatch",
                           hovered_color,
                           ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_AlphaPreview,
                           ImVec2(32, 32));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("#%02X%02X%02X", r, g, b);
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "%-3d ", r);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "%-3d ", g);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 0, 1, 1), "%-3d", b);
        ImGui::EndGroup();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_picker_color     = hovered_color;
            m_current_color    = IM_COL32(r, g, b, a);
            m_is_color_picking = false;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();

    // Keeps a precise reference point visible even outside the loupe region.
    {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddCircle(mouse_pos, 5.0f, IM_COL32(0, 0, 0, 180), 12, 2.0f);
        fg->AddCircleFilled(mouse_pos, 2.0f, IM_COL32(255, 255, 255, 255));
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_is_color_picking = false;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
}

void ScreenshotTool::UpdateHandleHoverState()
{
    const ImVec2& mouse_pos = ImGui::GetMousePos();
    m_handle_hover          = HandleHovered::kNone;

    if (m_state != ToolState::Selected && m_state != ToolState::Resizing)
        return;

    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_w = m_selection.get_width();
    const float sel_h = m_selection.get_height();

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
        const float sel_x = m_selection.get_x();
        const float sel_y = m_selection.get_y();
        const float sel_w = m_selection.get_width();
        const float sel_h = m_selection.get_height();

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

    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_w = m_selection.get_width();
    const float sel_h = m_selection.get_height();

    constexpr ImU32 dark_color = IM_COL32(0, 0, 0, 120);

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

    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_w = m_selection.get_width();
    const float sel_h = m_selection.get_height();

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
        // Now draw the menus
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open Image..."))
            {
                minimize_window();

                const char* filter[]  = { "*.png", "*.jpeg", "*.jpg", "*.bmp" };
                const char* open_path = tinyfd_openFileDialog("Open Image",
                                                              "",                // default path
                                                              4,                 // number of filter patterns
                                                              filter,            // file filters
                                                              "Images (*.png)",  // filter description
                                                              false              // allow multiple selections
                );

                maximize_window();

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
            ImGui::MenuItem("Anns. in image scans", "", &g_config->File.render_anns);
            if (ImGui::MenuItem("Enable vsync", "", &g_config->File.enable_vsync))
                extern_glfwSwapInterval(static_cast<int>(g_config->File.enable_vsync));
            if (ImGui::MenuItem("Allow text edit", "CTRL+E", &g_config->File.allow_out_edit))
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
        ImGui::SetNextWindowSize(ImVec2(350, 250), ImGuiCond_FirstUseEver);
        ImGui::Begin("About", &show_about, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        float            window_width = ImGui::GetWindowSize().x;
        std::string_view text_display;

        auto centered_text = [&](const std::string_view text) -> std::string_view {
            float name_width = ImGui::CalcTextSize(text.data()).x;
            ImGui::SetCursorPosX((window_width - name_width) / 2);
            return text;
        };

        // Center image
        ImGui::SetCursorPosX((window_width - 24.0f) / 2);
        ImGui::Image(logo_texture, ImVec2(32, 32));

        // Centered labels
        text_display = centered_text("oshot v" VERSION);
        ImGui::Text("%s", text_display.data());
        ImGui::Spacing();

        text_display = centered_text("Screenshot tool for extracting text on the fly");
        ImGui::Text("%s", text_display.data());
        ImGui::Spacing();

        // Rest remains left-aligned as normal
        text_display = "More Details:";
        if (ImGui::TreeNode(text_display.data()))
        {
            ImGui::BeginChild("##scrollable_region", ImVec2(0, 100), false, ImGuiWindowFlags_HorizontalScrollbar);

            ImGui::Text("%s", version_infos.c_str());

            if (ImGui::Button("Copy text"))
                g_clipboard->CopyText(version_infos);
            ImGui::EndChild();
            ImGui::TreePop();
        }

        ImGui::Text("Version: v" VERSION);
        ImGui::Text("Created by: Toni500");
        ImGui::Text("Copyright © 2026");
        ImGui::Spacing();

        ImGui::Text("Support the project at ");
        ImGui::SameLine(0, 1);
        if (ImGui::TextLinkOpenURL("Toni500github/oshot", "https://github.com/Toni500github/oshot"))
            minimize_window();

        if (ImGui::Button("Close"))
            show_about = false;
        ImGui::End();
    }
}

void ScreenshotTool::DrawOcrTools()
{
    std::string& ocr_path  = m_inputs.ocr_path;
    std::string& ocr_model = m_inputs.ocr_model;

    static size_t item_selected_idx = 0;

    auto refresh_models = [&]() {
        RefreshOcrModels();
        const auto& it    = std::find(m_ocr_models_list.begin(), m_ocr_models_list.end(), ocr_model);
        item_selected_idx = (it != m_ocr_models_list.end()) ? std::distance(m_ocr_models_list.begin(), it) : 0;
    };

    auto push_error_style = [](bool cond) {
        if (cond)
            ImGui::PushStyleColor(ImGuiCol_Text, k_error_color);
    };
    auto pop_error_label = [](bool cond, const char* label) {
        if (cond)
        {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(k_error_color, "%s", label);
        }
    };

    ImGui::PushID("OcrTools");
    ImGui::SeparatorText("OCR");

    const bool invalid_path  = HasError(InvalidPath);
    const bool invalid_model = HasError(InvalidModel);

    // --- Path input ---
    push_error_style(invalid_path);
    draw_input_text_folder("Path", "##ocr_path", refresh_models, ocr_path);
    pop_error_label(invalid_path, "Invalid!");
    ImGui::SameLine();
    HelpMarker("Full path to the OCR models (.traineddata). Supports drag-and-drop");

    // --- Model combo (only shown when path is valid) ---
    if (!invalid_path)
    {
        push_error_style(invalid_model);
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

            for (size_t i = 0; i < m_ocr_models_list.size(); ++i)
            {
                if (filter.PassFilter(m_ocr_models_list[i].c_str()))
                {
                    const bool is_selected = (item_selected_idx == i);
                    if (ImGui::Selectable(m_ocr_models_list[i].c_str(), is_selected))
                    {
                        item_selected_idx = i;
                        ocr_model         = m_ocr_models_list[i];
                        ClearError(InvalidModel);
                    }
                }
            }
            ImGui::EndCombo();
        }
        pop_error_label(invalid_model, "Invalid!");
    }

    // --- Extract button + result details ---
    if (!invalid_path && !invalid_model)
    {
        if (ImGui::Button("Extract Text"))
        {
            const Result<>& res = m_ocr_api.Configure(ocr_path.c_str(), ocr_model.c_str());
            if (!res.ok())
                SetError(FailedToScanOcr, res.error_v());
            else
            {
                ClearError(FailedToScanOcr);
                Result<ocr_result_t> result = m_ocr_api.ExtractTextCapture(GetFinalImage(true));
                if (result.ok())
                    m_inputs.ocr_results = std::move(result.get());
            }
        }

        if (HasError(FailedToScanOcr))
        {
            ImGui::SameLine();
            ImGui::TextColored(k_error_color, "Failed to scan: %s", GetError(FailedToScanOcr).c_str());
        }
        else
        {
            ImGui::SameLine();
            HelpMarker("If results seem off, try Edit > Optimize OCR for...");
        }

        if (m_inputs.ocr_results.confidence > 0 && ImGui::TreeNode("Details"))
        {
            ImGui::BulletText("Confidence:");
            ImGui::SameLine();
            ImGui::TextColored(
                get_confidence_color(m_inputs.ocr_results.confidence), "%d%%", m_inputs.ocr_results.confidence);

            ImGui::BulletText("PSM: %s", m_inputs.ocr_results.psm_str.c_str());
            ImGui::TreePop();
        }
    }

    ImGui::InputTextMultiline("##source",
                              &m_inputs.ocr_results.data,
                              ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                              g_config->File.allow_out_edit ? 0 : ImGuiInputTextFlags_ReadOnly);

    if (!m_inputs.ocr_results.data.empty())
        CreateCopyTextButton(m_inputs.ocr_results.data);

    ImGui::PopID();
}

void ScreenshotTool::DrawBarDecodeTools()
{
    ImGui::PushID("BarDecodeTools");
    ImGui::SeparatorText("QR/Bar Decode");

    if (ImGui::Button("Extract Text"))
    {
        const Result<zbar_result_t>& scan = m_zbar_api.ExtractTextsCapture(GetFinalImage(true));
        if (!scan.ok())
        {
            SetError(FailedToScanBarCode, scan.error_v());
        }
        else
        {
            ClearError(FailedToScanBarCode);
            m_inputs.zbar_scan_result = std::move(scan.get());
            m_inputs.barcode_text.clear();
            for (const auto& data : m_inputs.zbar_scan_result.datas)
                m_inputs.barcode_text += data + "\n\n";
        }
    }

    if (HasError(FailedToScanBarCode))
    {
        ImGui::SameLine();
        ImGui::TextColored(k_error_color, "Failed to decode: %s", GetError(FailedToScanBarCode).c_str());
    }
    else if (!m_inputs.zbar_scan_result.datas.empty() && ImGui::TreeNode("Details"))
    {
        ImGui::Text("Detected barcodes:");
        for (const auto& [sym, count] : m_inputs.zbar_scan_result.symbologies)
            ImGui::BulletText("%s (x%d)", sym.c_str(), count);
        ImGui::TreePop();
    }

    ImGui::InputTextMultiline("##barcode",
                              &m_inputs.barcode_text,
                              ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                              g_config->File.allow_out_edit ? 0 : ImGuiInputTextFlags_ReadOnly);

    if (!m_inputs.barcode_text.empty())
        CreateCopyTextButton(m_inputs.barcode_text);

    ImGui::PopID();
}

void ScreenshotTool::DrawAnnotationToolbar()
{
    static int                   item_picker      = 0;
    static constexpr const char* color_pickers[2] = { "Bar - Square", "Wheel - Triangle" };

    const float sel_x = m_selection.get_x();
    const float sel_y = m_selection.get_y();
    const float sel_h = m_selection.get_height();

    const ImVec2 toolbar_pos(sel_x, sel_y + sel_h + 10);

    ImGui::SetNextWindowPos(toolbar_pos);
    ImGui::Begin("##annotation_toolbar",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);

    // Tool selection buttons
    auto draw_and_set_button = [&](ToolType tool, const char* id, void* texture) {
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
            if (m_current_tool == ToolType::Text)
                m_tool_thickness[idx(m_current_tool)] = std::clamp(m_tool_thickness[idx(m_current_tool)], 8.0f, 144.0f);
            else
                m_tool_thickness[idx(m_current_tool)] = std::clamp(m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f);

            static ImGuiColorEditFlags color_picker_flags = ImGuiColorEditFlags_AlphaBar;

            ImGui::TextUnformatted("Annotation Settings");
            ImGui::Separator();
            ImGui::SetNextItemWidth(100);

            if (m_current_tool == ToolType::Text)
            {
                ImGui::InputFloat("##fontsize", &m_tool_thickness[idx(m_current_tool)], 2.0f, 2.0f, "%.0f px");
                ImGui::SameLine();
                ImGui::TextUnformatted("Font Size");
                static const char* font_filters[] = { "*.ttf", "*.otf", "*.woff", "*.woff2" };
                draw_input_text_file(
                    "Font name/path", "##font_path_ann_settings", font_filters, 4, [] {}, m_inputs.ann_font);
            }
            else
            {
                ImGui::SliderFloat("##thickness", &m_tool_thickness[idx(m_current_tool)], 1.0f, 10.0f, "%.2f");
                ImGui::SameLine();
                ImGui::TextUnformatted("Thickness");
            }

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

            if (ImGui::Button("Pick color"))
            {
                m_is_color_picking = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            HelpMarker("Click anywhere on the image to pick a color");

            ImGui::ColorPicker4("Color", reinterpret_cast<float*>(&m_picker_color), color_picker_flags);

            m_current_color = ImGui::ColorConvertFloat4ToU32(m_picker_color);
            ImGui::EndPopup();
        }

        ImGui::SameLine();
    };

    draw_and_set_button(ToolType::Arrow, "##Arrow", m_tool_textures[idx(ToolType::Arrow)]);
    draw_and_set_button(ToolType::Rectangle, "##Rectangle", m_tool_textures[idx(ToolType::Rectangle)]);
    draw_and_set_button(
        ToolType::RectangleFilled, "##Rectangle_filled", m_tool_textures[idx(ToolType::RectangleFilled)]);
    draw_and_set_button(ToolType::Circle, "##Circle", m_tool_textures[idx(ToolType::Circle)]);
    draw_and_set_button(ToolType::CircleFilled, "##Circle_filled", m_tool_textures[idx(ToolType::CircleFilled)]);
    draw_and_set_button(ToolType::Line, "##Line", m_tool_textures[idx(ToolType::Line)]);
    draw_and_set_button(ToolType::Text, "##icon_Text", m_tool_textures[idx(ToolType::Text)]);
    draw_and_set_button(ToolType::Pencil, "##Pencil", m_tool_textures[idx(ToolType::Pencil)]);

    if (!m_show_text_tools &&
        ImGui::ImageButton("##ShowTextTools", m_tool_textures[idx(ToolType::ToggleTextTools)], ImVec2(24, 24)))
        m_show_text_tools = true;

    ImGui::SameLine();
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

    auto draw_text = [&](const annotation_t& ann, const ImVec2& p1) {
        const float font_size = ann.thickness > 8.0f ? ann.thickness : ImGui::GetFontSize();
        ImFont*     font      = CacheAndGetFont(get_font_path(m_inputs.ann_font).string(), font_size);
        draw_list->AddText(font, font_size, p1, ann.color, ann.text.c_str());
    };

    auto draw_rectangle_or_filled =
        [&](const bool filled, const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
            ImVec2 min(std::min(p1.x, p2.x), std::min(p1.y, p2.y));
            ImVec2 max(std::max(p1.x, p2.x), std::max(p1.y, p2.y));
            filled ? draw_list->AddRectFilled(min, max, ann.color, 0.0f, 0)
                   : draw_list->AddRect(min, max, ann.color, 0.0f, 0, t);
        };

    auto draw_circle_or_filled =
        [&](const bool filled, const annotation_t& ann, const ImVec2& p1, const ImVec2& p2, const float t) {
            float dx     = p2.x - p1.x;
            float dy     = p2.y - p1.y;
            float radius = std::sqrt(dx * dx + dy * dy);
            filled ? draw_list->AddCircleFilled(p1, radius, ann.color, 0)
                   : draw_list->AddCircle(p1, radius, ann.color, 0, t);
        };

    auto draw_pencil = [&](const annotation_t& ann, const float t) {
        if (ann.points.size() > 1)
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
        float  len = std::sqrt(v.x * v.x + v.y * v.y);
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

    auto draw_annotation = [&](const annotation_t& ann) {
        const ImVec2 p1(ann.start.x, ann.start.y);
        const ImVec2 p2(ann.end.x, ann.end.y);
        const float  t = ann.thickness * dpi;

        switch (ann.type)
        {
            case ToolType::Line:            draw_line(ann, p1, p2, t); break;
            case ToolType::Arrow:           draw_arrow(ann, p1, p2, t); break;
            case ToolType::Rectangle:       draw_rectangle_or_filled(false, ann, p1, p2, t); break;
            case ToolType::RectangleFilled: draw_rectangle_or_filled(true, ann, p1, p2, t); break;
            case ToolType::Circle:          draw_circle_or_filled(false, ann, p1, p2, t); break;
            case ToolType::CircleFilled:    draw_circle_or_filled(true, ann, p1, p2, t); break;
            case ToolType::Text:            draw_text(ann, p1); break;
            case ToolType::Pencil:          draw_pencil(ann, t); break;

            default: break;
        }
    };

    // Render current annotation being drawn
    if (m_is_drawing)
        draw_annotation(m_current_annotation);

    for (const annotation_t& ann : m_annotations)
        draw_annotation(ann);
}

void ScreenshotTool::Cancel()
{
    m_state = ToolState::Idle;

    auto delete_texture = [](void*& tex) {
#ifdef __APPLE__
        tex = nullptr;
#else
        if (tex)
        {
            GLuint texture = (GLuint)(intptr_t)tex;
            glDeleteTextures(1, &texture);
            tex = nullptr;
        }
#endif
    };

    delete_texture(m_texture_id);
    delete_texture(logo_texture);
    for (void*& tex : m_tool_textures)
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
    fit_to_screen(m_screenshot);

#ifdef __APPLE__
    // Tell backend to recreate Metal texture
    if (m_on_image_reload)
        m_on_image_reload(m_screenshot);
#else

    // Recreate texture (CreateTexture() already deletes the old ones)
    const Result<void*>& r = CreateTexture(m_texture_id, m_screenshot.view(), m_screenshot.w, m_screenshot.h);
    if (!r.ok())
    {
        error("Failed to create openGL texture: {}", r.error());
        return false;
    }

    m_texture_id = r.get();
#endif

    // Reset interactions.
    // somes are already reseted from previous calls
    m_state           = ToolState::Selecting;
    m_current_tool    = ToolType::kNone;
    m_handle_hover    = HandleHovered::kNone;
    m_dragging_handle = HandleHovered::kNone;
    m_input_owner     = InputOwner::kNone;

    m_is_selecting         = false;
    m_selection            = {};
    m_drag_start_selection = {};
    m_drag_start_mouse     = {};
    m_image_origin         = {};
    m_image_end            = {};

    return true;
}

capture_result_t ScreenshotTool::GetFinalImage(bool is_text_tools)
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

        std::memcpy(dst.data() + dst_row_start, src.data() + src_row_start, bytes_to_copy);
    }

    if (is_text_tools && !g_config->File.render_anns)
        return result;

    // Render annotations to the final image
    const float offset_x = m_selection.get_x();
    const float offset_y = m_selection.get_y();

    auto set_pixel = [&](int x, int y, uint32_t color) {
        if (x < 0 || x >= result.w || y < 0 || y >= result.h)
            return;

        uint8_t src_r = (color >> 0) & 0xFF;
        uint8_t src_g = (color >> 8) & 0xFF;
        uint8_t src_b = (color >> 16) & 0xFF;
        uint8_t src_a = (color >> 24) & 0xFF;

        size_t idx = (static_cast<size_t>(y) * result.w + x) * 4;

        if (src_a == 255)
        {
            result.data[idx + 0] = src_r;
            result.data[idx + 1] = src_g;
            result.data[idx + 2] = src_b;
            result.data[idx + 3] = 255;
            return;
        }

        // Blend
        uint8_t dst_r = result.data[idx + 0];
        uint8_t dst_g = result.data[idx + 1];
        uint8_t dst_b = result.data[idx + 2];
        uint8_t dst_a = result.data[idx + 3];

        float a  = src_a / 255.0f;
        float ia = 1.0f - a;

        result.data[idx + 0] = uint8_t(src_r * a + dst_r * ia);
        result.data[idx + 1] = uint8_t(src_g * a + dst_g * ia);
        result.data[idx + 2] = uint8_t(src_b * a + dst_b * ia);
        result.data[idx + 3] = uint8_t(src_a + dst_a * (1.0f - a));
    };

    auto draw_line = [&](int x0, int y0, int x1, int y1, uint32_t color, float thickness) {
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
                        set_pixel(x0 + ox, y0 + oy, color);

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
            case ToolType::kNone:
            case ToolType::Count:
            case ToolType::ToggleTextTools: break;

            case ToolType::Line:
            case ToolType::Arrow:
                draw_line(x1, y1, x2, y2, ann.color, ann.thickness);
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
                        draw_line(x2, y2, ax1, ay1, ann.color, ann.thickness);
                        draw_line(x2, y2, ax2, ay2, ann.color, ann.thickness);
                    }
                }
                break;

            case ToolType::Text:
            {
                if (ann.text.empty())
                    break;

                const float font_size = ann.thickness > 8.0f ? ann.thickness : ImGui::GetFontSize();
                ImFont*     font      = CacheAndGetFont(get_font_path(m_inputs.ann_font).string(), font_size);
                if (!font || !font->OwnerAtlas)
                    break;

                ImFontBaked* baked = font->GetFontBaked(font_size);
                if (!baked)
                    break;

                unsigned char* pixels  = nullptr;
                int            atlas_w = 0, atlas_h = 0;
                font->OwnerAtlas->GetTexDataAsRGBA32(&pixels, &atlas_w, &atlas_h);
                if (!pixels || atlas_w == 0 || atlas_h == 0)
                    break;

                const uint32_t* font_pixels = reinterpret_cast<const uint32_t*>(pixels);

                const uint8_t col_r = (ann.color >> 0) & 0xFF;
                const uint8_t col_g = (ann.color >> 8) & 0xFF;
                const uint8_t col_b = (ann.color >> 16) & 0xFF;
                const uint8_t col_a = (ann.color >> 24) & 0xFF;

                float       cursor_x = static_cast<float>(x1);
                float       cursor_y = static_cast<float>(y1);
                const char* p        = ann.text.c_str();
                const char* end      = p + ann.text.size();

                while (p < end)
                {
                    unsigned int codepoint = 0;
                    p += ImTextCharFromUtf8(&codepoint, p, end);
                    if (codepoint == 0)
                        break;

                    const ImFontGlyph* glyph = baked->FindGlyph(static_cast<ImWchar>(codepoint));
                    if (!glyph)
                        continue;

                    const int dst_x0 = static_cast<int>(cursor_x + glyph->X0);
                    const int dst_y0 = static_cast<int>(cursor_y + glyph->Y0);
                    const int dst_x1 = static_cast<int>(cursor_x + glyph->X1);
                    const int dst_y1 = static_cast<int>(cursor_y + glyph->Y1);

                    const int src_x0 = static_cast<int>(glyph->U0 * atlas_w);
                    const int src_y0 = static_cast<int>(glyph->V0 * atlas_h);
                    const int src_x1 = static_cast<int>(glyph->U1 * atlas_w);
                    const int src_y1 = static_cast<int>(glyph->V1 * atlas_h);

                    const int dst_gw = dst_x1 - dst_x0;
                    const int dst_gh = dst_y1 - dst_y0;
                    const int src_gw = src_x1 - src_x0;
                    const int src_gh = src_y1 - src_y0;

                    if (dst_gw <= 0 || dst_gh <= 0 || src_gw <= 0 || src_gh <= 0)
                    {
                        cursor_x += glyph->AdvanceX;
                        continue;
                    }

                    for (int dy = 0; dy < dst_gh; ++dy)
                    {
                        const int src_ay = src_y0 + dy * src_gh / dst_gh;
                        if (src_ay < 0 || src_ay >= atlas_h)
                            continue;

                        for (int dx = 0; dx < dst_gw; ++dx)
                        {
                            const int src_ax = src_x0 + dx * src_gw / dst_gw;
                            if (src_ax < 0 || src_ax >= atlas_w)
                                continue;

                            const uint32_t atlas_px    = font_pixels[src_ay * atlas_w + src_ax];
                            const uint8_t  glyph_alpha = static_cast<uint8_t>((atlas_px >> 24) & 0xFF);
                            if (glyph_alpha == 0)
                                continue;

                            uint8_t  src_a       = col_a * glyph_alpha / 255u;
                            uint32_t final_color = (col_r) | (col_g << 8) | (col_b << 16) | (src_a << 24);

                            set_pixel(dst_x0 + dx, dst_y0 + dy, final_color);
                        }
                    }

                    cursor_x += glyph->AdvanceX;
                }
                break;
            }

            case ToolType::Rectangle:
                draw_line(x1, y1, x2, y1, ann.color, ann.thickness);
                draw_line(x2, y1, x2, y2, ann.color, ann.thickness);
                draw_line(x2, y2, x1, y2, ann.color, ann.thickness);
                draw_line(x1, y2, x1, y1, ann.color, ann.thickness);
                break;

            case ToolType::RectangleFilled:
            {
                int rx1 = std::min(x1, x2);
                int rx2 = std::max(x1, x2);
                int ry1 = std::min(y1, y2);
                int ry2 = std::max(y1, y2);
                for (int fy = ry1; fy <= ry2; ++fy)
                    for (int fx = rx1; fx <= rx2; ++fx)
                        set_pixel(fx, fy, ann.color);
                break;
            }

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
                                set_pixel(cx + x + ox, cy + y + oy, ann.color);
                                set_pixel(cx + y + ox, cy + x + oy, ann.color);
                                set_pixel(cx - y + ox, cy + x + oy, ann.color);
                                set_pixel(cx - x + ox, cy + y + oy, ann.color);
                                set_pixel(cx - x + ox, cy - y + oy, ann.color);
                                set_pixel(cx - y + ox, cy - x + oy, ann.color);
                                set_pixel(cx + y + ox, cy - x + oy, ann.color);
                                set_pixel(cx + x + ox, cy - y + oy, ann.color);
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

            case ToolType::CircleFilled:
            {
                int cx     = x1;
                int cy     = y1;
                int radius = static_cast<int>(std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)));
                for (int fy = cy - radius; fy <= cy + radius; ++fy)
                    for (int fx = cx - radius; fx <= cx + radius; ++fx)
                        if ((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) <= radius * radius)
                            set_pixel(fx, fy, ann.color);
                break;
            }

            case ToolType::Pencil:
                for (size_t i = 1; i < ann.points.size(); ++i)
                {
                    int px1 = static_cast<int>(ann.points[i - 1].x - offset_x);
                    int py1 = static_cast<int>(ann.points[i - 1].y - offset_y);
                    int px2 = static_cast<int>(ann.points[i].x - offset_x);
                    int py2 = static_cast<int>(ann.points[i].y - offset_y);
                    draw_line(px1, py1, px2, py2, ann.color, ann.thickness);
                }
                break;
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

ImFont* ScreenshotTool::CacheAndGetFont(const std::string& font_path, const float font_size)
{
    if (font_path.empty())
        return ImGui::GetDefaultFont();

    const float safe_size = std::max(font_size, 16.0f);
    std::pair   key(font_path, safe_size);
    auto        it = m_font_cache.find(key);
    if (it != m_font_cache.end())
        return it->second.font;

    ImFont* font =
        m_io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_size, nullptr, m_io.Fonts->GetGlyphRangesDefault());

    m_font_cache[key] = { font_path, font, true };
    if (font)
        m_io.Fonts->Build();

    return font;
}

void ScreenshotTool::CreateCopyTextButton(const std::string& text_copy)
{
    static bool   just_copied = false;
    static double copy_time   = 0.0;

    const double now = ImGui::GetTime();
    if (just_copied && now - copy_time > 1.5)
        just_copied = false;

    if (ImGui::Button(just_copied ? "Copied!" : "Copy Text"))
    {
        const Result<>& res = g_clipboard->CopyText(text_copy);
        if (!res.ok())
        {
            SetError(FailedToCopyText, res.error_v());
        }
        else
        {
            ClearError(FailedToCopyText);
            just_copied = true;
            copy_time   = now;
        }
    }

    if (HasError(FailedToCopyText))
    {
        ImGui::SameLine();
        ImGui::TextColored(k_error_color, "Failed to copy text: %s", GetError(FailedToCopyText).c_str());
    }
}

void ScreenshotTool::RefreshOcrModels()
{
    m_ocr_models_list = get_training_data_list(m_inputs.ocr_path);
    if (m_ocr_models_list.empty())
    {
        SetError(InvalidPath);
    }
    else
    {
        ClearError(InvalidPath);
        const auto& it = std::find(m_ocr_models_list.begin(), m_ocr_models_list.end(), m_inputs.ocr_model);
        if (it == m_ocr_models_list.end())
            SetError(InvalidModel);
        else
            ClearError(InvalidModel);
    }
}

Result<void*> ScreenshotTool::CreateTexture(void* tex, std::span<const uint8_t> data, int w, int h)
{
#ifdef __APPLE__
    // Metal backend handles textures separately
    return Ok(nullptr);
#else
    // Existing OpenGL implementation
    if (tex)
    {
        GLuint old_texture = (GLuint)(intptr_t)tex;
        glDeleteTextures(1, &old_texture);
    }

    GLuint texture;
    glGenTextures(1, &texture);

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());

    tex = (void*)(intptr_t)texture;
    return Ok(tex);
#endif
}
