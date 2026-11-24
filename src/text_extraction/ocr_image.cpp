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

#include "ocr_image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <vector>

// ------------------------------
// OCR image preparation
// ------------------------------

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
std::vector<rgb_t> main_colors(const std::vector<rgb_t>& px)
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
        if (k > 0 && hist[bins[k]] * 200ULL < samples)
            break;

        const auto& sum   = sums[bins[k]];
        const auto  count = hist[bins[k]];
        colors.push_back({ uint8_t(sum[0] / count), uint8_t(sum[1] / count), uint8_t(sum[2] / count) });
    }

    return colors;
}

// Grayscale where each pixel is its color distance from the background around it, so text comes out dark on white
// whatever its colors, without guessing when to invert. The background of each 4x4 cell is the main color that
// covers most of a `window` px square around it (text always covers less of it than its background)
PIX* distance_from_background(const std::vector<rgb_t>& px, const std::vector<rgb_t>& colors, int w, int h, int window)
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

// Grayscale where each pixel is its color distance from `text`, for text with no background color of its own
// (outlined text on a game scene or a gradient). `bg`, the color around the text, turns white even when it's close
// to the text color. Returns nullptr if `text` covers most of the image, since it's the background then
PIX* distance_from_text_color(const std::vector<rgb_t>& px, const rgb_t& text, const rgb_t& bg, int w, int h)
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

// Fill the inside of letters drawn as outlines (white letters with a black border, read through the border's
// color). The inside is what lies within an odd number of outlines, so an 'O' keeps its hole
PIX* fill_outlined_text(PIX* gray, int threshold)
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

// Glyph size and line count from the connected components of the text pixels
text_layout_t measure_text(PIX* gray)
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

    // The height half the glyphs' area is under. When it's far above the median, most blobs are specks of a
    // drawing or photo, and the median would scale the text up 4x for nothing
    std::ranges::sort(sizes);
    const int64_t total = std::accumulate(
        sizes.begin(), sizes.end(), int64_t(0), [](int64_t sum, const auto& s) { return sum + s.second; });
    int64_t area = 0;
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

// Scale so the glyphs are about the size Tesseract reads best, pad the edges,
// and straighten a skewed scan
PIX* normalize_for_ocr(PIX* gray, const text_layout_t& layout, ocr_transform_t& xform)
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

    // No binarization, since Tesseract's LSTM engine does its own and the gray levels keep stroke detail (CJK most)
    return out.release();
}
