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

#include "hotkey.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "i18n.hpp"

// clang-format off
#if OSHOT_WINDOWS
#  include <windows.h>
#  include "tray.hpp"
#elif OSHOT_MACOS
// AssertMacros.h (pulled in by Carbon) defines check()/verify()/require() otherwise
#  define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#  include <Carbon/Carbon.h>
#endif
// clang-format on

namespace hotkey
{
namespace
{
struct named_key_t
{
    std::string_view name;
    ImGuiKey         key;
};

// Keys that aren't a single letter, digit or F-key
constexpr std::array<named_key_t, 10> named_keys = { {
    { .name = "Space", .key = ImGuiKey_Space },
    { .name = "PrintScreen", .key = ImGuiKey_PrintScreen },
    { .name = "Insert", .key = ImGuiKey_Insert },
    { .name = "Delete", .key = ImGuiKey_Delete },
    { .name = "Home", .key = ImGuiKey_Home },
    { .name = "End", .key = ImGuiKey_End },
    { .name = "PageUp", .key = ImGuiKey_PageUp },
    { .name = "PageDown", .key = ImGuiKey_PageDown },
    { .name = "Pause", .key = ImGuiKey_Pause },
    { .name = "Tab", .key = ImGuiKey_Tab },
} };

constexpr std::array<std::pair<std::string_view, Mod>, 11> mod_names = { {
    { "ctrl", Mod::Ctrl },
    { "control", Mod::Ctrl },
    { "shift", Mod::Shift },
    { "alt", Mod::Alt },
    { "option", Mod::Alt },
    { "opt", Mod::Alt },
    { "super", Mod::Super },
    { "win", Mod::Super },
    { "cmd", Mod::Super },
    { "command", Mod::Super },
    { "meta", Mod::Super },
} };

bool is_letter(ImGuiKey k)
{
    return k >= ImGuiKey_A && k <= ImGuiKey_Z;
}
bool is_digit(ImGuiKey k)
{
    return k >= ImGuiKey_0 && k <= ImGuiKey_9;
}
bool is_fkey(ImGuiKey k)
{
    return k >= ImGuiKey_F1 && k <= ImGuiKey_F12;
}

std::string key_name(ImGuiKey k)
{
    if (is_letter(k))
        return std::string(1, char('A' + (k - ImGuiKey_A)));  // not {}: that's the chars 1 and 'A'

    if (is_digit(k))
        return std::string(1, char('0' + (k - ImGuiKey_0)));

    if (is_fkey(k))
        return fmt::format("F{}", 1 + (k - ImGuiKey_F1));

    for (const named_key_t& nk : named_keys)
        if (nk.key == k)
            return std::string(nk.name);

    return {};
}

ImGuiKey key_from_name(const std::string& lower)
{
    if (lower.size() == 1)
    {
        const char c = lower[0];
        if (c >= 'a' && c <= 'z')
            return ImGuiKey(ImGuiKey_A + (c - 'a'));
        if (c >= '0' && c <= '9')
            return ImGuiKey(ImGuiKey_0 + (c - '0'));
    }

    if (lower.size() >= 2 && lower[0] == 'f')
    {
        int n = 0;
        for (size_t i = 1; i < lower.size(); ++i)
        {
            if (lower[i] < '0' || lower[i] > '9')
                return ImGuiKey_None;
            n = n * 10 + (lower[i] - '0');
        }
        if (n >= 1 && n <= 12)
            return ImGuiKey(ImGuiKey_F1 + (n - 1));
        return ImGuiKey_None;
    }

    for (const named_key_t& nk : named_keys)
        if (str_tolower(std::string(nk.name)) == lower)
            return nk.key;

    if (lower == "prtsc" || lower == "print")
        return ImGuiKey_PrintScreen;

    return ImGuiKey_None;
}

std::string           s_status;
std::function<void()> s_on_pressed;
[[maybe_unused]] bool s_registered = false;

#if OSHOT_HAS_GLOBAL_HOTKEY
/**
 * Fail a registration. The status, shown in Preferences and in the tray's notification,
 * gets the error in the user's language; the returned error stays English for the log
 * @param msgid The English format string, marked with N_() for scripts/update_translations.sh
 * @param args Arguments in the format
 **/
template <typename... Args>
Result<> fail(fmt::format_string<Args...> msgid, Args&&... args)
{
    s_status = fmt::format(fmt::runtime(i18n::tr(msgid.get().data())), args...);
    return Err(fmt::format(fmt::runtime(msgid.get()), args...));
}
#endif

#if OSHOT_WINDOWS
constexpr int HOTKEY_ID = 1;

UINT to_vk(ImGuiKey k)
{
    if (is_letter(k))
        return 'A' + (k - ImGuiKey_A);

    if (is_digit(k))
        return '0' + (k - ImGuiKey_0);

    if (is_fkey(k))
        return VK_F1 + (k - ImGuiKey_F1);

    switch (k)
    {
        case ImGuiKey_Space:       return VK_SPACE;
        case ImGuiKey_PrintScreen: return VK_SNAPSHOT;
        case ImGuiKey_Insert:      return VK_INSERT;
        case ImGuiKey_Delete:      return VK_DELETE;
        case ImGuiKey_Home:        return VK_HOME;
        case ImGuiKey_End:         return VK_END;
        case ImGuiKey_PageUp:      return VK_PRIOR;
        case ImGuiKey_PageDown:    return VK_NEXT;
        case ImGuiKey_Pause:       return VK_PAUSE;
        case ImGuiKey_Tab:         return VK_TAB;
        default:                   return 0;
    }
}
#elif OSHOT_MACOS
EventHotKeyRef  s_ref     = nullptr;
EventHandlerRef s_handler = nullptr;

int to_kvk(ImGuiKey k)
{
    static constexpr std::array<int, 26> letters = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I,
        kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
        kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z,
    };

    static constexpr std::array<int, 10> digits = {
        kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
        kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
    };

    static constexpr std::array<int, 12> fkeys = {
        kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12,
    };

    if (is_letter(k))
        return letters[k - ImGuiKey_A];

    if (is_digit(k))
        return digits[k - ImGuiKey_0];

    if (is_fkey(k))
        return fkeys[k - ImGuiKey_F1];

    switch (k)
    {
        case ImGuiKey_Space:    return kVK_Space;
        case ImGuiKey_Delete:   return kVK_ForwardDelete;
        case ImGuiKey_Home:     return kVK_Home;
        case ImGuiKey_End:      return kVK_End;
        case ImGuiKey_PageUp:   return kVK_PageUp;
        case ImGuiKey_PageDown: return kVK_PageDown;
        case ImGuiKey_Tab:      return kVK_Tab;
        default:                return -1;  // no PrintScreen/Insert/Pause on Mac keyboards
    }
}

OSStatus on_hotkey_event(EventHandlerCallRef, EventRef, void*)
{
    if (s_on_pressed)
        s_on_pressed();
    return noErr;
}
#endif
}  // namespace

Result<hotkey_t> parse(std::string_view str)
{
    hotkey_t    hk;
    std::string s = str_tolower(std::string(str));
    std::erase(s, ' ');
    if (s.empty() || s == "none")
        return Ok(hk);

    size_t start = 0;
    while (true)
    {
        const size_t      plus  = s.find('+', start);
        const std::string token = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (token.empty())
            return Err("Invalid hotkey '{}': empty key between '+'", str);

        const auto mod_it = std::ranges::find_if(mod_names, [&](const auto& m) { return m.first == token; });
        if (mod_it != mod_names.end())
        {
            hk.mods |= mod_it->second;
        }
        else
        {
            if (!hk.empty())
                return Err("Invalid hotkey '{}': only one non-modifier key is allowed", str);
            hk.key = key_from_name(token);
            if (hk.empty())
                return Err("Invalid hotkey '{}': unknown key '{}'", str, token);
        }

        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }

    if (hk.empty())
        return Err("Invalid hotkey '{}': missing a non-modifier key (e.g. 'S')", str);

    return Ok(hk);
}

std::string to_string(const hotkey_t& hk)
{
    if (hk.empty())
        return {};

    std::string out;
    if (hk.mods & Mod::Ctrl)
        out += "Ctrl+";
    if (hk.mods & Mod::Alt)
        out += OSHOT_MACOS ? "Option+" : "Alt+";
    if (hk.mods & Mod::Shift)
        out += "Shift+";
    if (hk.mods & Mod::Super)
        out += OSHOT_MACOS ? "Cmd+" : (OSHOT_WINDOWS ? "Win+" : "Super+");

    return out + key_name(hk.key);
}

bool is_supported_key(ImGuiKey key)
{
#if OSHOT_MACOS
    return to_kvk(key) >= 0;
#else
    return !key_name(key).empty();
#endif
}

const std::string& status()
{
    return s_status;
}

#if OSHOT_WINDOWS
Result<> enable(const hotkey_t& hk, std::function<void()> on_pressed)
{
    disable();
    s_on_pressed = std::move(on_pressed);
    if (hk.empty())
    {
        s_status = _("Disabled");
        return Ok();
    }

    const HWND hwnd = static_cast<HWND>(Tray::GetNativeHandle());
    if (!hwnd)
    {
        return fail(N_("Not registered: the tray isn't running"));
    }

    UINT mods = MOD_NOREPEAT;
    if (hk.mods & Mod::Ctrl)
        mods |= MOD_CONTROL;
    if (hk.mods & Mod::Shift)
        mods |= MOD_SHIFT;
    if (hk.mods & Mod::Alt)
        mods |= MOD_ALT;
    if (hk.mods & Mod::Super)
        mods |= MOD_WIN;

    // WM_HOTKEY lands in the tray window's message loop
    Tray::SetHotkeyCallback([](int id) {
        if (id == HOTKEY_ID && s_on_pressed)
            s_on_pressed();
    });

    if (!RegisterHotKey(hwnd, HOTKEY_ID, mods, to_vk(hk.key)))
    {
        const DWORD err = GetLastError();
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED)
            return fail(N_("{} is already used by Windows or another app"), to_string(hk));
        return fail(N_("Failed to register {} (error {})"), to_string(hk), err);
    }

    s_registered = true;
    s_status     = i18n::tr_fmt("Active: {}", to_string(hk));
    return Ok();
}

void disable()
{
    if (s_registered)
        UnregisterHotKey(static_cast<HWND>(Tray::GetNativeHandle()), HOTKEY_ID);
    s_registered = false;
}

#elif OSHOT_MACOS
Result<> enable(const hotkey_t& hk, std::function<void()> on_pressed)
{
    disable();
    s_on_pressed = std::move(on_pressed);
    if (hk.empty())
    {
        s_status = _("Disabled");
        return Ok();
    }

    const int kvk = to_kvk(hk.key);
    if (kvk < 0)
    {
        return fail(N_("{} isn't available on Mac keyboards"), key_name(hk.key));
    }

    if (!s_handler)
    {
        const EventTypeSpec spec = { kEventClassKeyboard, kEventHotKeyPressed };
        InstallApplicationEventHandler(&on_hotkey_event, 1, &spec, nullptr, &s_handler);
    }

    UInt32 mods = 0;
    if (hk.mods & Mod::Ctrl)
        mods |= controlKey;
    if (hk.mods & Mod::Shift)
        mods |= shiftKey;
    if (hk.mods & Mod::Alt)
        mods |= optionKey;
    if (hk.mods & Mod::Super)
        mods |= cmdKey;

    // Carbon hotkeys need no Accessibility permission, unlike NSEvent global monitors
    constexpr OSType    signature = ('o' << 24) | ('s' << 16) | ('h' << 8) | 't';
    const EventHotKeyID id        = { signature, 1 };
    const OSStatus      err       = RegisterEventHotKey(UInt32(kvk), mods, id, GetApplicationEventTarget(), 0, &s_ref);
    if (err != noErr)
    {
        s_ref = nullptr;
        if (err == eventHotKeyExistsErr)
            return fail(N_("{} is already used by macOS or another app"), to_string(hk));
        return fail(N_("Failed to register {} (error {})"), to_string(hk), int(err));
    }

    s_registered = true;
    s_status     = i18n::tr_fmt("Active: {}", to_string(hk));
    return Ok();
}

void disable()
{
    if (s_ref)
        UnregisterEventHotKey(s_ref);
    s_ref        = nullptr;
    s_registered = false;
}

#else
Result<> enable(const hotkey_t&, std::function<void()>)  // NOLINT(performance-unnecessary-value-param)
{
    return Err("Global hotkeys aren't supported on this platform, bind `oshot --gui` in your WM/DE instead");
}

void disable()
{}
#endif

}  // namespace hotkey
