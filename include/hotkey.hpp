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

/**
 * System-wide shortcut that starts a capture from the tray instance.
 * Registration is only implemented on Windows and macOS: on Linux the
 * WM/DE owns global keybinds, so users bind `oshot --gui` there instead.
 **/
#define OSHOT_HAS_GLOBAL_HOTKEY (OSHOT_WINDOWS || OSHOT_MACOS)

namespace hotkey
{
// Bit flags, combined in hotkey_t::mods
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

/**
 * Parse a hotkey from the config, e.g. "Ctrl+Shift+S", "Cmd+Shift+2", "Alt+PrintScreen" (case insensitive)
 * @param str The hotkey string. Empty or "none" gives an empty (disabled) hotkey
 **/
Result<hotkey_t> parse(std::string_view str);

/**
 * Convert a hotkey to its canonical config string. The opposite of parse()
 * @param hk The hotkey
 * @return e.g. "Alt+Shift+S", or "" for an empty hotkey
 **/
std::string to_string(const hotkey_t& hk);

/**
 * Check whether a key can be the non-modifier part of a hotkey on this platform
 * @param key The key
 **/
bool is_supported_key(ImGuiKey key);

/**
 * Register a hotkey system-wide, replacing the previous one.
 * Must be called after the tray has been initialized
 * @param hk The hotkey. An empty one only unregisters the previous one
 * @param on_pressed What to run when it's pressed, on the tray thread
 **/
Result<> enable(const hotkey_t& hk, std::function<void()> on_pressed);

/* Unregister the hotkey registered by enable(), if any */
void disable();

/**
 * Get the registration state, for the Preferences window
 * @return e.g. "Active: Alt+Shift+S", or empty if this process never registered one
 **/
const std::string& status();
}  // namespace hotkey

#endif  // _HOTKEY_HPP_
