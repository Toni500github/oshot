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

#ifndef _SCREENSHOT_TOOL_HELPERS_HPP_
#define _SCREENSHOT_TOOL_HELPERS_HPP_

// Shared by the src/screenshot_tool/*.cpp files

#include <cstdint>

#include "imgui/imgui.h"
#include "platform.hpp"
#include "screenshot_tool.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "util/color.hpp"

// ImGui's shortcuts use Cmd where they use Ctrl elsewhere
#if OSHOT_MACOS
#  define SHORTCUT_MOD "CMD"
#else
#  define SHORTCUT_MOD "CTRL"
#endif

#ifdef DISABLE_PLUGINS
#  define OSHOT_TEXT_OCR     "ocr_output"
#  define OSHOT_TEXT_BARCODE "barcode_output"
#endif

constexpr rgba_t::rgba_t(ImVec4 vec)
    : r(static_cast<uint8_t>(vec.x * 255.0f)),
      g(static_cast<uint8_t>(vec.y * 255.0f)),
      b(static_cast<uint8_t>(vec.z * 255.0f)),
      a(static_cast<uint8_t>(vec.w * 255.0f))
{}

constexpr ImVec4 rgba_t::to_imvec4() const
{
    return { float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, float(a) / 255.0f };
}

constexpr rgba_t::operator ImVec4() const
{
    return to_imvec4();
}

// Shows the error in the theme's error color if it's set, and returns whether it is
template <typename Enum>
bool ShowIfError(const ErrorContext<Enum>& ctx, Enum e)
{
    if (!ctx.Has(e))
        return false;
    ui::TextWrappedColored(theme::color(AppCol::Error), "%s", ctx.Get(e).c_str());
    return true;
}

// Tools that draw a button in the toolbar
constexpr bool has_toolbar_button(ToolType t)
{
    return t != ToolType::kNone && t != ToolType::COUNT && t != ToolType::Logo;
}

#endif  // _SCREENSHOT_TOOL_HELPERS_HPP_
