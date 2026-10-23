#include "screen_capture.hpp"

#include <cstdint>
#include <deque>

#include "catch2/catch_amalgamated.hpp"

// An image whose every pixel says where it is: R, G = x, y and B, A = x / 256, y / 256
static capture_result_t make_image(const int w, const int h)
{
    capture_result_t img{ .data = std::vector<uint8_t>(size_t(w) * h * 4), .w = w, .h = h };
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            uint8_t* p = img.data.data() + (size_t(y) * w + x) * 4;
            p[0]       = uint8_t(x);
            p[1]       = uint8_t(y);
            p[2]       = uint8_t(x >> 8);
            p[3]       = uint8_t(y >> 8);
        }
    }
    return img;
}

struct point_t
{
    int x, y;

    bool operator==(const point_t&) const = default;
};

// Where the pixel at x, y of a crop came from in make_image()'s image
static point_t source_of(const capture_result_t& img, const int x, const int y)
{
    const uint8_t* p = img.data.data() + (size_t(y) * img.w + x) * 4;
    return { .x = p[0] | (p[2] << 8), .y = p[1] | (p[3] << 8) };
}

TEST_CASE("parse_region()", "[capture][region]")
{
    const region_t want{ .x = 10, .y = 20, .w = 100, .h = 50 };

    SECTION("slop's WxH+X+Y and slurp's \"X,Y WxH\"")
    {
        REQUIRE(parse_region("100x50+10+20").ok());
        CHECK(parse_region("100x50+10+20").get() == want);
        REQUIRE(parse_region("10,20 100x50").ok());
        CHECK(parse_region("10,20 100x50").get() == want);
        REQUIRE(parse_region("1x1+0+0").ok());
        CHECK(parse_region("1x1+0+0").get() == region_t{ .x = 0, .y = 0, .w = 1, .h = 1 });
    }

    SECTION("Invalid regions")
    {
        for (const char* bad : { "",
                                 "abc",
                                 "100x50",
                                 "100x50+10",
                                 "100x50+10+20junk",
                                 "100x50+10+20 ",
                                 "10,20 100x50 extra",
                                 "0x50+1+1",
                                 "100x0+1+1",
                                 "-5x50+1+1",
                                 "100x50+-1+0",
                                 "100x50+0+-1",
                                 "-1,0 10x10" })
        {
            INFO("'" << bad << "'");
            CHECK_FALSE(parse_region(bad).ok());
        }
    }
}

TEST_CASE("crop_image()", "[capture][crop]")
{
    const capture_result_t img = make_image(40, 30);

    SECTION("A region inside the image")
    {
        Result<capture_result_t> crop = crop_image(img, { .x = 5, .y = 7, .w = 10, .h = 4 });
        REQUIRE(crop.ok());
        CHECK(crop.get().w == 10);
        CHECK(crop.get().h == 4);
        CHECK(crop.get().data.size() == size_t(10) * 4 * 4);
        CHECK(source_of(crop.get(), 0, 0) == point_t{ 5, 7 });
        CHECK(source_of(crop.get(), 9, 3) == point_t{ 14, 10 });
    }

    SECTION("The whole image")
    {
        Result<capture_result_t> crop = crop_image(img, { .x = 0, .y = 0, .w = 40, .h = 30 });
        REQUIRE(crop.ok());
        CHECK(crop.get().data == img.data);
    }

    SECTION("Regions not fully inside the image")
    {
        for (const region_t& bad : { region_t{ .x = 35, .y = 0, .w = 10, .h = 10 },
                                     region_t{ .x = 0, .y = 25, .w = 10, .h = 10 },
                                     region_t{ .x = -1, .y = 0, .w = 10, .h = 10 },
                                     region_t{ .x = 0, .y = -1, .w = 10, .h = 10 },
                                     region_t{ .x = 0, .y = 0, .w = 0, .h = 10 },
                                     region_t{ .x = 0, .y = 0, .w = 10, .h = 0 },
                                     region_t{ .x = 0, .y = 0, .w = 41, .h = 30 } })
        {
            INFO(bad.w << "x" << bad.h << "+" << bad.x << "+" << bad.y);
            CHECK_FALSE(crop_image(img, bad).ok());
        }
    }
}

TEST_CASE("crop_to_monitor()", "[capture][crop]")
{
    SECTION("Two monitors side by side, at 1x")
    {
        const std::deque<region_t> monitors = { { .x = 0, .y = 0, .w = 1920, .h = 1080 },
                                                { .x = 1920, .y = 0, .w = 1280, .h = 1024 } };
        const capture_result_t     full     = make_image(3200, 1080);

        Result<capture_result_t> left = crop_to_monitor(full, monitors, monitors[0]);
        REQUIRE(left.ok());
        CHECK(left.get().w == 1920);
        CHECK(left.get().h == 1080);
        CHECK(source_of(left.get(), 0, 0) == point_t{ 0, 0 });

        Result<capture_result_t> right = crop_to_monitor(full, monitors, monitors[1]);
        REQUIRE(right.ok());
        CHECK(right.get().w == 1280);
        CHECK(right.get().h == 1024);
        CHECK(source_of(right.get(), 0, 0) == point_t{ 1920, 0 });
        CHECK(source_of(right.get(), 1279, 1023) == point_t{ 3199, 1023 });
    }

    SECTION("A capture at 2x the layout's size (HiDPI)")
    {
        const std::deque<region_t> monitors = { { .x = 0, .y = 0, .w = 1600, .h = 900 },
                                                { .x = 1600, .y = 0, .w = 1600, .h = 900 } };
        const capture_result_t     full     = make_image(6400, 1800);

        Result<capture_result_t> right = crop_to_monitor(full, monitors, monitors[1]);
        REQUIRE(right.ok());
        CHECK(right.get().w == 3200);
        CHECK(right.get().h == 1800);
        CHECK(source_of(right.get(), 0, 0) == point_t{ 3200, 0 });
    }

    SECTION("A 1.5x fractional scale")
    {
        const std::deque<region_t> monitors = { { .x = 0, .y = 0, .w = 1280, .h = 720 },
                                                { .x = 1280, .y = 0, .w = 1280, .h = 720 } };
        const capture_result_t     full     = make_image(3840, 1080);

        Result<capture_result_t> right = crop_to_monitor(full, monitors, monitors[1]);
        REQUIRE(right.ok());
        CHECK(right.get().w == 1920);
        CHECK(right.get().h == 1080);
        CHECK(source_of(right.get(), 0, 0) == point_t{ 1920, 0 });
    }

    SECTION("A monitor left of and above the origin")
    {
        const std::deque<region_t> monitors = { { .x = -1920, .y = -200, .w = 1920, .h = 1080 },
                                                { .x = 0, .y = 0, .w = 1920, .h = 1080 } };
        const capture_result_t     full     = make_image(3840, 1280);

        Result<capture_result_t> primary = crop_to_monitor(full, monitors, monitors[1]);
        REQUIRE(primary.ok());
        CHECK(primary.get().w == 1920);
        CHECK(primary.get().h == 1080);
        CHECK(source_of(primary.get(), 0, 0) == point_t{ 1920, 200 });

        Result<capture_result_t> left = crop_to_monitor(full, monitors, monitors[0]);
        REQUIRE(left.ok());
        CHECK(source_of(left.get(), 0, 0) == point_t{ 0, 0 });
    }

    SECTION("Monitors stacked vertically, with an offset")
    {
        const std::deque<region_t> monitors = { { .x = 0, .y = 0, .w = 2560, .h = 1440 },
                                                { .x = 320, .y = 1440, .w = 1920, .h = 1080 } };
        const capture_result_t     full     = make_image(2560, 2520);

        Result<capture_result_t> bottom = crop_to_monitor(full, monitors, monitors[1]);
        REQUIRE(bottom.ok());
        CHECK(bottom.get().w == 1920);
        CHECK(bottom.get().h == 1080);
        CHECK(source_of(bottom.get(), 0, 0) == point_t{ 320, 1440 });
        CHECK(source_of(bottom.get(), 1919, 1079) == point_t{ 2239, 2519 });
    }

    SECTION("A target partly outside the layout is clamped to the capture")
    {
        const std::deque<region_t> monitors = { { .x = 0, .y = 0, .w = 100, .h = 100 } };
        const capture_result_t     full     = make_image(100, 100);

        Result<capture_result_t> crop = crop_to_monitor(full, monitors, { .x = 50, .y = 50, .w = 100, .h = 100 });
        REQUIRE(crop.ok());
        CHECK(crop.get().w == 50);
        CHECK(crop.get().h == 50);
        CHECK(source_of(crop.get(), 49, 49) == point_t{ 99, 99 });
    }

    SECTION("Errors")
    {
        const capture_result_t full = make_image(100, 100);
        CHECK_FALSE(crop_to_monitor(full, {}, { .x = 0, .y = 0, .w = 100, .h = 100 }).ok());
        CHECK_FALSE(crop_to_monitor(full, { { .x = 0, .y = 0, .w = 0, .h = 0 } }, {}).ok());

        const std::deque<region_t> monitors = { { .x = 0, .y = 0, .w = 100, .h = 100 } };
        CHECK_FALSE(crop_to_monitor(full, monitors, { .x = 200, .y = 0, .w = 100, .h = 100 }).ok());
    }
}

TEST_CASE("rotate_rgba()", "[capture][rotate]")
{
    const capture_result_t img = make_image(3, 2);

    SECTION("90 degrees clockwise")
    {
        const capture_result_t r = rotate_rgba(img, 1);
        REQUIRE(r.w == 2);
        REQUIRE(r.h == 3);
        // The bottom left corner goes to the top left
        CHECK(source_of(r, 0, 0) == point_t{ 0, 1 });
        CHECK(source_of(r, 1, 0) == point_t{ 0, 0 });
        CHECK(source_of(r, 1, 2) == point_t{ 2, 0 });
    }

    SECTION("180 and 270 degrees")
    {
        const capture_result_t r180 = rotate_rgba(img, 2);
        REQUIRE(r180.w == 3);
        CHECK(source_of(r180, 0, 0) == point_t{ 2, 1 });

        const capture_result_t r270 = rotate_rgba(img, 3);
        REQUIRE(r270.w == 2);
        CHECK(source_of(r270, 0, 0) == point_t{ 2, 0 });
    }

    SECTION("Turns wrap around")
    {
        CHECK(rotate_rgba(img, 0).data == img.data);
        CHECK(rotate_rgba(img, 4).data == img.data);
        CHECK(rotate_rgba(img, -1).data == rotate_rgba(img, 3).data);
        CHECK(rotate_rgba(rotate_rgba(img, 1), 3).data == img.data);
        CHECK(rotate_rgba(rotate_rgba(img, 2), 2).data == img.data);
    }
}
