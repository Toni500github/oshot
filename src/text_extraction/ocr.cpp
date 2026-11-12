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

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/publictypes.h>
#include <tesseract/resultiterator.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <numeric>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "config.hpp"
#include "fmt/format.h"
#include "ocr_image.hpp"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "text_extraction.hpp"
#include "util/result.hpp"
#include "util/strings.hpp"

#if defined(__GLIBC__)
#  include <malloc.h>
#endif
#ifdef _OPENMP
#  include <omp.h>
#endif

// Tesseract 4 declares ETEXT_DESC outside its namespace, 5 inside it, and
// unqualified lookup here finds whichever exists
namespace ocr_compat
{
using namespace tesseract;
using monitor_t = ETEXT_DESC;
}  // namespace ocr_compat
using ocr_monitor_t = ocr_compat::monitor_t;

// --------------------
// OCR
// --------------------
static std::string_view psm_to_str(tesseract::PageSegMode psm)
{
    using namespace tesseract;
    switch (psm)
    {
        case PSM_SINGLE_CHAR:            return "Single character";
        case PSM_SINGLE_WORD:            return "Single word";
        case PSM_SINGLE_LINE:            return "Single line";
        case PSM_SINGLE_BLOCK_VERT_TEXT: return "Vertical block";
        case PSM_SPARSE_TEXT:            return "Sparsed text - big region";
        case PSM_SINGLE_BLOCK:           return "Mid-size block";
        case PSM_AUTO:
        case PSM_AUTO_OSD:               return "Auto detection";
        default:                         return "Unknown";
    }
}

static tesseract::PageSegMode choose_psm(const text_layout_t& layout)
{
    using namespace tesseract;

    if (g_config->Runtime.preferred_psm != 0)
        return PageSegMode(g_config->Runtime.preferred_psm);

    // The page layout analysis tends to drop a single short line
    if (layout.lines == 1)
        return PSM_SINGLE_LINE;

    return PSM_AUTO;
}

// Models for scripts written without spaces between words
static bool is_cjk_model(std::string_view model)
{
    return std::ranges::any_of(
        std::to_array<std::string_view>({ "jpn", "chi_", "kor", "Japanese", "HanS", "HanT", "Hangul" }),
        [&](std::string_view m) { return model.find(m) != std::string_view::npos; });
}

// Load a model into `api`, set up for the images normalize_for_ocr() makes
static bool init_engine(tesseract::TessBaseAPI&  api,
                        const char*              data_path,
                        const char*              model,
                        tesseract::OcrEngineMode oem)
{
    if (api.Init(data_path, model, oem) != 0)
        return false;

    // Otherwise Tesseract puts a space between every CJK character
    api.SetVariable("preserve_interword_spaces", is_cjk_model(model) ? "1" : "0");
    // Lines read under 70% are read again inverted, which never helps as the
    // text is always dark on white here, and is most lines of a busy background
    api.SetVariable("invert_threshold", "0");
    return true;
}

OcrAPI::OcrAPI() : m_api(std::make_unique<tesseract::TessBaseAPI>())
{}

OcrAPI::~OcrAPI()
{
    End();
}

void OcrAPI::End()
{
    m_pool.reset();
    if (m_api && m_initialized)
        m_api->End();
    m_initialized = false;
    m_config.reset();
}

Result<> OcrAPI::Configure(const char* data_path, const char* model, tesseract::OcrEngineMode oem)
{
    ocr_config_t next{ .path = data_path, .model = model, .oem = oem };

    if (m_config && *m_config == next)
        return Ok();

    // Tear down old engine
    m_pool.reset();
    if (m_initialized)
    {
        m_api->End();
        m_initialized = false;
    }

    if (!init_engine(*m_api, data_path, model, oem))
        return Err("Failed to Init OCR engine");

    m_config      = std::move(next);
    m_initialized = true;
    return Ok();
}

// A box Tesseract found in the image normalize_for_ocr() made, in the pixels
// of the image it was made from (rotated back, unpadded, unscaled and clamped)
static ocr_word_t map_box_to_source(int                    left,
                                    int                    top,
                                    int                    right,
                                    int                    bottom,
                                    const ocr_transform_t& xform,
                                    int                    proc_w,
                                    int                    proc_h,
                                    float                  src_w,
                                    float                  src_h)
{
    const float cx = float(proc_w) / 2.0f, cy = float(proc_h) / 2.0f;
    const float c = std::cos(xform.angle), s = std::sin(xform.angle);

    float                                    min_x = src_w, min_y = src_h, max_x = 0.0f, max_y = 0.0f;
    const std::array<std::pair<int, int>, 4> corners = {
        { { left, top }, { right, top }, { left, bottom }, { right, bottom } }
    };
    for (const auto& [px, py] : corners)
    {
        // pixRotate() turned the image clockwise by angle, so turn the corner back
        const float dx = float(px) - cx, dy = float(py) - cy;
        const float x = ((cx + (dx * c) + (dy * s)) - float(xform.pad)) / xform.scale;
        const float y = ((cy - (dx * s) + (dy * c)) - float(xform.pad)) / xform.scale;
        min_x         = std::min(min_x, x);
        min_y         = std::min(min_y, y);
        max_x         = std::max(max_x, x);
        max_y         = std::max(max_y, y);
    }

    min_x = std::clamp(min_x, 0.0f, src_w);
    min_y = std::clamp(min_y, 0.0f, src_h);
    max_x = std::clamp(max_x, min_x, src_w);
    max_y = std::clamp(max_y, min_y, src_h);
    return ocr_word_t{ .text       = {},
                       .x          = min_x,
                       .y          = min_y,
                       .w          = max_x - min_x,
                       .h          = max_y - min_y,
                       .confidence = {},
                       .line       = {},
                       .block      = {} };
}

static void trim(std::string& s)
{
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::ranges::find_if(s, not_ws));
    s.erase(std::ranges::find_if(std::views::reverse(s), not_ws).base(), s.end());
}

// Read a grayscale image made from the capture. The result is empty if nothing was recognized, and `monitor`
// can cancel it between text lines
static Result<ocr_result_t> recognize(tesseract::TessBaseAPI& api,
                                      PIX*                    gray,
                                      const text_layout_t&    layout,
                                      float                   src_w,
                                      float                   src_h,
                                      ocr_monitor_t*          monitor = nullptr)
{
    using TextPtr = std::unique_ptr<char, void (*)(char*)>;

    ocr_transform_t xform;
    PixPtr          pix(normalize_for_ocr(gray, layout, xform));
    if (!pix)
        return Err("Failed to preprocess image");

    const int                    proc_w = pixGetWidth(pix.get());
    const int                    proc_h = pixGetHeight(pix.get());
    const tesseract::PageSegMode psm    = choose_psm(layout);
    // The glyphs are now the size of ~10pt text at 300 DPI, whatever the screen
    constexpr int effective_dpi = 300;

#ifdef _OPENMP
    // Tesseract's LSTM opens an OpenMP parallel region for each step along a
    // line, and waking its threads costs more than they save (~40% of the CPU,
    // and slower). The setting is per thread, so it's made here
    omp_set_max_active_levels(0);
#endif

    api.SetPageSegMode(psm);
    api.SetImage(pix.get());
    api.SetSourceResolution(effective_dpi);

    if (api.Recognize(monitor) != 0)
        return Err("tesseract::Recognize() failed");

#if DEBUG
    // $OSHOT_DUMP_OCR=<dir> saves each pass's image as Tesseract got it, and
    // Tesseract's own binarization of it (used for layout analysis)
    if (const char* dir = std::getenv("OSHOT_DUMP_OCR"))
    {
        static std::atomic<int> passes = 0;
        const int               pass   = ++passes;
        pixWrite(fmt::format("{}/{:02}_input.png", dir, pass).c_str(), pix.get(), IFF_PNG);
        if (PIX* th = api.GetThresholdedImage())
        {
            pixWrite(fmt::format("{}/{:02}_threshold.png", dir, pass).c_str(), th, IFF_PNG);
            pixDestroy(&th);
        }
        spdlog::debug("OCR pass {} ({}) saved in {}", pass, psm_to_str(psm), dir);
    }
#endif

    TextPtr text(api.GetUTF8Text(), [](char* p) { delete[] p; });
    if (!text)
        return Err("Failed to get recognized text");

    ocr_result_t ret;
    ret.data = text.get();
    trim(ret.data);
    ret.psm_str    = psm_to_str(psm);
    ret.psm        = psm;
    ret.confidence = 0;
    if (ret.data.empty())
        return Ok(std::move(ret));

    if (tesseract::ResultIterator* ri = api.GetIterator())
    {
        double sum   = 0.0;
        int    count = 0;
        int    line = -1, block = -1;
        do
        {
            if (ri->Empty(tesseract::RIL_WORD))
                continue;

            float conf = ri->Confidence(tesseract::RIL_WORD);
            if (conf >= 0.0f)
            {
                sum += conf;
                ++count;
            }

            if (ri->IsAtBeginningOf(tesseract::RIL_BLOCK))
                ++block;
            if (ri->IsAtBeginningOf(tesseract::RIL_TEXTLINE))
                ++line;

            TextPtr word(ri->GetUTF8Text(tesseract::RIL_WORD), [](char* p) { delete[] p; });
            int     left = 0, top = 0, right = 0, bottom = 0;
            if (!word || !ri->BoundingBox(tesseract::RIL_WORD, &left, &top, &right, &bottom))
                continue;

            ocr_word_t& w = ret.words.emplace_back(
                map_box_to_source(left, top, right, bottom, xform, proc_w, proc_h, src_w, src_h));
            w.text       = word.get();
            w.confidence = int(std::round(std::max(conf, 0.0f)));
            w.line       = std::max(line, 0);
            w.block      = std::max(block, 0);
        } while (ri->Next(tesseract::RIL_WORD));

        ret.confidence = count ? int(std::round(sum / count)) : 0;
        delete ri;
    }
    else
    {
        ret.confidence = api.MeanTextConf();
    }

    return Ok(std::move(ret));
}

// Words read at 50% or more. Stripes or a background shade read as text get ~0
constexpr int sure_word = 50;

// Text in the words read at sure_word or more
static size_t sure_text(const std::vector<ocr_word_t>& words)
{
    size_t n = 0;
    for (const ocr_word_t& w : words)
        if (w.confidence >= sure_word)
            n += w.text.size();
    return n;
}

// A text line from one reading, in the pixels of the capture
struct ocr_line_t
{
    std::string             text;
    std::vector<ocr_word_t> words;
    float                   left = 0, top = 0, right = 0, bottom = 0;
    size_t                  sure       = 0;
    int                     confidence = 0;      // mean over its words
    bool                    has_text   = false;  // a word of 2+ letters at sure_word, or of 1 at 85%
};

// Add the lines of `r` to `lines`, moved by (dx, dy). `sep` goes between
// words if their line's text can't be taken from Tesseract's
static void add_lines(std::vector<ocr_line_t>& lines,
                      const ocr_result_t&      r,
                      std::string_view         sep,
                      float                    dx = 0,
                      float                    dy = 0)
{
    const auto space = [](unsigned char c) { return std::isspace(c); };
    // Any byte of a multibyte UTF-8 character counts, for the scripts that aren't ASCII,
    // so a single CJK character has enough
    const auto letter = [](unsigned char c) { return c >= 0x80 || std::isalnum(c); };

    // Tesseract's text has a line per text line, with its own spacing (none
    // between CJK characters, but before a Latin word)
    std::vector<std::string> texts;
    for (std::string t : split(r.data, '\n'))
    {
        trim(t);
        if (!t.empty())
            texts.push_back(std::move(t));
    }

    std::vector<std::vector<ocr_word_t>> groups;
    for (size_t i = 0; i < r.words.size(); ++i)
    {
        if (i == 0 || r.words[i].line != r.words[i - 1].line)
            groups.emplace_back();
        groups.back().push_back(r.words[i]);
    }
    const bool own_texts = texts.size() == groups.size();

    for (size_t g = 0; g < groups.size(); ++g)
    {
        ocr_line_t l;
        l.left = l.top = std::numeric_limits<float>::max();
        int sum        = 0;
        for (ocr_word_t& w : groups[g])
        {
            // Tesseract also returns "words" that are only spaces
            if (std::ranges::all_of(w.text, space))
                continue;
            w.x += dx;
            w.y += dy;
            l.left   = std::min(l.left, w.x);
            l.top    = std::min(l.top, w.y);
            l.right  = std::max(l.right, w.x + w.w);
            l.bottom = std::max(l.bottom, w.y + w.h);
            sum += w.confidence;
            const auto letters = std::ranges::count_if(w.text, letter);
            l.has_text |= (w.confidence >= sure_word && letters >= 2) || (w.confidence >= 85 && letters >= 1);
            if (!own_texts && !l.words.empty())
                l.text += sep;
            if (!own_texts)
                l.text += w.text;
            l.words.push_back(std::move(w));
        }
        if (l.words.empty())
            continue;

        if (own_texts)
            l.text = std::move(texts[g]);
        l.sure       = sure_text(l.words);
        l.confidence = int(std::round(float(sum) / float(l.words.size())));
        lines.push_back(std::move(l));
    }
}

// One reading made of the best reading of each line any pass found. Lines are taken from the most read text down,
// skipping any that covers most of a word already taken, and need has_text and a mean confidence of sure_word
// (or background scraps like "| |", read at 60-80%, get in). They keep the order of `order`, the best whole
// reading, since Tesseract's layout analysis knows about columns and tables
static ocr_result_t merge_lines(std::vector<ocr_line_t> lines, const ocr_result_t& order)
{
    std::ranges::stable_sort(lines, [](const ocr_line_t& a, const ocr_line_t& b) {
        return a.sure != b.sure ? a.sure > b.sure : a.confidence > b.confidence;
    });

    std::vector<ocr_line_t> taken;
    for (ocr_line_t& l : lines)
    {
        // By word, as one stray word can stretch a line's box over the next line
        const auto overlaps = [&](const ocr_line_t& t) {
            return std::ranges::any_of(l.words, [&](const ocr_word_t& a) {
                return std::ranges::any_of(t.words, [&](const ocr_word_t& b) {
                    const float w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
                    const float h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
                    return w > 0 && h > 0 && w * h > std::min(a.w * a.h, b.w * b.h) / 2;
                });
            });
        };
        if (l.has_text && l.confidence >= sure_word && std::ranges::none_of(taken, overlaps))
            taken.push_back(std::move(l));
    }

    // Where each line goes: after the line of `order` it covers the most of.
    // One it doesn't cover goes after the last line that starts above it
    std::vector<ocr_line_t> order_lines;
    add_lines(order_lines, order, " ");
    const auto slot = [&](const ocr_line_t& l) {
        float  most = 0;
        size_t at   = order_lines.size();
        for (size_t i = 0; i < order_lines.size(); ++i)
        {
            const ocr_line_t& o = order_lines[i];
            const float       w = std::min(l.right, o.right) - std::max(l.left, o.left);
            const float       h = std::min(l.bottom, o.bottom) - std::max(l.top, o.top);
            if (w > 0 && h > 0 && w * h > most)
            {
                most = w * h;
                at   = i;
            }
        }
        if (most == 0)
            at = size_t(std::ranges::count_if(order_lines, [&](const ocr_line_t& o) { return o.top <= l.top; }));
        return std::tuple(at, l.top, l.left);
    };
    std::ranges::sort(taken, {}, slot);

    ocr_result_t ret;
    ret.psm     = tesseract::PSM_SINGLE_LINE;
    ret.psm_str = "Line by line";
    int sum     = 0;
    for (size_t i = 0; i < taken.size(); ++i)
    {
        if (i > 0)
            ret.data += '\n';
        ret.data += taken[i].text;
        for (ocr_word_t& w : taken[i].words)
        {
            sum += w.confidence;
            w.line  = int(i);
            w.block = 0;
            ret.words.push_back(std::move(w));
        }
    }
    ret.confidence = ret.words.empty() ? 0 : int(std::round(float(sum) / float(ret.words.size())));
    return ret;
}

struct line_box_t
{
    int x = 0, y = 0, w = 0, h = 0;
    int row_top = 0, row_bottom = 0;  // of a line, its glyphs in the padded box

    bool operator==(const line_box_t&) const = default;
};

// Boxes around the text lines of a grayscale image, from its glyphs. Unlike Tesseract's layout analysis, a big
// title over small captions doesn't join lines. Glyphs of a similar height whose middles share a band make a row,
// split at gaps wider than a few glyphs (columns), and a line needs at least 3 of them
static std::vector<line_box_t> find_lines(PIX* gray, int threshold)
{
    std::vector<line_box_t> lines;
    PIX*                    bin = pixThresholdToBinary(gray, threshold);
    if (!bin)
        return lines;
    BOXA* boxa = pixConnCompBB(bin, 8);
    pixDestroy(&bin);
    if (!boxa)
        return lines;

    const int               img_w = pixGetWidth(gray), img_h = pixGetHeight(gray);
    std::vector<line_box_t> glyphs;
    for (int i = 0; i < boxaGetCount(boxa); ++i)
    {
        line_box_t g;
        boxaGetBoxGeometry(boxa, i, &g.x, &g.y, &g.w, &g.h);
        // Specks, rules and frames
        if (g.h < 4 || g.w > g.h * 8 || g.h > img_h / 2)
            continue;
        glyphs.push_back(g);
    }
    boxaDestroy(&boxa);

    struct row_t
    {
        int                     top, bottom, h;
        std::vector<line_box_t> glyphs;
    };
    std::vector<row_t> rows;
    std::ranges::sort(glyphs, {}, &line_box_t::y);
    for (const line_box_t& g : glyphs)
    {
        const int mid = g.y + (g.h / 2);
        auto      row = std::ranges::find_if(
            rows, [&](const row_t& r) { return mid >= r.top && mid <= r.bottom && g.h * 2 >= r.h && g.h <= r.h * 2; });
        if (row == rows.end())
        {
            rows.push_back({ .top = g.y, .bottom = g.y + g.h, .h = g.h, .glyphs = { g } });
            continue;
        }
        row->top    = std::min(row->top, g.y);
        row->bottom = std::max(row->bottom, g.y + g.h);
        row->h      = std::max(row->h, g.h);
        row->glyphs.push_back(g);
    }

    constexpr int min_glyphs = 3;
    for (row_t& r : rows)
    {
        std::ranges::sort(r.glyphs, {}, &line_box_t::x);
        const int pad = r.h / 2;
        for (size_t start = 0, i = 1; i <= r.glyphs.size(); ++i)
        {
            if (i < r.glyphs.size() && r.glyphs[i].x - (r.glyphs[i - 1].x + r.glyphs[i - 1].w) <= r.h * 3)
                continue;
            if (i - start >= min_glyphs)
            {
                const int x0 = std::max(0, r.glyphs[start].x - pad);
                const int x1 = std::min(img_w, r.glyphs[i - 1].x + r.glyphs[i - 1].w + pad);
                const int y0 = std::max(0, r.top - pad), y1 = std::min(img_h, r.bottom + pad);
                // A little more than the glyphs for punctuation and accents
                const int margin = r.h / 8;
                lines.push_back({ .x          = x0,
                                  .y          = y0,
                                  .w          = x1 - x0,
                                  .h          = y1 - y0,
                                  .row_top    = std::max(0, r.top - margin - y0),
                                  .row_bottom = std::min(y1, r.bottom + margin) - y0 });
            }
            start = i;
        }
    }

    return lines;
}

// A grayscale image made from the capture, with its text measured
struct ocr_pass_t
{
    PixPtr        gray;
    text_layout_t layout;
};

static ocr_pass_t make_pass(PIX* gray)
{
    return { .gray = PixPtr(gray), .layout = gray ? measure_text(gray) : text_layout_t{} };
}

// A line of a pass, cut out to be read on its own
struct line_crop_t
{
    line_box_t box;
    PixPtr     crop;
};

// The lines find_lines() finds in a pass, each with only its own glyphs
static std::vector<line_crop_t> crop_lines(const ocr_pass_t& pass)
{
    // A page of text would take seconds, and its layout is rarely the problem
    constexpr size_t max_lines = 40;

    std::vector<line_crop_t>      crops;
    const std::vector<line_box_t> boxes = find_lines(pass.gray.get(), pass.layout.threshold);
    if (boxes.size() > max_lines)
        return crops;

    for (const line_box_t& b : boxes)
    {
        BOX*   box = boxCreate(b.x, b.y, b.w, b.h);
        PixPtr crop(pixClipRectangle(pass.gray.get(), box, nullptr));
        boxDestroy(&box);
        if (!crop)
            continue;

        // Only this line, not the edges of the ones above and below in the
        // padding (pixSetInRect() turns 8 bpp pixels white)
        for (const auto [top, bottom] : { std::pair(0, b.row_top), std::pair(b.row_bottom, b.h) })
        {
            if (bottom <= top)
                continue;
            BOX* band = boxCreate(0, top, b.w, bottom - top);
            pixSetInRect(crop.get(), band);
            boxDestroy(&band);
        }
        crops.push_back({ .box = b, .crop = std::move(crop) });
    }

    return crops;
}

// Reads the passes of a busy background on a few Tesseract engines at once. The caller's engine reads in Take(),
// and the others load the same model on their own threads and take reads in order
class OcrEnginePool
{
public:
    using read_t = std::optional<ocr_result_t>;

    // A read started with Read(), whose result Take() returns
    struct ticket_t
    {
        uint64_t            id = 0;
        std::future<read_t> result;
    };

    OcrEnginePool(tesseract::TessBaseAPI&  main,
                  const std::string&       data_path,
                  const std::string&       model,
                  tesseract::OcrEngineMode oem,
                  size_t                   engines)
        : m_main(main)
    {
        for (size_t i = 1; i < engines; ++i)
        {
            m_threads.emplace_back([this, data_path, model, oem] {
                // One at a time (~50 ms each), as loading takes ~50 MB more
                // than the engine keeps, for the whole model file
                tesseract::TessBaseAPI api;
                std::unique_lock       loading(m_loading);
                if (m_stop || !init_engine(api, data_path.c_str(), model.c_str(), oem))
                    return;
#if defined(__GLIBC__)
                // That memory stays in this thread's arena otherwise
                malloc_trim(0);
#endif
                loading.unlock();
                Run(api);
            });
        }
    }

    ~OcrEnginePool()
    {
        {
            std::scoped_lock lock(m_mutex);
            m_stop = true;
        }
        Cancel();
        m_wake.notify_all();
        for (std::thread& t : m_threads)
            t.join();
    }

    OcrEnginePool(const OcrEnginePool&)            = delete;
    OcrEnginePool& operator=(const OcrEnginePool&) = delete;

    [[nodiscard]] size_t Size() const { return m_threads.size() + 1; }

    // Read a pass, as the whole image of `src_w` x `src_h` px
    ticket_t Read(const ocr_pass_t& pass, float src_w, float src_h)
    {
        // Its own copy, as Leptonica's reference counts aren't thread-safe
        return Submit([gray = PixPtr(pixCopy(nullptr, pass.gray.get())), layout = pass.layout, src_w, src_h](
                          tesseract::TessBaseAPI& api, ocr_monitor_t* monitor) -> read_t {
            if (!gray)
                return std::nullopt;
            Result<ocr_result_t> res = recognize(api, gray.get(), layout, src_w, src_h, monitor);
            return res ? read_t(std::move(res.get())) : std::nullopt;
        });
    }

    // Read a line on its own, as an image the size of its box
    ticket_t Read(const line_crop_t& line)
    {
        return Submit([crop = PixPtr(pixCopy(nullptr, line.crop.get())), w = line.box.w, h = line.box.h](
                          tesseract::TessBaseAPI& api, ocr_monitor_t* monitor) -> read_t {
            if (!crop)
                return std::nullopt;
            text_layout_t layout     = measure_text(crop.get());
            layout.lines             = 1;  // read as a single line
            Result<ocr_result_t> res = recognize(api, crop.get(), layout, float(w), float(h), monitor);
            return res ? read_t(std::move(res.get())) : std::nullopt;
        });
    }

    // The result of a read, read now with the caller's engine if no other has started it
    read_t Take(ticket_t& ticket)
    {
        std::optional<job_t> job;
        {
            std::scoped_lock lock(m_mutex);
            if (auto it = std::ranges::find(m_jobs, ticket.id, &job_t::id); it != m_jobs.end())
            {
                job = std::move(*it);
                m_jobs.erase(it);
            }
        }
        if (job)
            Execute(*job, m_main);
        return ticket.result.get();
    }

    // Drop the reads not started yet and stop the running ones at their next text line, without waiting for them
    // (they don't use the caller's engine)
    void Cancel()
    {
        std::scoped_lock lock(m_mutex);
        m_jobs.clear();
        ++m_generation;
    }

private:
    struct job_t
    {
        uint64_t                                                            id;
        uint64_t                                                            generation;
        std::packaged_task<read_t(tesseract::TessBaseAPI&, ocr_monitor_t*)> task;
    };

    template <typename F>
    ticket_t Submit(F&& fn)
    {
        job_t    job{ .id = 0, .generation = 0, .task = decltype(job_t::task)(std::forward<F>(fn)) };
        ticket_t ret{ .result = job.task.get_future() };
        {
            std::scoped_lock lock(m_mutex);
            job.id = ret.id = ++m_last_id;
            job.generation  = m_generation;
            m_jobs.push_back(std::move(job));
        }
        m_wake.notify_one();
        return ret;
    }

    void Execute(job_t& job, tesseract::TessBaseAPI& api)
    {
        // Cancelled once Cancel() moved on to the next generation of reads
        struct cancel_t
        {
            const std::atomic<uint64_t>& current;
            uint64_t                     generation;
        } cancel{ .current = m_generation, .generation = job.generation };

        ocr_monitor_t monitor;
        monitor.cancel_this = &cancel;
        monitor.cancel      = [](void* data, int) {
            const auto* c = static_cast<cancel_t*>(data);
            return c->current != c->generation;
        };
        job.task(api, &monitor);
    }

    void Run(tesseract::TessBaseAPI& api)
    {
        for (;;)
        {
            job_t job;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
                if (m_stop)
                    return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            Execute(job, api);
        }
    }

    tesseract::TessBaseAPI&  m_main;
    std::mutex               m_loading;
    std::mutex               m_mutex;
    std::condition_variable  m_wake;  // a read was added, or the pool is stopping
    std::deque<job_t>        m_jobs;
    uint64_t                 m_last_id    = 0;
    std::atomic<uint64_t>    m_generation = 0;
    bool                     m_stop       = false;
    std::vector<std::thread> m_threads;
};

// Clean text, even small or degraded, is read at 85% or more with no word
// under 10% (a word Tesseract couldn't make out at all, like one the
// background ate). Anything else may be text on a busy background
static bool unsure(const ocr_result_t& r)
{
    constexpr int retry_below      = 85;
    constexpr int retry_word_below = 10;
    return r.confidence < retry_below ||
           std::ranges::any_of(r.words, [](const ocr_word_t& w) { return w.confidence < retry_word_below; });
}

// The best reading has the most text in words read at sure_word or more,
// then the highest mean confidence. The mean alone prefers a few stray
// symbols read off the background over most of the text read a bit less surely
static bool better(const ocr_result_t& r, const ocr_result_t& than)
{
    const size_t a = sure_text(r.words), b = sure_text(than.words);
    return a != b ? a > b : r.confidence > than.confidence;
}

// Take `res` into `best` if it's better, adding its lines to `lines`.
// Returns whether it was taken
static bool consider(std::optional<ocr_result_t> res,
                     std::string_view            sep,
                     ocr_result_t&               best,
                     std::vector<ocr_line_t>&    lines)
{
    if (!res)
        return false;
    add_lines(lines, *res, sep);
    if (!better(*res, best))
        return false;
    best = std::move(*res);
    return true;
}

// The first pass: grayscale from each pixel's distance from the background around it
static ocr_pass_t background_pass(const std::vector<rgb_t>& px, const std::vector<rgb_t>& colors, int w, int h)
{
    // The background window must be a few glyphs wide, which we only know
    // once we've seen the text. Measure it first, then redo it for big text
    constexpr int default_window = 64;
    ocr_pass_t    pass           = make_pass(distance_from_background(px, colors, w, h, default_window));
    if (pass.gray && pass.layout.char_height * 3 > default_window)
        pass = make_pass(distance_from_background(px, colors, w, h, pass.layout.char_height * 3));
    return pass;
}

// Passes with a main color as the text color, for text on a busy or graded background. The colors are tried by
// their distance from the most common one, since text stands out from it more than the background's other shades
static std::vector<ocr_pass_t> text_color_passes(const std::vector<rgb_t>& px,
                                                 const std::vector<rgb_t>& colors,
                                                 int                       w,
                                                 int                       h)
{
    constexpr size_t max_tries = 3;

    std::vector<size_t> order(colors.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::stable_sort(
        order, std::ranges::greater{}, [&](size_t i) { return color_dist2(colors[i], colors[0]); });

    std::vector<ocr_pass_t> passes;
    for (size_t i : order)
    {
        if (passes.size() >= max_tries)
            break;
        const rgb_t& bg = colors[(i == 0 && colors.size() > 1) ? 1 : 0];
        if (PIX* gray = distance_from_text_color(px, colors[i], bg, w, h))
            passes.push_back(make_pass(gray));
    }
    return passes;
}

// Read text the first pass was unsure of, improving `best`. Spare engines read ahead the passes that may be needed
// next, but only the results a one by one read would use are taken, so the text doesn't depend on the engine count
static void read_busy_text(OcrEnginePool&            engines,
                           const std::vector<rgb_t>& px,
                           const std::vector<rgb_t>& colors,
                           int                       w,
                           int                       h,
                           std::string_view          sep,
                           ocr_result_t&             best)
{
    using read_t = OcrEnginePool::read_t;

    const auto              src_w = float(w), src_h = float(h);
    const bool              speculate = engines.Size() > 1;
    std::vector<ocr_line_t> lines;  // every line any pass read, to merge at the end
    add_lines(lines, best, sep);

    // Each main color as the text color, keeping the best reading
    const std::vector<ocr_pass_t>        candidates = text_color_passes(px, colors, w, h);
    std::vector<OcrEnginePool::ticket_t> reads;
    reads.reserve(candidates.size());
    for (const ocr_pass_t& pass : candidates)
        reads.push_back(engines.Read(pass, src_w, src_h));

    // Colors read this surely are text, kept to combine below
    constexpr int                  text_color_min = sure_word;
    std::vector<const ocr_pass_t*> text_passes;
    const ocr_pass_t*              best_pass = nullptr;
    for (size_t i = 0; i < candidates.size() && unsure(best); ++i)
    {
        read_t res = engines.Take(reads[i]);
        if (res && res->confidence >= text_color_min)
            text_passes.push_back(&candidates[i]);
        if (consider(std::move(res), sep, best, lines))
            best_pass = &candidates[i];
    }
    if (!unsure(best))
        return;

    // Text in more than one color (highlighted words) is only partly there in
    // each color's pass, so each pixel takes its distance from the nearest one
    ocr_pass_t                             combined;
    std::optional<OcrEnginePool::ticket_t> combined_read;
    if (text_passes.size() > 1)
    {
        PixPtr min(pixMinOrMax(nullptr, text_passes[0]->gray.get(), text_passes[1]->gray.get(), L_CHOOSE_MIN));
        for (size_t i = 2; i < text_passes.size() && min; ++i)
            min.reset(pixMinOrMax(nullptr, min.get(), text_passes[i]->gray.get(), L_CHOOSE_MIN));
        combined = make_pass(min.release());
        if (combined.gray)
            combined_read = engines.Read(combined, src_w, src_h);
    }

    // The best text color may only be the outline around the letters, so the
    // best pass is read again with them filled. Which pass that is depends on
    // the combined one, so with spare engines both are filled
    struct filled_t
    {
        const ocr_pass_t*       from;
        ocr_pass_t              pass;
        OcrEnginePool::ticket_t read;
    };
    std::deque<filled_t> fills;
    const auto           fill = [&](const ocr_pass_t* from) -> filled_t& {
        if (auto it = std::ranges::find(fills, from, &filled_t::from); it != fills.end())
            return *it;
        filled_t& f = fills.emplace_back(
            filled_t{ .from = from,
                      .pass = make_pass(fill_outlined_text(from->gray.get(), from->layout.threshold)),
                      .read = {} });
        if (f.pass.gray)
            f.read = engines.Read(f.pass, src_w, src_h);
        return f;
    };

    // For text that needs different passes (an outlined title over a caption), or lines the layout analysis joins
    // with the background, the text passes are also read line by line. A line cut the same out of two is read once
    std::vector<line_crop_t>             crops;
    std::vector<OcrEnginePool::ticket_t> line_reads;
    const auto                           read_lines_of = [&](const ocr_pass_t& pass) {
        for (line_crop_t& c : crop_lines(pass))
        {
            const auto same = [&](const line_crop_t& o) {
                l_int32 equal = 0;
                return o.box == c.box && pixEqual(o.crop.get(), c.crop.get(), &equal) == 0 && equal;
            };
            if (std::ranges::any_of(crops, same))
                continue;
            line_reads.push_back(engines.Read(c));
            crops.push_back(std::move(c));
        }
    };

    if (speculate)
    {
        if (best_pass)
            fill(best_pass);
        if (combined.gray)
            fill(&combined);
        for (const ocr_pass_t* pass : text_passes)
            read_lines_of(*pass);
    }

    if (combined_read)
    {
        if (consider(engines.Take(*combined_read), sep, best, lines))
            best_pass = &combined;
        if (!unsure(best))
            return;
    }

    const ocr_pass_t* filled = nullptr;
    if (best_pass)
    {
        filled_t& f = fill(best_pass);
        if (f.pass.gray)
        {
            filled = &f.pass;
            consider(engines.Take(f.read), sep, best, lines);
        }
        if (!unsure(best))
            return;
    }

    if (!speculate)
        for (const ocr_pass_t* pass : text_passes)
            read_lines_of(*pass);
    if (filled)
        read_lines_of(*filled);
    for (size_t i = 0; i < line_reads.size(); ++i)
        if (read_t res = engines.Take(line_reads[i]))
            add_lines(lines, *res, sep, float(crops[i].box.x), float(crops[i].box.y));

    ocr_result_t merged = merge_lines(std::move(lines), best);
    if (better(merged, best))
        best = std::move(merged);
}

Result<ocr_result_t> OcrAPI::ExtractTextCapture(const capture_result_t& cap)
{
    if (!m_initialized)
        return Err("Initialize the engine first");

    if (cap.view().empty() || cap.w <= 0 || cap.h <= 0)
        return Err("Image is empty");

    const size_t required = size_t(cap.w) * cap.h * 4;
    if (cap.view().size() < required)
        return Err(fmt::format(
            "Image buffer too small: got {} bytes, need {} ({}x{} × 4)", cap.view().size(), required, cap.w, cap.h));

    std::vector<rgb_t> px(size_t(cap.w) * cap.h);
    for (size_t i = 0; i < px.size(); ++i)
        px[i] = opaque_rgb(cap.view().data() + (i * 4));
    const std::vector<rgb_t> colors = main_colors(px);

    const ocr_pass_t first_pass = background_pass(px, colors, cap.w, cap.h);
    if (!first_pass.gray)
        return Err("Failed to convert image into Pix format");

    Result<ocr_result_t> first =
        recognize(*m_api, first_pass.gray.get(), first_pass.layout, float(cap.w), float(cap.h));
    if (!first)
        return first;
    ocr_result_t best = std::move(first.get());

    if (unsure(best))
    {
        // One engine per core, up to a few, each ~20 MB. Kept until End(), for
        // the next OCR of the capture
        if (!m_pool)
        {
            constexpr unsigned max_engines = 5;
            const unsigned     engines     = std::clamp(std::thread::hardware_concurrency(), 1u, max_engines);
            m_pool = std::make_unique<OcrEnginePool>(*m_api, m_config->path, m_config->model, m_config->oem, engines);
        }

        read_busy_text(*m_pool, px, colors, cap.w, cap.h, is_cjk_model(m_config->model) ? "" : " ", best);
        m_pool->Cancel();  // the reads it started and didn't need
    }

    if (best.data.empty())
        return Err(fmt::format("No text recognized (PSM: {}, {}x{} px)", best.psm_str, cap.w, cap.h));

    return Ok(std::move(best));
}
