#include "text_extraction.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "config.hpp"
#include "utf8.h"
#include "util.hpp"

// The test set: ocr/<name>.png with its text in ocr/<name>.txt, and
// barcode/<name>.png with one payload per line in barcode/<name>.txt
static const fs::path data_dir = OSHOT_TESTS_DATA_DIR;

// The images in a folder whose name starts with prefix, sorted
static std::vector<std::string> list_images(const std::string_view folder, const std::string_view prefix)
{
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(data_dir / folder))
    {
        const std::string& name = entry.path().stem().string();
        if (entry.path().extension() == ".png" && name.starts_with(prefix))
            names.push_back(name);
    }
    std::ranges::sort(names);
    return names;
}

static capture_result_t load_image(const fs::path& path)
{
    Result<capture_result_t> img = load_image_rgba(path.string());
    if (!img.ok())
        FAIL(img.error_v());
    return std::move(img.get());
}

// Python's \s, which the test set's scorer used
static bool is_space(const char32_t c)
{
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// The text as code points, so the edit distance counts characters, not bytes,
// with every run of whitespace as one space and none at the ends
static std::u32string normalize(const std::string& text)
{
    std::u32string out;
    bool           space = false;
    // utf8_int8_t is char8_t in C++20
    for (const auto* p = reinterpret_cast<const utf8_int8_t*>(text.c_str()); *p;)
    {
        utf8_int32_t cp = 0;
        p               = utf8codepoint(p, &cp);

        const auto c = static_cast<char32_t>(cp);
        if (is_space(c))
        {
            space = true;
            continue;
        }
        if (space && !out.empty())
            out.push_back(U' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

static size_t levenshtein(const std::u32string_view a, const std::u32string_view b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;

    for (size_t i = 1; i <= a.size(); ++i)
    {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1]) });
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// Character error rate: edits to turn got into want, over want's length, at most 1
static double cer(const std::u32string_view want, const std::u32string_view got)
{
    return std::min(1.0, double(levenshtein(want, got)) / double(std::max<size_t>(1, want.size())));
}

// Load a model into test_ocr(), or skip the test if it isn't installed.
// TESSDATA_PREFIX picks the models folder, else the one oshot defaults to
static void use_model(const std::string& model)
{
    const char*        env  = std::getenv("TESSDATA_PREFIX");
    const std::string& path = env && *env ? std::string(env) : g_config->File.ocr_path;

    if (!fs::exists(fs::path(path) / (model + ".traineddata")))
        SKIP("No " << model << ".traineddata in '" << path << "' (set TESSDATA_PREFIX)");

    const Result<>& res = test_ocr().Configure(path.c_str(), model.c_str());
    if (!res.ok())
        FAIL("Failed to load the OCR model '" << model << "': " << res.error_v());
}

// Read an image of the test set and check its character error rate, returning the text read
static std::u32string check_ocr(const std::string& name, const double max_cer)
{
    const capture_result_t& img  = load_image(data_dir / "ocr" / (name + ".png"));
    const std::u32string&   want = normalize(read_file(data_dir / "ocr" / (name + ".txt")));

    Result<ocr_result_t> res = test_ocr().ExtractTextCapture(img);
    if (!res.ok())
        FAIL_CHECK(name << ": " << res.error_v());
    const std::u32string& got = res.ok() ? normalize(res.get().data) : std::u32string();

    const double rate = cer(want, got);
    fmt::println(
        "  {:<28} CER {:5.1f}%  limit {:2.0f}%  {}", name, rate * 100, max_cer * 100, rate <= max_cer ? "ok" : "FAIL");
    INFO(name << ": CER " << rate * 100 << "%, read:\n" << (res.ok() ? res.get().data : ""));
    CHECK(rate <= max_cer);
    return got;
}

// Check every image whose name starts with prefix
static void check_ocr_group(const std::string_view prefix, const double max_cer)
{
    const std::vector<std::string>& names = list_images("ocr", prefix);
    REQUIRE_FALSE(names.empty());
    for (const std::string& name : names)
        check_ocr(name, max_cer);
}

// Decode an image of the test set and check it gives exactly its payloads.
// known_failure only changes what the progress line says
static void check_barcode(ZbarAPI& zbar, const std::string& name, const bool known_failure = false)
{
    const capture_result_t& img = load_image(data_dir / "barcode" / (name + ".png"));

    std::multiset<std::string> want;
    std::istringstream         lines(read_file(data_dir / "barcode" / (name + ".txt")));
    for (std::string line; std::getline(lines, line);)
    {
        // A checkout with core.autocrlf
        if (line.ends_with('\r'))
            line.pop_back();
        want.insert(line);
    }

    Result<zbar_result_t>           res   = zbar.ExtractTextsCapture(img);
    const std::vector<std::string>& datas = res.ok() ? res.get().datas : std::vector<std::string>();
    std::multiset<std::string>      got(datas.begin(), datas.end());

    fmt::println("  {:<28} {} of {} payloads  {}",
                 name,
                 got.size(),
                 want.size(),
                 got == want     ? "ok"
                 : known_failure ? "FAIL (known)"
                                 : "FAIL");

    // An empty result is a pass for the negative images, not an error
    INFO(name << (res.ok() ? "" : ": " + res.error_v()));
    CHECK(got == want);
}

static void check_barcode_group(const std::string_view prefix, const std::set<std::string_view>& skip = {})
{
    ZbarAPI                         zbar;
    const std::vector<std::string>& names = list_images("barcode", prefix);
    REQUIRE_FALSE(names.empty());
    for (const std::string& name : names)
        if (!skip.contains(name))
            check_barcode(zbar, name);
}

// ------------------------------
// OCR
// ------------------------------
// The limits leave room for a character or two over what Tesseract 5.5 with
// tessdata reads now: 3% for clean text, 5% for small, noisy or CJK text
TEST_CASE("OCR: black text on white documents", "[ocr][doc]")
{
    use_model("eng");
    check_ocr_group("doc_", 0.03);
}

TEST_CASE("OCR: light text on dark backgrounds", "[ocr][dark]")
{
    use_model("eng");
    check_ocr_group("dark_", 0.05);
}

TEST_CASE("OCR: text on flat saturated colors", "[ocr][flat_sat]")
{
    use_model("eng");
    check_ocr_group("flat_sat_", 0.03);
}

TEST_CASE("OCR: low contrast and gradients", "[ocr][lowcontrast][gradient]")
{
    use_model("eng");
    check_ocr_group("lowcontrast_", 0.03);
    check_ocr_group("gradient_", 0.03);
}

TEST_CASE("OCR: selections with a 1px margin", "[ocr][tight]")
{
    use_model("eng");
    check_ocr_group("tight_", 0.03);
}

TEST_CASE("OCR: columns and tables", "[ocr][layout]")
{
    use_model("eng");
    check_ocr_group("layout_", 0.03);
}

TEST_CASE("OCR: noise, JPEG, skew and fractional scaling", "[ocr][degraded]")
{
    use_model("eng");
    check_ocr_group("degraded_", 0.05);
}

TEST_CASE("OCR: memes and subtitles on busy backgrounds", "[ocr][stretch]")
{
    use_model("eng");
    check_ocr_group("stretch_", 0.15);
}

// The same text cropped with different margins must read the same
TEST_CASE("OCR: the selection's margins don't change the text", "[ocr][jitter]")
{
    use_model("eng");
    for (const std::string_view key : { "dark", "light", "sat_blue" })
    {
        const std::vector<std::string>& names = list_images("ocr", fmt::format("jitter_{}_", key));
        REQUIRE_FALSE(names.empty());

        std::set<std::u32string> outputs;
        for (const std::string& name : names)
            outputs.insert(check_ocr(name, 0.03));

        fmt::println(
            "  jitter_{}_*: {} different output(s)  {}", key, outputs.size(), outputs.size() == 1 ? "ok" : "FAIL");
        INFO("jitter_" << key << "_*: " << outputs.size() << " different outputs");
        CHECK(outputs.size() == 1);
    }
}

TEST_CASE("OCR: Italian accents", "[ocr][lang]")
{
    use_model("ita");
    check_ocr("lang_italian_accents", 0.03);
}

TEST_CASE("OCR: Japanese", "[ocr][lang]")
{
    use_model("jpn");
    check_ocr("lang_japanese", 0.05);
}

// ------------------------------
// Barcodes
// ------------------------------
TEST_CASE("Barcodes: QR codes", "[barcode][qr]")
{
    check_barcode_group("qr_");
}

TEST_CASE("Barcodes: 1D barcodes", "[barcode][bar]")
{
    check_barcode_group("bar_", { "bar_code39", "bar_ean13_tiny" });
}

TEST_CASE("Barcodes: several codes in one image", "[barcode][multi]")
{
    check_barcode_group("multi_");
}

TEST_CASE("Barcodes: images without codes decode nothing", "[barcode][neg]")
{
    check_barcode_group("neg_");
}

// bar_code39 gives "OSHOT123%" (the optional mod-43 check character, ZXing
// reads it the same), bar_ean13_tiny is an aliased render ZBar can't read at most scales
TEST_CASE("Barcodes: known failures", "[barcode][!mayfail]")
{
    ZbarAPI zbar;
    check_barcode(zbar, "bar_code39", true);
    check_barcode(zbar, "bar_ean13_tiny", true);
}

TEST_CASE("OCR: the prepared image maps back onto the selection", "[ocr][prepared]")
{
    SECTION("Scaled and padded")
    {
        const capture_result_t       src  = load_image(data_dir / "ocr" / "doc_sans_10px.png");
        const Result<ocr_prepared_t> prep = prepare_ocr_image(src);
        REQUIRE(prep.ok());
        const ocr_prepared_t& p = prep.get();

        // Small text is scaled up, and the edges get a white border
        CHECK(p.xform.scale > 1.0f);
        CHECK(p.xform.pad > 0);
        CHECK(p.xform.angle == 0.0f);
        CHECK(p.src_w == src.w);
        CHECK(p.src_h == src.h);
        REQUIRE(p.image.data.size() == size_t(p.image.w) * p.image.h * 4);

        // Grey and opaque
        bool grey = true;
        for (size_t i = 0; i < p.image.data.size(); i += 4)
            grey = grey && p.image.data[i] == p.image.data[i + 1] && p.image.data[i] == p.image.data[i + 2] &&
                   p.image.data[i + 3] == 255;
        CHECK(grey);

        // The area inside the border is the whole source
        const float      pad = float(p.xform.pad);
        const ocr_word_t all =
            map_prepared_box(p, pad, pad, float(p.image.w) - (2 * pad), float(p.image.h) - (2 * pad));
        CHECK(all.x == Catch::Approx(0).margin(1));
        CHECK(all.y == Catch::Approx(0).margin(1));
        CHECK(all.w == Catch::Approx(src.w).margin(1));
        CHECK(all.h == Catch::Approx(src.h).margin(1));

        // A box in the border is clamped to the source
        const ocr_word_t corner = map_prepared_box(p, 0, 0, pad / 2, pad / 2);
        CHECK(corner.x == 0);
        CHECK(corner.y == 0);
        CHECK(corner.w == 0);
        CHECK(corner.h == 0);
    }

    SECTION("Straightened")
    {
        const capture_result_t       src  = load_image(data_dir / "ocr" / "degraded_skew_2deg_scan.png");
        const Result<ocr_prepared_t> prep = prepare_ocr_image(src);
        REQUIRE(prep.ok());
        const ocr_prepared_t& p = prep.get();
        CHECK(p.xform.angle != 0.0f);

        // The middle stays in the middle
        const float      cx = float(p.image.w) / 2, cy = float(p.image.h) / 2;
        const ocr_word_t mid = map_prepared_box(p, cx - 1, cy - 1, 2, 2);
        CHECK(mid.x + (mid.w / 2) == Catch::Approx(float(src.w) / 2).margin(2));
        CHECK(mid.y + (mid.h / 2) == Catch::Approx(float(src.h) / 2).margin(2));
    }

    SECTION("An empty image")
    {
        CHECK_FALSE(prepare_ocr_image({}).ok());
    }
}
