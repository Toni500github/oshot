#include "util.hpp"

#include <string>
#include <vector>

#include "catch2/catch_amalgamated.hpp"

using strings = std::vector<std::string>;

TEST_CASE("split()", "[util]")
{
    CHECK(split("a,b,c", ',') == strings{ "a", "b", "c" });
    CHECK(split("abc", ',') == strings{ "abc" });
    CHECK(split("", ',').empty());

    // Empty fields between and before delimiters are kept, a trailing delimiter adds none
    CHECK(split("a,,b", ',') == strings{ "a", "", "b" });
    CHECK(split(",a", ',') == strings{ "", "a" });
    CHECK(split("a,b,", ',') == strings{ "a", "b" });
}

TEST_CASE("parse_hex_rgba()", "[util][color]")
{
    rgba_t c;

    SECTION("#RRGGBB is opaque")
    {
        REQUIRE(parse_hex_rgba("#ff8000", c));
        CHECK((c.r == 0xff && c.g == 0x80 && c.b == 0x00 && c.a == 0xff));

        REQUIRE(parse_hex_rgba("#FFAA00", c));
        CHECK((c.r == 0xff && c.g == 0xaa && c.b == 0x00 && c.a == 0xff));
    }

    SECTION("#RRGGBBAA")
    {
        REQUIRE(parse_hex_rgba("#11223344", c));
        CHECK((c.r == 0x11 && c.g == 0x22 && c.b == 0x33 && c.a == 0x44));

        REQUIRE(parse_hex_rgba("#00000000", c));
        CHECK((c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0));
    }

    SECTION("Invalid colors are rejected and leave out untouched")
    {
        const rgba_t before(0x01020304);
        for (const char* hex : { "",
                                 "#",
                                 "ff8000",
                                 "#ff800",
                                 "#ff80000",
                                 "#ff8000000",
                                 "#gg0000",
                                 "#ff800g",
                                 // strtoul() alone would accept these (a sign, spaces or a 0x prefix)
                                 "#0x1234",
                                 "#+12345",
                                 "#-12345",
                                 "# 12345",
                                 "#0x123456",
                                 "#-0000000" })
        {
            c = before;
            INFO("'" << hex << "'");
            CHECK_FALSE(parse_hex_rgba(hex, c));
            CHECK((c.r == before.r && c.g == before.g && c.b == before.b && c.a == before.a));
        }
    }

    SECTION("col_to_hexstr() writes what parse_hex_rgba() reads")
    {
        for (const char* hex : { "#00000000", "#ffffffff", "#12ab34cd", "#80808080" })
        {
            REQUIRE(parse_hex_rgba(hex, c));
            CHECK(col_to_hexstr(c) == hex);
        }
    }
}
