// The SDK's mock host (oshot/oshot_mock_host.h), driving the example plugins.
// tests/CMakeLists.txt compiles each example into this program with its
// oshot_plugin_entry() renamed, so they can all be linked together.

#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "oshot/oshot_mock_host.h"
#include "util/paths.hpp"

extern "C" {
const oshot_plugin_t* oshot_test_uppercase_entry(void);
const oshot_plugin_t* oshot_test_join_lines_entry(void);
const oshot_plugin_t* oshot_test_extract_links_entry(void);
const oshot_plugin_t* oshot_test_export_png_entry(void);
int                   mock_host_c_test(void);
}

namespace fs = std::filesystem;

namespace
{
// Frees the mock at the end of a test, even when a REQUIRE fails
struct mock_t
{
    oshot_mock_t* m = oshot_mock_new();
    ~mock_t() { oshot_mock_free(m); }
    operator oshot_mock_t*() const { return m; }
};

std::string text(oshot_mock_t* m, const char* buffer)
{
    return oshot_mock_text(m, buffer);
}
}  // namespace

TEST_CASE("Mock host: the uppercase example", "[plugins][mock]")
{
    const size_t live = oshot_mock_live_allocations();
    {
        mock_t                     m;
        const std::vector<uint8_t> pixels(4 * 3 * 4, 0x80);
        oshot_mock_set_capture(m, 4, 3, pixels.data());
        REQUIRE(oshot_mock_load(m, oshot_test_uppercase_entry()));
        oshot_mock_capture_start(m);

        SECTION("The button uppercases the OCR text and shows the selection's size")
        {
            oshot_mock_ocr(m, "hello world", nullptr, 0);
            CHECK(text(m, OSHOT_TEXT_OCR) == "hello world");

            oshot_mock_render(m);
            CHECK(oshot_mock_drew(m, "button Uppercase the OCR output"));
            CHECK_FALSE(oshot_mock_drew(m, "Selection:"));

            oshot_mock_click(m, "Uppercase the OCR output");
            oshot_mock_render(m);
            CHECK(text(m, OSHOT_TEXT_OCR) == "HELLO WORLD");
            oshot_mock_render(m);
            CHECK(oshot_mock_drew(m, "text Selection: 4x3"));
        }

        SECTION("The saved settings uppercase new text with the prefix")
        {
            oshot_mock_set_bool(m, OSHOT_STORE_CONFIG, "auto-uppercase", true);
            oshot_mock_set_string(m, OSHOT_STORE_CONFIG, "prefix", "> ");
            oshot_mock_save_preferences(m);
            CHECK(std::string(oshot_mock_ocr(m, "abc", nullptr, 0)) == "> ABC");

            // Nothing read, so on_ocr_done gets empty text and leaves it alone
            CHECK(std::string(oshot_mock_ocr(m, "", nullptr, 0)).empty());
        }

        CHECK(oshot_mock_log_count(m, OSHOT_LOG_WARN) == 0);
    }
    CHECK(oshot_mock_live_allocations() == live);
}

TEST_CASE("Mock host: the text transformer examples", "[plugins][mock]")
{
    const size_t live = oshot_mock_live_allocations();
    {
        mock_t join;
        REQUIRE(oshot_mock_load(join, oshot_test_join_lines_entry()));
        CHECK(std::string(oshot_mock_transform(join, OSHOT_TEXT_OCR, "exam-\nple text\nnext line\n\nnew para")) ==
              "example text next line\n\nnew para");
        // Barcodes pass through
        CHECK(std::string(oshot_mock_transform(join, OSHOT_TEXT_BARCODE, "a\nb")) == "a\nb");

        oshot_mock_set_bool(join, OSHOT_STORE_CONFIG, "join-paragraphs", false);
        oshot_mock_save_preferences(join);
        CHECK(std::string(oshot_mock_transform(join, OSHOT_TEXT_OCR, "exam-\nple text\nnext")) == "example text\nnext");

        mock_t links;
        REQUIRE(oshot_mock_load(links, oshot_test_extract_links_entry()));
        oshot_mock_capture_start(links);
        CHECK(std::string(oshot_mock_ocr(links, "see https://example.com/a and mail me@example.org", nullptr, 0)) ==
              "https://example.com/a\nme@example.org");
        CHECK(std::string(oshot_mock_ocr(links, "no links here", nullptr, 0)) == "no links here");

        const char*           data   = "https://example.com/qr";
        const oshot_barcode_t code[] = { { oshot_str("QR-Code"), oshot_str(data), 1, 2, 30, 30 } };
        CHECK(std::string(oshot_mock_barcodes(links, code, 1)) == "https://example.com/qr");
    }
    CHECK(oshot_mock_live_allocations() == live);
}

TEST_CASE("Mock host: the export-png example", "[plugins][mock]")
{
    const fs::path dir = test_temp_dir() / "mock-export-png";
    fs::create_directories(dir);

    const size_t live = oshot_mock_live_allocations();
    {
        mock_t m;
        oshot_mock_set_data_dir(m, dir.string().c_str());
        const std::vector<uint8_t> pixels(2 * 2 * 4, 0xff);
        oshot_mock_set_capture(m, 2, 2, pixels.data());
        REQUIRE(oshot_mock_load(m, oshot_test_export_png_entry()));
        CHECK(oshot_mock_action_count(m, OSHOT_PLACE_TOOLBAR) == 1);
        CHECK(oshot_mock_action_count(m, OSHOT_PLACE_TEXT_TOOLS) == 1);

        SECTION("No overlay: request_final_image() fails and the plugin says so")
        {
            REQUIRE(oshot_mock_run_action(m, "Export as PNG"));
            bool ok = true;
            CHECK(oshot_mock_result(m, &ok, nullptr) != nullptr);
            CHECK_FALSE(ok);
        }

        SECTION("The image arrives on the next frame and a thread writes it")
        {
            oshot_mock_capture_start(m);
            REQUIRE(oshot_mock_run_action(m, "Export as PNG"));
            CHECK(oshot_mock_result(m, nullptr, nullptr) == nullptr);
            oshot_mock_run_ui_tasks(m);
            oshot_mock_unload(m);  // destroy() joins the thread

            bool        ok       = false;
            const char* copyable = nullptr;
            REQUIRE(oshot_mock_result(m, &ok, &copyable) != nullptr);
            CHECK(ok);
            CHECK(fs::exists(fs::path(copyable)));
            CHECK(fs::path(copyable).parent_path() == dir / "exports");
        }

        SECTION("The overlay closes first: the image never comes")
        {
            oshot_mock_capture_start(m);
            REQUIRE(oshot_mock_run_action(m, "Export as PNG"));
            oshot_mock_capture_end(m);
            bool ok = true;
            CHECK(oshot_mock_result(m, &ok, nullptr) != nullptr);
            CHECK_FALSE(ok);
        }
    }
    CHECK(oshot_mock_live_allocations() == live);
}

TEST_CASE("Mock host: what oshot refuses", "[plugins][mock]")
{
    SECTION("An older oshot without the widgets: the plugin refuses to load")
    {
        mock_t m;
        oshot_mock_set_host_size(m, offsetof(oshot_host_t, ui_text));
        CHECK_FALSE(oshot_mock_load(m, oshot_test_uppercase_entry()));
    }

    SECTION("A bad descriptor isn't loaded")
    {
        oshot_plugin_t p = *oshot_test_uppercase_entry();
        p.init           = nullptr;
        p.destroy        = nullptr;

        mock_t bad_id;
        p.id = "Not.Valid";
        CHECK_FALSE(oshot_mock_load(bad_id, &p));

        mock_t bad_abi;
        p.id          = "io.github.test.ok";
        p.abi_version = OSHOT_API_VERSION + 1;
        CHECK_FALSE(oshot_mock_load(bad_abi, &p));
    }

    SECTION("UI-thread functions from another thread")
    {
        mock_t m;
        REQUIRE(oshot_mock_load(m, oshot_test_uppercase_entry()));
        const oshot_host_t* host = oshot_mock_host(m);
        oshot_ctx_t*        ctx  = oshot_mock_ctx(m);

        oshot_err_t r = OSHOT_OK;
        std::thread t([&] { r = host->copy_text(ctx, oshot_str("x"), nullptr); });
        t.join();
        CHECK(r == OSHOT_ERR_THREAD);
        CHECK(oshot_mock_log_count(m, OSHOT_LOG_ERROR) == 1);
        CHECK(host->copy_text(ctx, oshot_str("x"), nullptr) == OSHOT_OK);
    }
}

TEST_CASE("Mock host: from C", "[plugins][mock]")
{
    CHECK(mock_host_c_test() == 0);
}
