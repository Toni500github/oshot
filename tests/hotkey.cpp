#include "hotkey.hpp"

#include <string>

#include "catch2/catch_amalgamated.hpp"

using namespace hotkey;

static hotkey_t parsed(const std::string& str)
{
    Result<hotkey_t> hk = parse(str);
    if (!hk.ok())
        FAIL("parse(\"" << str << "\"): " << hk.error_v());
    return hk.get();
}

TEST_CASE("hotkey::parse()", "[hotkey]")
{
    SECTION("Modifiers and a key")
    {
        CHECK(parsed("Ctrl+Shift+S") == hotkey_t{ .mods = Ctrl | Shift, .key = ImGuiKey_S });
        CHECK(parsed("Alt+2") == hotkey_t{ .mods = Alt, .key = ImGuiKey_2 });
        CHECK(parsed("Super+F12") == hotkey_t{ .mods = Super, .key = ImGuiKey_F12 });
        CHECK(parsed("Q") == hotkey_t{ .mods = kNone, .key = ImGuiKey_Q });
    }

    SECTION("Case and spaces don't matter, and the order of the modifiers neither")
    {
        const hotkey_t want{ .mods = Ctrl | Shift, .key = ImGuiKey_S };
        CHECK(parsed("ctrl+shift+s") == want);
        CHECK(parsed(" CTRL + Shift + s ") == want);
        CHECK(parsed("Shift+Ctrl+S") == want);
    }

    SECTION("Modifier aliases")
    {
        for (const char* ctrl : { "Ctrl", "Control" })
            CHECK(parsed(std::string(ctrl) + "+A").mods == Ctrl);
        for (const char* alt : { "Alt", "Option", "Opt" })
            CHECK(parsed(std::string(alt) + "+A").mods == Alt);
        for (const char* super : { "Super", "Win", "Cmd", "Command", "Meta" })
            CHECK(parsed(std::string(super) + "+A").mods == Super);
    }

    SECTION("Named keys")
    {
        CHECK(parsed("Alt+PrintScreen").key == ImGuiKey_PrintScreen);
        CHECK(parsed("Alt+PrtSc").key == ImGuiKey_PrintScreen);
        CHECK(parsed("Alt+Print").key == ImGuiKey_PrintScreen);
        CHECK(parsed("Ctrl+Space").key == ImGuiKey_Space);
        CHECK(parsed("Ctrl+PageUp").key == ImGuiKey_PageUp);
        CHECK(parsed("Ctrl+F1").key == ImGuiKey_F1);
    }

    SECTION("Empty and 'none' disable the hotkey")
    {
        for (const char* off : { "", "   ", "none", "NONE" })
        {
            INFO("'" << off << "'");
            CHECK(parsed(off).empty());
        }
    }

    SECTION("Invalid hotkeys")
    {
        for (const char* bad :
             { "Ctrl+", "+S", "Ctrl++S", "Ctrl+Shift", "Ctrl+S+D", "Ctrl+Foo", "F0", "F13", "Ctrl+F1x", "Ctrl+ß" })
        {
            INFO("'" << bad << "'");
            CHECK_FALSE(parse(bad).ok());
        }
    }
}

TEST_CASE("hotkey::to_string()", "[hotkey]")
{
    CHECK(to_string({}).empty());
    CHECK(to_string(parsed("shift+ctrl+s")) == "Ctrl+Shift+S");
    CHECK(to_string(parsed("F5")) == "F5");
    CHECK(to_string(parsed("ctrl+pagedown")) == "Ctrl+PageDown");

    // Every key and set of modifiers comes back from parse() as it was
    std::vector<ImGuiKey> keys = { ImGuiKey_Space, ImGuiKey_PrintScreen, ImGuiKey_Insert, ImGuiKey_Delete,
                                   ImGuiKey_Home,  ImGuiKey_End,         ImGuiKey_PageUp, ImGuiKey_PageDown,
                                   ImGuiKey_Pause, ImGuiKey_Tab };
    for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
        keys.push_back(ImGuiKey(k));
    for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k)
        keys.push_back(ImGuiKey(k));
    for (int k = ImGuiKey_F1; k <= ImGuiKey_F12; ++k)
        keys.push_back(ImGuiKey(k));

    for (const ImGuiKey key : keys)
    {
        for (std::uint8_t mods = 0; mods < 16; ++mods)
        {
            const hotkey_t     hk{ .mods = mods, .key = key };
            const std::string& str = to_string(hk);
            INFO("'" << str << "'");
            REQUIRE(parse(str).ok());
            CHECK(parse(str).get() == hk);
        }
    }
}
