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

#include "text_extraction.hpp"

#include <tesseract/publictypes.h>
#include <zbar.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <numeric>
#include <ranges>
#include <utility>
#include <vector>

#include "config.hpp"
#include "screen_capture.hpp"
#include "util.hpp"

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

struct PixFree
{
    void operator()(PIX* pix) const
    {
        if (pix)
            pixDestroy(&pix);
    }
};
using PixPtr = std::unique_ptr<PIX, PixFree>;

using rgb_t = std::array<uint8_t, 3>;

// Composited over white, so the transparent parts of an opened image read as background
static rgb_t opaque_rgb(const uint8_t* p)
{
    const unsigned a = p[3];
    if (a == 255)
        return { p[0], p[1], p[2] };

    const auto blend = [a](unsigned c) { return uint8_t(((c * a) + (255 * (255 - a)) + 127) / 255); };
    return { blend(p[0]), blend(p[1]), blend(p[2]) };
}

static int color_dist2(const rgb_t& a, const rgb_t& b)
{
    const int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return (dr * dr) + (dg * dg) + (db * db);
}

// The image's main colors, most common first: the most populated bins of a
// 4 bits per channel histogram, each averaged to its real color
static std::vector<rgb_t> main_colors(const std::vector<rgb_t>& px)
{
    constexpr size_t max_colors = 8;
    const size_t     step       = std::max<size_t>(1, px.size() / 1'000'000);  // sampling is plenty

    const auto bin = [](const rgb_t& c) { return ((c[0] >> 4) << 8) | ((c[1] >> 4) << 4) | (c[2] >> 4); };

    std::vector<uint32_t> hist(4096);
    size_t                samples = 0;
    for (size_t i = 0; i < px.size(); i += step, ++samples)
        ++hist[bin(px[i])];

    std::vector<int> bins(hist.size());
    std::iota(bins.begin(), bins.end(), 0);
    std::ranges::partial_sort(bins, bins.begin() + max_colors, [&](int a, int b) { return hist[a] > hist[b]; });

    std::vector<rgb_t> colors;
    for (size_t k = 0; k < max_colors; ++k)
    {
        // Text and antialiasing, not an area
        if (k > 0 && hist[bins[k]] * 200 < samples)
            break;

        std::array<uint64_t, 3> sum{};
        uint64_t                count = 0;
        for (size_t i = 0; i < px.size(); i += step)
        {
            if (bin(px[i]) != bins[k])
                continue;
            for (size_t c = 0; c < 3; ++c)
                sum[c] += px[i][c];
            ++count;
        }
        colors.push_back({ uint8_t(sum[0] / count), uint8_t(sum[1] / count), uint8_t(sum[2] / count) });
    }

    return colors;
}

// Grayscale where each pixel is its color distance from the background around
// it: dark text on white for any pair of colors (white on blue, red on black,
// black on yellow...), in every panel of a screenshot that mixes light, dark
// and colored areas, without guessing when to invert.
// The local background is whichever main color covers the most of a `window`
// px square around each 4x4 cell: text covers less of it than its background,
// and next to a straight edge between two areas a pixel's own side always
// covers at least half.
static PIX* distance_from_background(const std::vector<rgb_t>& px,
                                     const std::vector<rgb_t>& colors,
                                     int                       w,
                                     int                       h,
                                     int                       window)
{
    const int n = int(colors.size());

    constexpr int cell = 4;
    const int     cw = (w + cell - 1) / cell, ch = (h + cell - 1) / cell;

    // Main color of each pixel (if it's close to one), counted per cell, as
    // per-color summed-area tables over the cells
    constexpr int        tolerance2 = 32 * 32;
    std::vector<int32_t> sat(size_t(n) * (cw + 1) * (ch + 1));
    const auto           at = [&](int k, int cx, int cy) -> int32_t& {
        return sat[((size_t(k) * (ch + 1)) + cy) * (cw + 1) + cx];
    };
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const rgb_t& c       = px[(size_t(y) * w) + x];
            int          best    = -1;
            int          best_d2 = tolerance2 + 1;
            for (int k = 0; k < n; ++k)
            {
                const int d2 = color_dist2(c, colors[k]);
                if (d2 < best_d2)
                {
                    best_d2 = d2;
                    best    = k;
                }
            }
            if (best >= 0)
                ++at(best, (x / cell) + 1, (y / cell) + 1);
        }
    }
    for (int k = 0; k < n; ++k)
        for (int cy = 1; cy <= ch; ++cy)
            for (int cx = 1; cx <= cw; ++cx)
                at(k, cx, cy) += at(k, cx - 1, cy) + at(k, cx, cy - 1) - at(k, cx - 1, cy - 1);

    // Most common main color in the window of `radius` cells around a cell, and its share of the window
    const auto majority = [&](int cx, int cy, int radius) -> std::pair<int, float> {
        const int x0 = std::max(0, cx - radius), x1 = std::min(cw, cx + radius + 1);
        const int y0 = std::max(0, cy - radius), y1 = std::min(ch, cy + radius + 1);
        int       best = 0;
        int32_t   most = 0;
        for (int k = 0; k < n; ++k)
        {
            const int32_t count = at(k, x1, y1) - at(k, x0, y1) - at(k, x1, y0) + at(k, x0, y0);
            if (count > most)
            {
                most = count;
                best = k;
            }
        }
        return { best, float(most) / float((x1 - x0) * (y1 - y0) * cell * cell) };
    };

    // The big window misses the corners of an area (its own color covers a
    // quarter of the window there), so a color that fills most of a window a
    // quarter that size wins: text strokes never fill that much of it
    const int        radius       = std::max(1, window / (2 * cell));
    const int        small_radius = std::max(1, radius / 4);
    std::vector<int> cell_bg(size_t(cw) * ch, 0);
    for (int cy = 0; cy < ch; ++cy)
    {
        for (int cx = 0; cx < cw; ++cx)
        {
            const auto [near_color, near_share] = majority(cx, cy, small_radius);
            cell_bg[(size_t(cy) * cw) + cx]     = near_share >= 0.6f ? near_color : majority(cx, cy, radius).first;
        }
    }

    PIX* pix = pixCreate(w, h, 8);
    if (!pix)
        return nullptr;

    l_uint32* dst = pixGetData(pix);
    const int wpl = pixGetWpl(pix);
    for (int y = 0; y < h; ++y)
    {
        l_uint32* line = dst + static_cast<ptrdiff_t>(y) * wpl;
        for (int x = 0; x < w; ++x)
        {
            const rgb_t& bg = colors[cell_bg[(size_t(y / cell) * cw) + (x / cell)]];
            const int    d  = int(std::lround(std::sqrt(float(color_dist2(px[(size_t(y) * w) + x], bg)))));
            SET_DATA_BYTE(line, x, 255 - std::min(d, 255));
        }
    }

    return pix;
}

// Grayscale where each pixel is its color distance from `text`, so that color
// is black and anything far from it white. For text that has no background
// color of its own: outlined text on a game scene or a meme, or on a gradient.
// Returns nullptr if `text` covers most of the image, as it's the background
static PIX* distance_from_text_color(const std::vector<rgb_t>& px, const rgb_t& text, int w, int h)
{
    PIX* pix = pixCreate(w, h, 8);
    if (!pix)
        return nullptr;

    // Antialiasing halfway between the text and a black outline is still text
    constexpr int gain = 2;
    size_t        dark = 0;
    l_uint32*     dst  = pixGetData(pix);
    const int     wpl  = pixGetWpl(pix);
    for (int y = 0; y < h; ++y)
    {
        l_uint32* line = dst + static_cast<ptrdiff_t>(y) * wpl;
        for (int x = 0; x < w; ++x)
        {
            const int d = int(std::lround(std::sqrt(float(color_dist2(px[(size_t(y) * w) + x], text))))) * gain;
            SET_DATA_BYTE(line, x, std::min(d, 255));
            dark += d < 128;
        }
    }

    if (dark * 2 > size_t(w) * h)
        pixDestroy(&pix);
    return pix;
}

// Otsu's threshold, kept below 255 - 24 so a background with a little noise
// or JPEG artifacts doesn't count as text
static int text_threshold(PIX* gray)
{
    std::array<double, 256> hist{};
    const l_uint32*         data = pixGetData(gray);
    const int               wpl  = pixGetWpl(gray);
    const int               w = pixGetWidth(gray), h = pixGetHeight(gray);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            hist[GET_DATA_BYTE(data + static_cast<ptrdiff_t>(y) * wpl, x)] += 1;

    const double total = double(w) * h;
    double       sum   = 0;
    for (size_t i = 0; i < hist.size(); ++i)
        sum += double(i) * hist[i];

    double sum_low = 0, weight_low = 0, best_var = -1;
    int    best = 128;
    for (int t = 0; t < 256; ++t)
    {
        weight_low += hist[t];
        sum_low += double(t) * hist[t];
        const double weight_high = total - weight_low;
        if (weight_low == 0 || weight_high == 0)
            continue;

        const double mean_low = sum_low / weight_low, mean_high = (sum - sum_low) / weight_high;
        const double var = weight_low * weight_high * (mean_low - mean_high) * (mean_low - mean_high);
        if (var > best_var)
        {
            best_var = var;
            best     = t + 1;  // pixels < best are text
        }
    }

    return std::min(best, 255 - 24);
}

// Text drawn as outlines only, as white letters with a black border on a light
// picture look when read through the border's color: fill the inside of each
// letter, which is what lies inside an odd number of outlines, so an 'O'
// keeps its hole. The paper is walked from the edges of the image inwards,
// one outline at a time
static PIX* fill_outlined_text(PIX* gray, int threshold)
{
    const int w = pixGetWidth(gray), h = pixGetHeight(gray);
    PixPtr    ink(pixThresholdToBinary(gray, threshold));
    if (!ink)
        return nullptr;
    PixPtr paper(pixInvert(nullptr, ink.get()));
    PixPtr inside(pixCreate(w, h, 1));
    PixPtr seed(pixCreate(w, h, 1));
    if (!paper || !inside || !seed)
        return nullptr;

    pixSetOrClearBorder(seed.get(), 1, 1, 1, 1, PIX_SET);
    pixAnd(seed.get(), seed.get(), paper.get());

    // Letters inside the holes of other letters are rare, deeper is noise
    constexpr int max_depth = 6;
    for (int depth = 0; depth < max_depth; ++depth)
    {
        PixPtr  area(pixSeedfillBinary(nullptr, seed.get(), paper.get(), 4));
        l_int32 empty = 1;
        if (!area || pixZero(area.get(), &empty) != 0 || empty)
            break;
        if (depth % 2 == 1)
            pixOr(inside.get(), inside.get(), area.get());
        pixSubtract(paper.get(), paper.get(), area.get());

        // The outlines around this area, then the paper on their other side
        PixPtr border(pixDilateBrick(nullptr, area.get(), 3, 3));
        if (!border)
            break;
        pixAnd(border.get(), border.get(), ink.get());
        PixPtr outlines(pixSeedfillBinary(nullptr, border.get(), ink.get(), 8));
        if (!outlines)
            break;
        pixSubtract(ink.get(), ink.get(), outlines.get());

        seed.reset(pixDilateBrick(nullptr, outlines.get(), 3, 3));
        if (!seed)
            break;
        pixAnd(seed.get(), seed.get(), paper.get());
    }

    PIX* out = pixCopy(nullptr, gray);
    if (out)
        pixPaintThroughMask(out, inside.get(), 0, 0, 0);
    return out;
}

struct text_layout_t
{
    int char_height = 0;  // median height of the glyphs, 0 if no text was found
    int lines       = 0;
    int threshold   = 128;
};

// Glyph size and line count from the connected components of the text pixels
static text_layout_t measure_text(PIX* gray)
{
    text_layout_t layout;
    layout.threshold = text_threshold(gray);

    PIX* bin = pixThresholdToBinary(gray, layout.threshold);
    if (!bin)
        return layout;
    BOXA* boxa = pixConnCompBB(bin, 8);
    pixDestroy(&bin);
    if (!boxa)
        return layout;

    std::vector<std::pair<int, int>> spans;  // y, h
    for (int i = 0; i < boxaGetCount(boxa); ++i)
    {
        l_int32 x = 0, y = 0, w = 0, h = 0;
        boxaGetBoxGeometry(boxa, i, &x, &y, &w, &h);
        // Specks, and rules or underlines
        if (h < 3 || w > h * 8)
            continue;
        spans.emplace_back(y, h);
    }
    boxaDestroy(&boxa);
    if (spans.empty())
        return layout;

    std::vector<int> heights;
    heights.reserve(spans.size());
    for (const auto& [y, h] : spans)
        heights.push_back(h);
    std::ranges::nth_element(heights, heights.begin() + static_cast<ptrdiff_t>(heights.size() / 2));
    layout.char_height = heights[heights.size() / 2];

    // Lines: overlapping vertical spans of the glyphs, leaving out punctuation
    // and accents, and frames or table grids that would join every line
    std::ranges::sort(spans);
    int line_end = -1;
    for (const auto& [y, h] : spans)
    {
        if (h * 2 < layout.char_height || h > layout.char_height * 3)
            continue;
        if (y >= line_end)
            ++layout.lines;
        line_end = std::max(line_end, y + h);
    }

    return layout;
}

// What normalize_for_ocr() did to the image, to map Tesseract's boxes back onto it
struct ocr_transform_t
{
    float scale = 1.0f;
    int   pad   = 0;
    float angle = 0.0f;  // radians, clockwise, about the center of the padded image
};

// Scale so the glyphs are about the size Tesseract reads best, pad the edges,
// and straighten a skewed scan
static PIX* normalize_for_ocr(PIX* gray, const text_layout_t& layout, ocr_transform_t& xform)
{
    // Median glyph height (mostly x-height and capitals) Tesseract reads best, from the test set
    constexpr int target = 24;

    PixPtr out(pixClone(gray));
    if (layout.char_height > 0)
    {
        const float max_px = 40e6f;  // keep a 4K screenshot with small text from exploding
        float       scale  = std::clamp(float(target) / float(layout.char_height), 1.0f, 4.0f);
        scale = std::min(scale, std::sqrt(max_px / (float(pixGetWidth(gray)) * float(pixGetHeight(gray)))));
        scale = std::round(scale * 4) / 4;  // same scale for the same text size, wherever it's cropped
        if (scale > 1.0f)
        {
            if (PIX* scaled = pixScaleGrayLI(gray, scale, scale))
            {
                out.reset(scaled);
                xform.scale = scale;
            }
        }
    }

    // Text touching the edge of a tight selection is often dropped
    if (PIX* padded = pixAddBorder(out.get(), target, 255))
    {
        out.reset(padded);
        xform.pad = target;
    }

    PIX*      bin   = pixThresholdToBinary(out.get(), layout.threshold);
    l_float32 angle = 0.f, conf = 0.f;
    if (bin && pixFindSkew(bin, &angle, &conf) == 0 && std::abs(angle) > 0.4f && conf > 1.5f)
    {
        // The angle to rotate by, as in pixDeskew()
        const l_float32 rad = angle * (std::numbers::pi_v<l_float32> / 180.f);
        if (PIX* rotated = pixRotate(out.get(), rad, L_ROTATE_AREA_MAP, L_BRING_IN_WHITE, 0, 0))
        {
            out.reset(rotated);
            xform.angle = rad;
        }
    }
    if (bin)
        pixDestroy(&bin);

    // No binarization: Tesseract's LSTM engine does its own, and keeping the
    // gray levels helps stroke detail, especially for CJK scripts
    return out.release();
}

static tesseract::PageSegMode choose_psm(const text_layout_t& layout)
{
    using namespace tesseract;

    if (g_config->Runtime.preferred_psm != 0)
        return PageSegMode(g_config->Runtime.preferred_psm);

    // A word, a number or a line: the page layout analysis tends to drop text this short
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

OcrAPI::~OcrAPI()
{
    End();
}

void OcrAPI::End()
{
    if (m_api && m_initialized)
        m_api->End();
    m_initialized = false;
    m_config.reset();
}

Result<> OcrAPI::Configure(const char* data_path, const char* model, tesseract::OcrEngineMode oem)
{
    ocr_config_t next{ .path = data_path, .model = model };

    if (m_config && *m_config == next)
        return Ok();  // nothing to do

    // Tear down old engine
    if (m_initialized)
    {
        m_api->End();
        m_initialized = false;
    }

    if (m_api->Init(data_path, model, oem) != 0)
        return Err("Failed to Init OCR engine");

    // Otherwise Tesseract puts a space between every CJK character
    m_api->SetVariable("preserve_interword_spaces", is_cjk_model(model) ? "1" : "0");

    m_config      = std::move(next);
    m_initialized = true;
    return Ok();
}

// A box Tesseract found in the image normalize_for_ocr() made, in the pixels
// of the image it was made from: rotated back, unpadded, unscaled and clamped
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
        // pixRotate() turned the image clockwise by angle: turn the corner back
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
    return ocr_word_t{ .x = min_x, .y = min_y, .w = max_x - min_x, .h = max_y - min_y };
}

// From "  hello world  \n  " to "hello world"
static void trim(std::string& s)
{
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::ranges::find_if(s, not_ws));
    s.erase(std::ranges::find_if(std::views::reverse(s), not_ws).base(), s.end());
}

// Read a grayscale image made from the capture with Tesseract.
// Returns an empty result (no text, confidence 0) if nothing was recognized
static Result<ocr_result_t> recognize(tesseract::TessBaseAPI& api,
                                      PIX*                    gray,
                                      const text_layout_t&    layout,
                                      float                   src_w,
                                      float                   src_h)
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

    api.SetPageSegMode(psm);
    api.SetImage(pix.get());
    api.SetSourceResolution(effective_dpi);

    // Make OCR + confidence deterministic
    if (api.Recognize(nullptr) != 0)
        return Err("tesseract::Recognize() failed");

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

    // The background window must be a few glyphs wide, which we only know
    // once we've seen the text: measure it, then redo it for big text
    constexpr int default_window = 64;
    PixPtr        gray(distance_from_background(px, colors, cap.w, cap.h, default_window));
    if (!gray)
        return Err("Failed to convert image into Pix format");

    text_layout_t layout = measure_text(gray.get());
    if (layout.char_height * 3 > default_window)
    {
        gray.reset(distance_from_background(px, colors, cap.w, cap.h, layout.char_height * 3));
        if (!gray)
            return Err("Failed to convert image into Pix format");
        layout = measure_text(gray.get());
    }

    Result<ocr_result_t> first = recognize(*m_api, gray.get(), layout, float(cap.w), float(cap.h));
    if (!first)
        return first;
    ocr_result_t best = std::move(first.get());

    // Text on a busy or graded background, which has no color of its own to
    // measure from: try each main color as the text color instead, and keep
    // whichever reading Tesseract is the most sure of. Clean text, even small
    // or degraded, is read at 85% or more and never gets here
    constexpr int    retry_below = 85;
    constexpr size_t max_tries   = 3;
    PixPtr           best_text_gray;
    text_layout_t    best_text_layout;
    for (size_t i = 0; i < std::min(colors.size(), max_tries) && best.confidence < retry_below; ++i)
    {
        PixPtr text_gray(distance_from_text_color(px, colors[i], cap.w, cap.h));
        if (!text_gray)
            continue;

        const text_layout_t  text_layout = measure_text(text_gray.get());
        Result<ocr_result_t> res         = recognize(*m_api, text_gray.get(), text_layout, float(cap.w), float(cap.h));
        if (res && res.get().confidence > best.confidence)
        {
            best             = std::move(res.get());
            best_text_gray   = std::move(text_gray);
            best_text_layout = text_layout;
        }
    }

    // The best text color may only be the outline around the letters
    if (best_text_gray && best.confidence < retry_below)
    {
        if (PixPtr filled{ fill_outlined_text(best_text_gray.get(), best_text_layout.threshold) })
        {
            Result<ocr_result_t> res =
                recognize(*m_api, filled.get(), measure_text(filled.get()), float(cap.w), float(cap.h));
            if (res && res.get().confidence > best.confidence)
                best = std::move(res.get());
        }
    }

    if (best.data.empty())
        return Err(fmt::format("No text recognized (PSM: {}, {}x{} px)", best.psm_str, cap.w, cap.h));

    return Ok(std::move(best));
}

// ---------------
// Zbar
// ---------------
ZbarAPI::ZbarAPI()
{
    SetConfig(zbar::ZBAR_NONE, true);  // enable all
}

// Codes found in a grayscale image, added to `ret.datas` unless already there.
// `scale`: how much bigger `gray` is than the scanned image
static void scan_gray(zbar::ImageScanner&   scanner,
                      std::vector<uint8_t>& gray,
                      int                   w,
                      int                   h,
                      int                   scale,
                      zbar_result_t&        ret)
{
    // ZBar doesn't take ownership of the buffer
    zbar::Image image(w, h, "Y800", gray.data(), gray.size());
    if (scanner.scan(image) <= 0)
        return;

    for (auto sym = image.symbol_begin(); sym != image.symbol_end(); ++sym)
    {
        std::string data = sym->get_data();

        barcode_t& code = ret.codes.emplace_back(barcode_t{ .type = sym->get_type_name(), .data = data });
        if (const int n = sym->get_location_size(); n > 0)
        {
            int min_x = w, min_y = h, max_x = 0, max_y = 0;
            for (int i = 0; i < n; ++i)
            {
                min_x = std::min(min_x, sym->get_location_x(i));
                min_y = std::min(min_y, sym->get_location_y(i));
                max_x = std::max(max_x, sym->get_location_x(i));
                max_y = std::max(max_y, sym->get_location_y(i));
            }
            code.x = float(min_x) / float(scale);
            code.y = float(min_y) / float(scale);
            code.w = float(max_x - min_x) / float(scale);
            code.h = float(max_y - min_y) / float(scale);
        }

        if (std::ranges::find(ret.datas, data) != ret.datas.end())
            continue;
        ret.datas.push_back(std::move(data));
        ret.symbologies[sym->get_type_name()]++;
    }
}

Result<zbar_result_t> ZbarAPI::ExtractTextsCapture(const capture_result_t& cap)
{
    if (cap.view().empty() || cap.w <= 0 || cap.h <= 0)
        return Err("Image is empty");

    zbar_result_t        ret;
    std::vector<uint8_t> gray(size_t(cap.w) * cap.h);

    rgba_to_grayscale(cap.view().data(), gray.data(), cap.w, cap.h);
    scan_gray(m_scanner, gray, cap.w, cap.h, 1, ret);

    // Light codes on a dark background (dark mode, inverted codes)
    if (ret.datas.empty())
    {
        std::ranges::transform(gray, gray.begin(), [](uint8_t v) { return uint8_t(255 - v); });
        scan_gray(m_scanner, gray, cap.w, cap.h, 1, ret);
        std::ranges::transform(gray, gray.begin(), [](uint8_t v) { return uint8_t(255 - v); });
    }

    // Bars or modules under ~2px, both polarities
    if (ret.datas.empty() && std::max(cap.w, cap.h) <= 2000)
    {
        const int            w2 = cap.w * 2, h2 = cap.h * 2;
        std::vector<uint8_t> big(size_t(w2) * h2);
        for (int y = 0; y < h2; ++y)
            for (int x = 0; x < w2; ++x)
                big[(size_t(y) * w2) + x] = gray[(size_t(y / 2) * cap.w) + (x / 2)];

        scan_gray(m_scanner, big, w2, h2, 2, ret);
        if (ret.datas.empty())
        {
            std::ranges::transform(big, big.begin(), [](uint8_t v) { return uint8_t(255 - v); });
            scan_gray(m_scanner, big, w2, h2, 2, ret);
        }
    }

    if (ret.datas.empty())
        return Err("No QR code or barcode found");

    return Ok(std::move(ret));
}

bool ZbarAPI::SetConfig(zbar::zbar_symbol_type_e zbar_code, int enable)
{
    return m_scanner.set_config(zbar_code, zbar::ZBAR_CFG_ENABLE, enable) == 0;
}
