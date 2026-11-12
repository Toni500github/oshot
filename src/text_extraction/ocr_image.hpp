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

#ifndef _OCR_IMAGE_HPP_
#define _OCR_IMAGE_HPP_

// Preparing a capture for Tesseract, shared by the src/text_extraction/ OCR files

#include <leptonica/allheaders.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// ------------------------------
// Types
// ------------------------------

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

struct text_layout_t
{
    int char_height = 0;  // median height of the glyphs, 0 if no text was found
    int lines       = 0;
    int threshold   = 128;
};

// What normalize_for_ocr() did to the image, to map Tesseract's boxes back onto it
struct ocr_transform_t
{
    float scale = 1.0f;
    int   pad   = 0;
    float angle = 0.0f;  // radians, clockwise, about the center of the padded image
};

// ------------------------------
// Pixels
// ------------------------------

// Composited over white, so the transparent parts of an opened image read as background
inline rgb_t opaque_rgb(const uint8_t* p)
{
    const unsigned a = p[3];
    if (a == 255)
        return { p[0], p[1], p[2] };

    const auto blend = [a](unsigned c) { return uint8_t(((c * a) + (255 * (255 - a)) + 127) / 255); };
    return { blend(p[0]), blend(p[1]), blend(p[2]) };
}

inline int color_dist2(const rgb_t& a, const rgb_t& b)
{
    const int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return (dr * dr) + (dg * dg) + (db * db);
}

// ------------------------------
// Images
// ------------------------------

/**
 * The image's main colors, most common first. These are the most populated bins
 * of a 4 bits per channel histogram, each averaged to its real color
 * @param px The image's pixels, opaque
 **/
std::vector<rgb_t> main_colors(const std::vector<rgb_t>& px);

/**
 * Grayscale where each pixel is its color distance from the background around it, so text comes out dark on
 * white whatever its colors. The background of each 4x4 cell is the main color that covers most of a `window`
 * px square around it
 * @param px The image's pixels, opaque
 * @param colors main_colors() of `px`
 * @param w The image width
 * @param h The image height
 * @param window The side of the square, in px
 * @return A new 8 bpp image, or nullptr
 **/
PIX* distance_from_background(const std::vector<rgb_t>& px, const std::vector<rgb_t>& colors, int w, int h, int window);

/**
 * Grayscale where each pixel is its color distance from `text`, for text with no background color of its own
 * (outlined text on a game scene or a gradient). `bg`, the color around the text, turns white
 * @param px The image's pixels, opaque
 * @param text The text color
 * @param bg The color around the text
 * @param w The image width
 * @param h The image height
 * @return A new 8 bpp image, or nullptr if `text` covers most of the image (it's the background then)
 **/
PIX* distance_from_text_color(const std::vector<rgb_t>& px, const rgb_t& text, const rgb_t& bg, int w, int h);

/**
 * Fill the inside of letters drawn as outlines (white letters with a black border, read through the border's
 * color). The inside is what lies within an odd number of outlines, so an 'O' keeps its hole
 * @param gray The 8 bpp image
 * @param threshold Darker than this is ink
 * @return A new image, or nullptr
 **/
PIX* fill_outlined_text(PIX* gray, int threshold);

/**
 * Glyph size and line count from the connected components of the text pixels
 * @param gray The 8 bpp image, dark text on white
 **/
text_layout_t measure_text(PIX* gray);

/**
 * Scale so the glyphs are about the size Tesseract reads best, pad the edges, and straighten a skewed scan
 * @param gray The 8 bpp image, dark text on white
 * @param layout measure_text() of `gray`
 * @param xform Set to what was done, to map Tesseract's boxes back
 * @return A new image
 **/
PIX* normalize_for_ocr(PIX* gray, const text_layout_t& layout, ocr_transform_t& xform);

#endif  // _OCR_IMAGE_HPP_
