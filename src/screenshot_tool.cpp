#include "screenshot_tool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// clang-format off
// Because of X11 headers now I need to wonder
// about the order of each included header file.
#include "translation.hpp"
// clang-format on

#include "config.hpp"
#include "fmt/base.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3_loader.h"
#include "imgui/imgui_stdlib.h"
#include "langs.hpp"
#include "screen_capture.hpp"
#include "util.hpp"

static ImVec2 origin(0, 0);

static std::unique_ptr<Translator> translator;

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

    switch (get_session_type())
    {
        case X11:        m_screenshot = capture_full_screen_x11(); break;
        case WAYLAND:    m_screenshot = capture_full_screen_wayland(); break;
        case OS_WINDOWS: m_screenshot = capture_full_screen_windows(); break;
        default:         ;
    }

    if (!m_screenshot.success || m_screenshot.data.empty() || !m_screenshot.error_msg.empty())
    {
        m_state = ToolState::Idle;
        fmt::println(stderr, "Failed to do data screenshot: {}", m_screenshot.error_msg);
        return false;
    }
    m_state = ToolState::Selecting;
    CreateTexture();
    return true;
}

bool ScreenshotTool::RenderOverlay()
{
    if (!IsActive())
        return false;

    // Create fullscreen overlay window
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(m_io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, origin);
    ImGui::Begin("Screenshot Tool",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground);

    if (m_screenshot.data.empty() || !m_screenshot.success || !m_texture_id)
        return false;

    // Draw the screenshot as background
    ImGui::GetBackgroundDrawList()->AddImage(
        m_texture_id, origin, ImVec2(m_screenshot.region.width, m_screenshot.region.height));

    if (m_state == ToolState::Selecting || m_state == ToolState::Selected)
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
        ImGui::Begin("Text tools");
        ImVec2 window_pos  = ImGui::GetWindowPos();
        ImVec2 window_size = ImGui::GetWindowSize();
        m_is_hovering_ocr  = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootWindow) ||
                            (ImGui::IsMouseHoveringRect(
                                window_pos, ImVec2(window_pos.x + window_size.x, window_pos.y + window_size.y)));
        DrawOcrTools();
        DrawTranslationTools();
        ImGui::End();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        Cancel();
        return false;
    }

    if (!m_is_hovering_ocr && ImGui::IsKeyPressed(ImGuiKey_Enter) && m_state == ToolState::Selected)
    {
        if (m_on_complete)
            m_on_complete(GetFinalImage());

        m_state = ToolState::Idle;
        return false;
    }

    return true;
}

void ScreenshotTool::HandleSelectionInput()
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

void ScreenshotTool::DrawDarkOverlay()
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

void ScreenshotTool::DrawSelectionBorder()
{
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    float sel_x = m_selection.get_x();
    float sel_y = m_selection.get_y();
    float sel_w = m_selection.get_width();
    float sel_h = m_selection.get_height();

    draw_list->AddRect(
        ImVec2(sel_x, sel_y), ImVec2(sel_x + sel_w, sel_y + sel_h), IM_COL32(0, 150, 255, 255), 0.0f, 0, 2.0f);
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

    if (!HasError(ErrorState::InvalidPath) &&
        ImGui::BeginCombo("Model", ocr_model.c_str(), ImGuiComboFlags_HeightLarge))
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
        ClearError(ErrorState::InvalidModel);
        ImGui::EndCombo();
    }
    else if (HasError(ErrorState::InvalidPath))
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
    if (!HasErrors() && ImGui::Button("Extract Text"))
    {
        if (std::find(list.begin(), list.end(), ocr_model) == list.end())
        {
            SetError(InvalidModel);
        }
        else if (!m_api.Init(ocr_path, ocr_model))
        {
            SetError(InitOcr);
        }
        else
        {
            m_api.SetImage(GetFinalImage());
            const auto& text = m_api.ExtractText();
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

    ImGui::InputTextMultiline(
        "##source", &m_ocr_text, ImVec2(-1, ImGui::GetTextLineHeight() * 10), ImGuiInputTextFlags_ReadOnly);
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
            if (HasError(err))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
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
                            ImGui::PopStyleColor();
                        }
                    }
                }
                ImGui::EndCombo();
            }

            if (HasError(err))
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
    HelpMarker("The translation is done by online services such as Google translate");

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
    region_t region{
        static_cast<int>(m_selection.get_x()),
        static_cast<int>(m_selection.get_y()),
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

bool ScreenshotTool::HasErrors()
{
    return m_err_state != 0;
}

void ScreenshotTool::SetError(ErrorState err)
{
    m_err_state |= err;
}

void ScreenshotTool::ClearError(ErrorState err)
{
    m_err_state &= ~err;
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

void ScreenshotTool::CreateTexture()
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
