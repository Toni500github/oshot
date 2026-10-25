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

#include <tesseract/ocrclass.h>
#include <tesseract/publictypes.h>
#include <zbar.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <numbers>
#include <numeric>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

#include "config.hpp"
#include "screen_capture.hpp"
#include "util.hpp"

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

// Color distance, rounded and capped at 255. From a table, as it's per pixel
static int color_dist(const rgb_t& a, const rgb_t& b)
{
    // Past 255.5^2 the rounded distance is over 255
    constexpr int     max_d2 = 65280;
    static const auto table  = [] {
        std::array<uint8_t, max_d2 + 1> t{};
        for (int d2 = 0; d2 <= max_d2; ++d2)
            t[d2] = uint8_t(std::lround(std::sqrt(float(d2))));
        return t;
    }();
    const int d2 = color_dist2(a, b);
    return d2 <= max_d2 ? table[d2] : 255;
}

// The image's main colors, most common first. These are the most populated bins
// of a 4 bits per channel histogram, each averaged to its real color
static std::vector<rgb_t> main_colors(const std::vector<rgb_t>& px)
{
    constexpr size_t max_colors = 8;
    const size_t     step       = std::max<size_t>(1, px.size() / 1'000'000);  // sampling is plenty

    const auto bin = [](const rgb_t& c) { return ((c[0] >> 4) << 8) | ((c[1] >> 4) << 4) | (c[2] >> 4); };

    std::vector<uint32_t>                hist(4096);
    std::vector<std::array<uint64_t, 3>> sums(hist.size());
    size_t                               samples = 0;
    for (size_t i = 0; i < px.size(); i += step, ++samples)
    {
        const int b = bin(px[i]);
        ++hist[b];
        for (size_t c = 0; c < 3; ++c)
            sums[b][c] += px[i][c];
    }

    std::vector<int> bins(hist.size());
    std::iota(bins.begin(), bins.end(), 0);
    std::ranges::partial_sort(bins, bins.begin() + max_colors, [&](int a, int b) { return hist[a] > hist[b]; });

    std::vector<rgb_t> colors;
    for (size_t k = 0; k < max_colors; ++k)
    {
        // Text and antialiasing, not an area
        if (k > 0 && hist[bins[k]] * 200 < samples)
            break;

        const auto& sum   = sums[bins[k]];
        const auto  count = hist[bins[k]];
        colors.push_back({ uint8_t(sum[0] / count), uint8_t(sum[1] / count), uint8_t(sum[2] / count) });
    }

    return colors;
}

// Grayscale where each pixel is its color distance from the background around
// it. That gives dark text on white for any pair of colors (white on blue, red on
// black, black on yellow...), in every panel of a screenshot that mixes light,
// dark and colored areas, without guessing when to invert.
// The local background is whichever main color covers the most of a `window`
// px square around each 4x4 cell, because text covers less of it than its background,
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
    // quarter that size wins (text strokes never fill that much of it)
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
            SET_DATA_BYTE(line, x, 255 - color_dist(px[(size_t(y) * w) + x], bg));
        }
    }

    return pix;
}

// Grayscale where each pixel is its color distance from `text`, so that color
// is black and anything far from it white. For text that has no background
// color of its own, like outlined text on a game scene, a meme or a gradient.
// `bg` is the color around the text, which turns white even when it's close to
// the text color (dark gray text on a dark striped panel).
// Returns nullptr if `text` covers most of the image, as it's the background
static PIX* distance_from_text_color(const std::vector<rgb_t>& px, const rgb_t& text, const rgb_t& bg, int w, int h)
{
    PIX* pix = pixCreate(w, h, 8);
    if (!pix)
        return nullptr;

    // Antialiasing halfway between the text and a black outline is still text.
    // Low contrast is stretched up to 8x, more would only raise the noise
    const float              contrast = std::sqrt(float(color_dist2(text, bg)));
    const float              gain     = std::clamp(255.0f / std::max(contrast, 1.0f), 2.0f, 8.0f);
    std::array<uint8_t, 256> stretched{};
    for (int d = 0; d < 256; ++d)
        stretched[d] = uint8_t(std::min(int(std::lround(float(d) * gain)), 255));

    size_t    dark = 0;
    l_uint32* dst  = pixGetData(pix);
    const int wpl  = pixGetWpl(pix);
    for (int y = 0; y < h; ++y)
    {
        l_uint32* line = dst + static_cast<ptrdiff_t>(y) * wpl;
        for (int x = 0; x < w; ++x)
        {
            const uint8_t d = stretched[color_dist(px[(size_t(y) * w) + x], text)];
            SET_DATA_BYTE(line, x, d);
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
    std::array<std::array<uint32_t, 256>, 4> counts{};
    const l_uint32*                          data = pixGetData(gray);
    const int                                wpl  = pixGetWpl(gray);
    const int                                w = pixGetWidth(gray), h = pixGetHeight(gray);
    for (int y = 0; y < h; ++y)
    {
        const l_uint32* line = data + static_cast<ptrdiff_t>(y) * wpl;
        int             x    = 0;
        for (; x + 4 <= w; x += 4)
        {
            const l_uint32 word = line[x / 4];
            ++counts[0][word & 0xff];
            ++counts[1][(word >> 8) & 0xff];
            ++counts[2][(word >> 16) & 0xff];
            ++counts[3][word >> 24];
        }
        for (; x < w; ++x)
            ++counts[0][GET_DATA_BYTE(line, x)];
    }
    std::array<double, 256> hist{};
    for (size_t i = 0; i < hist.size(); ++i)
        hist[i] = double(counts[0][i]) + counts[1][i] + counts[2][i] + counts[3][i];

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
// picture look when read through the border's color. So fill the inside of each
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
    PIXA* pixa = nullptr;
    BOXA* boxa = pixConnComp(bin, &pixa, 8);
    pixDestroy(&bin);
    if (!boxa)
    {
        pixaDestroy(&pixa);
        return layout;
    }

    std::vector<std::pair<int, int>> spans;  // y, h
    std::vector<std::pair<int, int>> sizes;  // h, box area
    for (int i = 0; i < boxaGetCount(boxa); ++i)
    {
        l_int32 x = 0, y = 0, w = 0, h = 0;
        boxaGetBoxGeometry(boxa, i, &x, &y, &w, &h);
        // Specks, and rules or underlines
        if (h < 3 || w > h * 8)
            continue;
        spans.emplace_back(y, h);

        // Frames and table grids are mostly empty, glyphs (even outlined ones) aren't
        l_int32 ink = 0;
        if (PIX* comp = pixaGetPix(pixa, i, L_CLONE))
        {
            pixCountPixels(comp, &ink, nullptr);
            pixDestroy(&comp);
        }
        if (ink * 100 >= w * h * 15)
            sizes.emplace_back(h, w * h);
    }
    boxaDestroy(&boxa);
    pixaDestroy(&pixa);
    if (spans.empty())
        return layout;

    std::vector<int> heights;
    heights.reserve(spans.size());
    for (const auto& [y, h] : spans)
        heights.push_back(h);
    std::ranges::nth_element(heights, heights.begin() + static_cast<ptrdiff_t>(heights.size() / 2));
    layout.char_height = heights[heights.size() / 2];

    // The height half the glyphs' area is under. Far above the median, the
    // blobs are mostly specks of a drawing or photo around the text, which
    // would have it scaled up 4x for nothing
    std::ranges::sort(sizes);
    const int64_t total = std::accumulate(sizes.begin(), sizes.end(), int64_t(0),
                                          [](int64_t sum, const auto& s) { return sum + s.second; });
    int64_t       area  = 0;
    for (const auto& [h, a] : sizes)
    {
        area += a;
        if (area * 2 >= total)
        {
            if (layout.char_height * 4 < h)
                layout.char_height = h;
            break;
        }
    }

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
        return Ok();  // nothing to do

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
// Returns an empty result (no text, confidence 0) if nothing was recognized.
// `monitor` can cancel it between text lines
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

    // Make OCR + confidence deterministic
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
    int                     confidence = 0;  // mean over its words
    bool                    has_text   = false;  // see merge_lines()
};

// Add the lines of `r` to `lines`, moved by (dx, dy). `sep` goes between
// words if their line's text can't be taken from Tesseract's
static void add_lines(
    std::vector<ocr_line_t>& lines, const ocr_result_t& r, std::string_view sep, float dx = 0, float dy = 0)
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

// One reading made of the best reading of each line that any pass found.
// Lines are taken from the most read text down, leaving out any that covers
// most of a word of one already taken. A line also needs a mean
// confidence of sure_word or more and a word with 2 letters or digits read at
// that much, or a single one read as surely as clean text (85%). Otherwise
// scraps of the background read as symbols or single characters ("| |",
// "3 N { <", at 60-80%) would get in.
// They're put in the order of the lines they cover in `order`, the best whole
// reading, as Tesseract's page layout analysis knows about columns and tables
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

// Boxes around the text lines of a grayscale image, from its glyphs. Unlike
// Tesseract's page layout analysis, a big title above small captions or scraps
// of a drawing next to a line don't join lines together.
// Blobs of a similar height whose middles fall in the same band make a row,
// split where a gap is wider than a few glyphs (columns). A line needs at
// least 3 glyphs, fewer are usually scraps of the background
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
        auto      row = std::ranges::find_if(rows, [&](const row_t& r) {
            return mid >= r.top && mid <= r.bottom && g.h * 2 >= r.h && g.h <= r.h * 2;
        });
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
    return { PixPtr(gray), gray ? measure_text(gray) : text_layout_t{} };
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
        crops.push_back({ b, std::move(crop) });
    }

    return crops;
}

// Runs reads on a few Tesseract engines at once, for the passes of a busy
// background, which don't depend on each other. The engine already loaded
// reads on the caller's thread, in Take(). The others load the same model in
// their own thread as the pool starts, and take reads in order as soon as
// they're free
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

    size_t Size() const { return m_threads.size() + 1; }

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

    // Drop the reads not started yet and stop the others at their next text
    // line, as their results are no longer wanted. Doesn't wait for them, as
    // they don't use the caller's engine
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
        } cancel{ m_generation, job.generation };

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

// Passes with a main color as the text color, for text on a busy or graded
// background, which has no color of its own to measure from.
// Text stands out from the most common color more than the other shades of
// the background do (the stripes of a patterned panel can take the first
// few places by count), so the main colors are tried by how far they are
// from it, skipping the ones that cover most of the image
static std::vector<ocr_pass_t> text_color_passes(const std::vector<rgb_t>& px,
                                                 const std::vector<rgb_t>& colors,
                                                 int                       w,
                                                 int                       h)
{
    constexpr size_t max_tries = 3;

    std::vector<size_t> order(colors.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::stable_sort(order, std::ranges::greater{},
                             [&](size_t i) { return color_dist2(colors[i], colors[0]); });

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

// Read text the first pass was unsure of, improving `best`. With spare
// engines, the passes that may be needed next are read while waiting for the
// one that decides it. Either way, only the results that reading them one by
// one would use are taken, so the text doesn't depend on the engine count
static void read_busy_text(OcrEnginePool&            engines,
                           const std::vector<rgb_t>& px,
                           const std::vector<rgb_t>& colors,
                           int                       w,
                           int                       h,
                           std::string_view          sep,
                           ocr_result_t&             best)
{
    using read_t = OcrEnginePool::read_t;

    const float             src_w = float(w), src_h = float(h);
    const bool              speculate = engines.Size() > 1;
    std::vector<ocr_line_t> lines;  // every line any pass read, to merge at the end
    add_lines(lines, best, sep);

    // Each main color as the text color, keeping the best reading
    const std::vector<ocr_pass_t>        candidates = text_color_passes(px, colors, w, h);
    std::vector<OcrEnginePool::ticket_t> reads;
    for (const ocr_pass_t& pass : candidates)
        reads.push_back(engines.Read(pass, src_w, src_h));

    // Colors read at 50% or more are text (stripes or a background shade read
    // at ~0), kept to combine below
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
            filled_t{ from, make_pass(fill_outlined_text(from->gray.get(), from->layout.threshold)) });
        if (f.pass.gray)
            f.read = engines.Read(f.pass, src_w, src_h);
        return f;
    };

    // Parts of the text that need different passes (an outlined title over a
    // caption), or lines the page layout analysis joins with the background.
    // The passes that kept only the text are also read line by line, and the
    // best reading of each line is kept. A line cut the same out of two
    // passes is read once
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
