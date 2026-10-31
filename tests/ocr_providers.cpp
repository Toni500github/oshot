#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "catch2/catch_amalgamated.hpp"
#include "plugin.hpp"

using namespace std::chrono_literals;

TEST_CASE("resolve_ocr_engine()", "[plugins][ocr-engines]")
{
    const std::vector<std::string> loaded = { ocr_engine_key("io.github.a.paddle", "paddle"),
                                              ocr_engine_key("io.github.b.cloud", "fast") };
    CHECK(loaded[0] == "io.github.a.paddle/paddle");

    SECTION("Tesseract for its own name, nothing, and values that name no plugin engine")
    {
        for (const char* engine : { "tesseract", "", "paddle", "/paddle", "io.github.a.paddle/" })
        {
            INFO(engine);
            CHECK(resolve_ocr_engine(engine, loaded) == OcrEngineKind::Tesseract);
        }
    }

    SECTION("A loaded plugin's engine")
    {
        CHECK(resolve_ocr_engine("io.github.b.cloud/fast", loaded) == OcrEngineKind::Plugin);
    }

    SECTION("Missing when its plugin isn't loaded or doesn't have it, so Tesseract reads instead")
    {
        CHECK(resolve_ocr_engine("io.github.b.cloud/slow", loaded) == OcrEngineKind::Missing);
        CHECK(resolve_ocr_engine("io.github.x.disabled/engine", loaded) == OcrEngineKind::Missing);
        CHECK(resolve_ocr_engine("io.github.a.paddle/paddle", {}) == OcrEngineKind::Missing);
    }

    SECTION("find_ocr_provider() finds nothing without loaded plugins")
    {
        CHECK(find_ocr_provider("io.github.a.paddle/paddle") == nullptr);
        CHECK(find_ocr_provider("tesseract") == nullptr);
    }
}

TEST_CASE("pick_ocr_language()", "[plugins][ocr-engines]")
{
    const std::vector<std::string> langs = { "ara", "eng", "ita" };
    CHECK(pick_ocr_language(langs, "ita", "eng") == "ita");
    CHECK(pick_ocr_language(langs, "", "eng") == "eng");
    CHECK(pick_ocr_language(langs, "deu", "jpn") == "ara");
    CHECK(pick_ocr_language({}, "eng", "eng").empty());
}

static oshot_err_t no_recognize(void*, const oshot_ocr_request_t*, oshot_ocr_output_t*, oshot_str_t*)
{
    return OSHOT_OK;
}

TEST_CASE("validate_ocr_provider()", "[plugins][ocr-engines]")
{
    static const char* const langs[] = { "eng", "deu" };
    oshot_ocr_provider_t     p{ .struct_size = sizeof(oshot_ocr_provider_t),
                                .id          = "cloud-1",
                                .name        = "Cloud",
                                .languages   = langs,
                                .n_languages = 2,
                                .flags       = 0,
                                .recognize   = no_recognize,
                                .user        = nullptr };
    CHECK(validate_ocr_provider(&p).ok());

    SECTION("Invalid ones")
    {
        CHECK_FALSE(validate_ocr_provider(nullptr).ok());

        oshot_ocr_provider_t bad = p;
        bad.struct_size          = 8;
        CHECK_FALSE(validate_ocr_provider(&bad).ok());

        for (const char* id : { "", "Cloud", "a/b", "with space" })
        {
            INFO(id);
            bad    = p;
            bad.id = id;
            CHECK_FALSE(validate_ocr_provider(&bad).ok());
        }
        bad    = p;
        bad.id = nullptr;
        CHECK_FALSE(validate_ocr_provider(&bad).ok());

        bad      = p;
        bad.name = "";
        CHECK_FALSE(validate_ocr_provider(&bad).ok());

        bad           = p;
        bad.recognize = nullptr;
        CHECK_FALSE(validate_ocr_provider(&bad).ok());

        bad           = p;
        bad.languages = nullptr;
        CHECK_FALSE(validate_ocr_provider(&bad).ok());

        static const char* const empty[] = { "eng", "" };
        bad                              = p;
        bad.languages                    = empty;
        CHECK_FALSE(validate_ocr_provider(&bad).ok());
    }

    SECTION("No languages is fine")
    {
        p.languages   = nullptr;
        p.n_languages = 0;
        CHECK(validate_ocr_provider(&p).ok());
    }
}

// What a plugin's recognize() fills in: an array from mem_alloc with texts from str_new
struct test_word_t
{
    const char* text;
    float       x, y, w, h;
    int32_t     confidence, line, block;
};

static oshot_ocr_output_t make_output(const std::vector<test_word_t>& words)
{
    oshot_ocr_output_t out{ .words = nullptr, .n_words = 0, .text = { nullptr, 0 }, .confidence = -1 };
    out.words   = static_cast<oshot_ocr_word_t*>(g_oshot_host.mem_alloc(words.size() * sizeof(oshot_ocr_word_t)));
    out.n_words = words.size();
    for (size_t i = 0; i < words.size(); ++i)
    {
        const test_word_t& w = words[i];
        out.words[i] = { .text = w.text ? g_oshot_host.str_new(w.text, std::strlen(w.text)) : oshot_str_t{ nullptr, 0 },
                         .x    = w.x,
                         .y    = w.y,
                         .w    = w.w,
                         .h    = w.h,
                         .confidence = w.confidence,
                         .line       = w.line,
                         .block      = w.block };
    }
    return out;
}

TEST_CASE("ocr_result_from_output()", "[plugins][ocr-engines]")
{
    SECTION("The text is joined from the words, and the confidence is their mean")
    {
        oshot_ocr_output_t out = make_output({ { "Hello", 0, 0, 30, 10, 90, 0, 0 },
                                               { "world", 35, 0, 30, 10, 80, 0, 0 },
                                               { "second", 0, 15, 40, 10, 70, 1, 0 },
                                               { "column", 100, 0, 40, 10, 60, 2, 1 } });
        bool               bad = true;
        const ocr_result_t res = ocr_result_from_output(out, 200, 100, nullptr, &bad);
        CHECK_FALSE(bad);
        CHECK(res.data == "Hello world\nsecond\n\ncolumn");
        CHECK(res.confidence == 75);
        CHECK(res.psm == -1);
        REQUIRE(res.words.size() == 4);
        CHECK(res.words[3].text == "column");
        CHECK(res.words[3].block == 1);

        // Freed and emptied
        CHECK(out.words == nullptr);
        CHECK(out.n_words == 0);
    }

    SECTION("A text and confidence of the engine's own are kept")
    {
        oshot_ocr_output_t out = make_output({ { "a", 0, 0, 5, 5, 10, 0, 0 } });
        out.text               = g_oshot_host.str_new("A, read better", 14);
        out.confidence         = 99;
        const ocr_result_t res = ocr_result_from_output(out, 10, 10, nullptr, nullptr);
        CHECK(res.data == "A, read better");
        CHECK(res.confidence == 99);
        CHECK(out.text.p == nullptr);
    }

    SECTION("Words without text or a valid box are skipped, and boxes clamped to the image")
    {
        const float        nan = std::numeric_limits<float>::quiet_NaN();
        oshot_ocr_output_t out = make_output({ { nullptr, 0, 0, 5, 5, 50, 0, 0 },
                                               { "nan", nan, 0, 5, 5, 50, 0, 0 },
                                               { "negative", 0, 0, -5, 5, 50, 0, 0 },
                                               { "edge", 90, -10, 30, 20, 150, -3, 0 } });
        bool               bad = false;
        const ocr_result_t res = ocr_result_from_output(out, 100, 50, nullptr, &bad);
        CHECK(bad);
        REQUIRE(res.words.size() == 1);
        const ocr_word_t& w = res.words[0];
        CHECK(w.text == "edge");
        CHECK(w.x == 90);
        CHECK(w.y == 0);
        CHECK(w.w == 10);
        CHECK(w.h == 10);
        CHECK(w.confidence == 100);
        CHECK(w.line == 0);
        CHECK(res.data == "edge");
    }

    SECTION("Nothing read")
    {
        oshot_ocr_output_t out{ .words = nullptr, .n_words = 0, .text = { nullptr, 0 }, .confidence = -1 };
        bool               bad = true;
        const ocr_result_t res = ocr_result_from_output(out, 10, 10, nullptr, &bad);
        CHECK_FALSE(bad);
        CHECK(res.data.empty());
        CHECK(res.words.empty());
        CHECK(res.confidence == 0);
    }
}

// What the test engines saw, and how they behave
struct engine_probe_t
{
    std::atomic<bool> started   = false;
    std::atomic<bool> cancelled = false;
    int               w = 0, h = 0;
    std::string       language;
};

static oshot_err_t engine_ok(void* user, const oshot_ocr_request_t* req, oshot_ocr_output_t* out, oshot_str_t*)
{
    auto* probe     = static_cast<engine_probe_t*>(user);
    probe->w        = req->image->w;
    probe->h        = req->image->h;
    probe->language = std::string(req->language.p, req->language.len);
    *out            = make_output({ { "read", 2, 3, 10, 4, 88, 0, 0 } });
    out->confidence = -1;
    return OSHOT_OK;
}

static oshot_err_t engine_until_cancelled(void* user, const oshot_ocr_request_t* req, oshot_ocr_output_t*, oshot_str_t*)
{
    auto* probe    = static_cast<engine_probe_t*>(user);
    probe->started = true;
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!req->cancelled(req) && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(1ms);
    probe->cancelled = req->cancelled(req);
    return OSHOT_ERR_FAILED;
}

static oshot_err_t engine_fails(void*, const oshot_ocr_request_t*, oshot_ocr_output_t* out, oshot_str_t* err)
{
    // What it allocated before failing is freed by oshot
    *out = make_output({ { "partial", 0, 0, 1, 1, 1, 0, 0 } });
    *err = g_oshot_host.str_new("quota exceeded", 14);
    return OSHOT_ERR_FAILED;
}

static bool wait_done(const OcrJob& job)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!job.Done() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(1ms);
    return job.Done();
}

static capture_result_t white_image(int w, int h)
{
    return { .data = std::vector<uint8_t>(size_t(w) * h * 4, 255), .w = w, .h = h };
}

TEST_CASE("OcrJob", "[plugins][ocr-engines]")
{
    engine_probe_t        probe;
    plugin_ocr_provider_t provider{
        .languages = { "eng" }, .id = "test", .name = "Test engine", .recognize = engine_ok, .user = &probe, .flags = 0
    };

    SECTION("The result, on the selection as it is")
    {
        OcrJob job("io.github.t.test", provider, white_image(40, 20), "eng");
        REQUIRE(wait_done(job));
        Result<ocr_result_t> res = job.TakeResult();
        REQUIRE(res.ok());
        CHECK(res.get().data == "read");
        CHECK(res.get().confidence == 88);
        CHECK(res.get().engine == "Test engine");
        CHECK(probe.w == 40);
        CHECK(probe.h == 20);
        CHECK(probe.language == "eng");
        CHECK_FALSE(job.TakeResult().ok());  // taken already
    }

    SECTION("Cancelling stops it and drops the result")
    {
        provider.recognize = engine_until_cancelled;
        OcrJob job("io.github.t.test", provider, white_image(8, 8), "");
        while (!probe.started)
            std::this_thread::sleep_for(1ms);
        CHECK_FALSE(job.Done());

        job.Cancel();
        REQUIRE(wait_done(job));
        CHECK(probe.cancelled);
        CHECK_FALSE(job.TakeResult().ok());
    }

    SECTION("Destroying a running job cancels it and waits")
    {
        provider.recognize = engine_until_cancelled;
        {
            OcrJob job("io.github.t.test", provider, white_image(8, 8), "");
            while (!probe.started)
                std::this_thread::sleep_for(1ms);
        }
        CHECK(probe.cancelled);
    }

    SECTION("A failure carries the engine's message")
    {
        provider.recognize = engine_fails;
        OcrJob job("io.github.t.test", provider, white_image(8, 8), "");
        REQUIRE(wait_done(job));
        Result<ocr_result_t> res = job.TakeResult();
        REQUIRE_FALSE(res.ok());
        CHECK(res.error_v() == "quota exceeded");
    }

    SECTION("OSHOT_OCR_PREPARED gets the prepared image, and the boxes come back in the selection's pixels")
    {
        // Dark text-like bars on white, so the image has something to scale for
        capture_result_t img = white_image(200, 40);
        for (int y = 15; y < 25; ++y)
            for (int x = 10; x < 190; ++x)
                if ((x / 4) % 2 == 0)
                    std::memset(&img.data[(size_t(y) * img.w + x) * 4], 0, 3);

        provider.flags = OSHOT_OCR_PREPARED;
        OcrJob job("io.github.t.test", provider, img, "");
        REQUIRE(wait_done(job));
        Result<ocr_result_t> res = job.TakeResult();
        REQUIRE(res.ok());
        CHECK(probe.w > img.w);
        CHECK(probe.h > img.h);
        REQUIRE(res.get().words.size() == 1);
        const ocr_word_t& w = res.get().words[0];
        CHECK(w.x >= 0);
        CHECK(w.y >= 0);
        CHECK(w.x + w.w <= float(img.w));
        CHECK(w.y + w.h <= float(img.h));
    }
}
