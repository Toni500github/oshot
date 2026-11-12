#include "util.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "screen_capture.hpp"
#include "util/color.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/strings.hpp"

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

TEST_CASE("ProcessLock", "[util][lock]")
{
    // Names of their own, so a running oshot's locks aren't touched
    constexpr std::string_view name = "oshot-tests-lock";
    {
        const ProcessLock first(name);
        REQUIRE(first.held());
#if !OSHOT_WINDOWS
        CHECK(fs::exists(test_temp_dir() / "xdg-runtime" / "oshot-tests-lock.lock"));
#endif

        // Taken, here just like from another process
        const ProcessLock second(name);
        CHECK(second.state() == ProcessLock::State::Busy);
        CHECK_FALSE(second.held());

        CHECK(ProcessLock("oshot-tests-other").held());
    }

    // Destroying the holder released it
    CHECK(ProcessLock(name).held());
}

TEST_CASE("encode_to_image() PNG round trip", "[util][png]")
{
    // Gradients, hard edges, noise and partial alpha, so every row filter and the
    // compressor see something to work with
    capture_result_t img;
    img.w = 301;
    img.h = 157;
    img.data.resize(size_t(img.w) * img.h * 4);
    uint32_t noise = 12345;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
        {
            noise          = noise * 1664525 + 1013904223;
            uint8_t*   px  = &img.data[(size_t(y) * img.w + x) * 4];
            const bool box = x > 100 && x < 200 && y > 40 && y < 120;
            px[0]          = uint8_t(x);
            px[1]          = box ? 0 : uint8_t(y * 3);
            px[2]          = uint8_t(noise >> 24);
            px[3]          = y < 20 ? uint8_t(x) : 255;
        }

    const std::vector<uint8_t> png = encode_to_image(img, ImageExt::PNG);
    REQUIRE(png.size() > 8);
    CHECK(std::string_view(reinterpret_cast<const char*>(png.data()), 8) == "\x89PNG\r\n\x1a\n");

    const auto path = test_temp_dir() / "roundtrip.png";
    write_file(path, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
    const Result<capture_result_t> res = load_image_rgba(path.string());
    REQUIRE(res.ok());
    const capture_result_t& back = res.get();
    CHECK(back.w == img.w);
    CHECK(back.h == img.h);
    CHECK(back.data == img.data);
}
