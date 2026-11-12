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

#include <zbar.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "screen_capture.hpp"
#include "text_extraction.hpp"
#include "util/image.hpp"
#include "util/result.hpp"

// ------------------------------
// Barcodes (ZBar)
// ------------------------------

struct BarcodeAPI::scanner_t
{
    zbar::ImageScanner scanner;
};

BarcodeAPI::BarcodeAPI() : m_scanner(std::make_unique<scanner_t>())
{
    m_scanner->scanner.set_config(zbar::ZBAR_NONE, zbar::ZBAR_CFG_ENABLE, 1);  // enable all
}

BarcodeAPI::~BarcodeAPI() = default;

// Codes found in a grayscale image, added to `ret.datas` unless already there.
// `scale`: how much bigger `gray` is than the scanned image
static void scan_gray(zbar::ImageScanner&   scanner,
                      std::vector<uint8_t>& gray,
                      int                   w,
                      int                   h,
                      int                   scale,
                      barcode_result_t&     ret)
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

Result<barcode_result_t> BarcodeAPI::ExtractTextsCapture(const capture_result_t& cap)
{
    if (cap.view().empty() || cap.w <= 0 || cap.h <= 0)
        return Err("Image is empty");

    barcode_result_t     ret;
    std::vector<uint8_t> gray(size_t(cap.w) * cap.h);

    rgba_to_grayscale(cap.view().data(), gray.data(), cap.w, cap.h);
    scan_gray(m_scanner->scanner, gray, cap.w, cap.h, 1, ret);

    // Light codes on a dark background (dark mode, inverted codes)
    if (ret.datas.empty())
    {
        std::ranges::transform(gray, gray.begin(), [](uint8_t v) { return uint8_t(255 - v); });
        scan_gray(m_scanner->scanner, gray, cap.w, cap.h, 1, ret);
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

        scan_gray(m_scanner->scanner, big, w2, h2, 2, ret);
        if (ret.datas.empty())
        {
            std::ranges::transform(big, big.begin(), [](uint8_t v) { return uint8_t(255 - v); });
            scan_gray(m_scanner->scanner, big, w2, h2, 2, ret);
        }
    }

    if (ret.datas.empty())
        return Err("No QR code or barcode found");

    return Ok(std::move(ret));
}
