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

#ifndef _HOTKEY_HPP_
#define _HOTKEY_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "imgui/imgui.h"
#include "platform.hpp"
#include "util.hpp"

// System-wide shortcut that starts a capture from the tray instance.
// Registration is only implemented on Windows and macOS: on Linux the
// WM/DE owns global keybinds, so users bind `oshot --gui` there instead.
#define OSHOT_HAS_GLOBAL_HOTKEY (OSHOT_WINDOWS || OSHOT_MACOS)

namespace hotkey
{
enum Mod : std::uint8_t
{
    kNone = 0,
    Ctrl  = 1 << 0,
    Shift = 1 << 1,
    Alt   = 1 << 2,
    Super = 1 << 3,  // Win on Windows, Cmd on macOS
};

struct hotkey_t
{
    std::uint8_t mods = Mod::kNone;
    ImGuiKey     key  = ImGuiKey_None;

    [[nodiscard]] bool empty() const { return key == ImGuiKey_None; }
    bool               operator==(const hotkey_t&) const = default;
};

// Parse e.g. "Ctrl+Shift+S", "Cmd+Shift+2", "Alt+PrintScreen" (case insensitive).
// An empty string or "none" gives an empty (disabled) hotkey.
Result<hotkey_t> parse(std::string_view str);

// Canonical config string, e.g. "Alt+Shift+S". Empty hotkey gives "".
std::string to_string(const hotkey_t& hk);

// Whether `key` can be the non-modifier part of a hotkey on this platform
bool is_supported_key(ImGuiKey key);

// Register `hk` system-wide, replacing the previous one. `on_pressed` runs on
// the tray thread. Must be called after the tray has been initialized.
Result<> enable(const hotkey_t& hk, std::function<void()> on_pressed);
void     disable();

// Registration state for the Preferences window, empty if this process never registered one
const std::string& status();
}  // namespace hotkey

#endif  // _HOTKEY_HPP_
