#include "ui.hpp"

#include "catch2/catch_amalgamated.hpp"
#include "imgui/imgui.h"

TEST_CASE("ui::PassFilter()", "[ui]")
{
    SECTION("An empty filter passes every record")
    {
        const ImGuiTextFilter filter;
        CHECK(ui::PassFilter(filter, { "anything", "" }));
        CHECK(ui::PassFilter(filter, {}));
    }

    SECTION("A word matches in any field, ignoring case")
    {
        const ImGuiTextFilter filter("qr");
        CHECK(ui::PassFilter(filter, { "https://example.com", "QR-Code" }));
        CHECK(ui::PassFilter(filter, { "qrcode", "EAN-13" }));
        CHECK_FALSE(ui::PassFilter(filter, { "4006381333931", "EAN-13" }));
    }

    SECTION("Comma-separated words match either")
    {
        const ImGuiTextFilter filter("ean,code-128");
        CHECK(ui::PassFilter(filter, { "4006381333931", "EAN-13" }));
        CHECK(ui::PassFilter(filter, { "abc", "CODE-128" }));
        CHECK_FALSE(ui::PassFilter(filter, { "https://example.com", "QR-Code" }));
    }

    SECTION("An excluded word in any field excludes the whole record")
    {
        const ImGuiTextFilter filter("-qr");
        CHECK_FALSE(ui::PassFilter(filter, { "https://example.com", "QR-Code" }));
        CHECK(ui::PassFilter(filter, { "4006381333931", "EAN-13" }));

        const ImGuiTextFilter both("example,-qr");
        CHECK_FALSE(ui::PassFilter(both, { "https://example.com", "QR-Code" }));
        CHECK(ui::PassFilter(both, { "https://example.com", "Data Matrix" }));
    }

    SECTION("A word doesn't match across two fields")
    {
        const ImGuiTextFilter filter("bc");
        CHECK_FALSE(ui::PassFilter(filter, { "ab", "cd" }));
        CHECK(ui::PassFilter(filter, { "abcd", "" }));
    }
}
