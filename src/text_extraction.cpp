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
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
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

static tesseract::PageSegMode choose_psm(int w, int h)
{
    using namespace tesseract;

    if (g_config->Runtime.preferred_psm != 0)
        return PageSegMode(g_config->Runtime.preferred_psm);

    const size_t area   = size_t(w) * h;
    const float  aspect = (h > 0) ? float(w) / float(h) : 1.0f;

    spdlog::debug("Choosing PSM, area: {}*{}={}, aspect: {}", w, h, area, aspect);

    // Single character: tiny and roughly square
    if (area < 2'500 && aspect > 0.3f && aspect < 3.0f)
        return PSM_SINGLE_CHAR;

    // Single word: small area, not excessively wide
    if (area < 15'000 && aspect > 0.3f && aspect < 5.0f)
        return PSM_SINGLE_WORD;

    // AUTO_OSD is good enough to take care of the rest.
    return PSM_AUTO_OSD;
}

static PIX* preprocess_pix(PIX* src)
{
    // Remove alpha and convert to grayscale
    // pixConvertRGBToGray doesn't handle 32bpp RGBA correctly
    PIX* no_alpha = pixRemoveAlpha(src);  // returns new 24bpp RGB Pix
    if (!no_alpha)
        no_alpha = pixClone(src);

    // Quick dark-bg detection on the RGB image
    PIX*      gray_probe = pixConvertRGBToGray(no_alpha, 0.299f, 0.587f, 0.114f);
    l_float32 mean_val   = 128.f;
    if (gray_probe)
    {
        pixGetAverageMasked(gray_probe, nullptr, 0, 0, 1, L_MEAN_ABSVAL, &mean_val);
        pixDestroy(&gray_probe);
    }

    PIX* gray    = nullptr;
    bool dark_bg = (mean_val < 128.f);

    if (dark_bg)
    {
        // Max-channel: preserves colored text (red, green, cyan) on dark BG.
        // Luma weights would map red(200,50,50) -> ~95, almost invisible after invert.
        // Max-channel maps it -> 200, giving full contrast after invert.
        gray = pixConvertRGBToGrayMinMax(no_alpha, L_CHOOSE_MAX);
    }
    else
    {
        gray = pixConvertRGBToGray(no_alpha, 0.299f, 0.587f, 0.114f);
    }

    pixDestroy(&no_alpha);
    if (!gray)
        return pixClone(src);

    // Invert dark backgrounds
    if (dark_bg)
    {
        PIX* inverted = pixInvert(nullptr, gray);
        if (inverted)
        {
            pixDestroy(&gray);
            gray = inverted;
        }
    }

    // Upscale tiny text before binarization (helps for <12px font sizes)
    if (pixGetHeight(gray) < 200)
    {
        PIX* scaled = pixScale(gray, 2.0f, 2.0f);
        if (scaled)
        {
            pixDestroy(&gray);
            gray = scaled;
        }
    }

    // Deskew
    PIX*      bin_for_skew = pixThresholdToBinary(gray, 128);
    l_float32 angle = 0.f, conf = 0.f;
    PIX*      result_gray = gray;

    if (bin_for_skew && pixFindSkew(bin_for_skew, &angle, &conf) == 0 && std::abs(angle) > 0.4f && conf > 1.5f)
    {
        l_float32 rad     = -angle * (std::numbers::pi_v<l_float32> / 180.f);
        PIX*      rotated = pixRotate(gray, rad, L_ROTATE_AREA_MAP, L_BRING_IN_WHITE, 0, 0);
        if (rotated)
        {
            pixDestroy(&gray);
            result_gray = rotated;
        }
    }
    if (bin_for_skew)
        pixDestroy(&bin_for_skew);

    // Let Tesseract's LSTM engine do its own internal binarization.
    // Pre-binarizing (e.g. Sauvola) strips gradient information that LSTM uses for
    // stroke-width estimation. This is especially damaging for CJK scripts where
    // fine stroke detail distinguishes many characters. The CLI tool works because
    // it passes the raw image; we must do the same.
    return result_gray;
}

static Pix* rgba_to_pix(std::span<const uint8_t> rgba, int w, int h)
{
    const size_t required = size_t(w) * h * 4;
    if (rgba.size() < required)
        return nullptr;

    PIX* pix = pixCreate(w, h, 32);
    if (!pix)
        return nullptr;

    const uint8_t* src    = rgba.data();
    uint32_t*      dst    = pixGetData(pix);
    const int      stride = pixGetWpl(pix);

    for (int y = 0; y < h; ++y)
    {
        uint32_t* row = dst + static_cast<ptrdiff_t>(y * stride);
        for (int x = 0; x < w; ++x)
        {
            const uint8_t* p    = src + (size_t(y) * w + x) * 4;
            rgba_t         byte = load_rgba(p);
            // Leptonica 32bpp word layout (big-endian word): R G B A
            SET_DATA_FOUR_BYTES(row, x, byte.to_rgba());
        }
    }

    return pix;
}

OcrAPI::~OcrAPI()
{
    if (m_api && m_initialized)
        m_api->End();
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

    m_config      = std::move(next);
    m_initialized = true;
    return Ok();
}

// From "  hello world  \n  " to "hello world"
static void trim(std::string& s)
{
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::ranges::find_if(s, not_ws));
    s.erase(std::ranges::find_if(std::views::reverse(s), not_ws).base(), s.end());
}

Result<ocr_result_t> OcrAPI::ExtractTextCapture(const capture_result_t& cap)
{
    ocr_result_t ret;

    if (!m_initialized)
        return Err("Initialize the engine first");

    if (cap.view().empty() || cap.w <= 0 || cap.h <= 0)
        return Err("Image is empty");

    const size_t required = size_t(cap.w) * cap.h * 4;
    if (cap.view().size() < required)
        return Err(fmt::format(
            "Image buffer too small: got {} bytes, need {} ({}x{} × 4)", cap.view().size(), required, cap.w, cap.h));

    PixPtr raw_pix(rgba_to_pix(cap.view(), cap.w, cap.h));
    if (!raw_pix)
        return Err("Failed to convert image into Pix format");

    // Preprocess: dark-bg inversion, upscale, deskew.
    // Returns grayscale so Tesseract's LSTM engine retains stroke-gradient info.
    PixPtr pix(preprocess_pix(raw_pix.get()));
    if (!pix)
        return Err("Failed to preprocess image");

    // Use the binarized pix dimensions for PSM (they may differ after deskew rotation)
    const int proc_w = pixGetWidth(pix.get());
    const int proc_h = pixGetHeight(pix.get());

    tesseract::PageSegMode psm = choose_psm(proc_w, proc_h);

    float scale         = std::min(float(g_scr_w) / float(cap.w), float(g_scr_h) / float(cap.h));
    int   effective_dpi = int(float(get_screen_dpi()) * scale);
    effective_dpi       = std::clamp(effective_dpi, 150, 300);

    m_api->SetPageSegMode(psm);
    m_api->SetImage(pix.get());
    m_api->SetSourceResolution(effective_dpi);

    // Make OCR + confidence deterministic
    if (m_api->Recognize(nullptr) != 0)
        return Err("tesseract::Recognize() failed");

    TextPtr text(m_api->GetUTF8Text(), [](char* p) { delete[] p; });
    if (!text)
        return Err("Failed to get recognized text");

    std::string data(text.get());
    trim(data);
    if (data.empty())
        return Err(fmt::format(
            "No text recognized (PSM: {}, {}x{} px, DPI: {})", psm_to_str(psm), proc_w, proc_h, effective_dpi));

    ret.data    = std::move(data);
    ret.psm_str = psm_to_str(psm);
    ret.psm     = psm;

    if (tesseract::ResultIterator* ri = m_api->GetIterator())
    {
        double sum   = 0.0;
        int    count = 0;
        do
        {
            float conf = ri->Confidence(tesseract::RIL_WORD);
            if (conf >= 0.0f)
            {
                sum += conf;
                ++count;
            }
        } while (ri->Next(tesseract::RIL_WORD));

        ret.confidence = count ? int(std::round(sum / count)) : 0;
        delete ri;
    }
    else
    {
        ret.confidence = m_api->MeanTextConf();
    }

    return Ok(std::move(ret));
}

// ---------------
// Zbar
// ---------------
ZbarAPI::ZbarAPI()
{
    SetConfig(zbar::ZBAR_NONE, true);  // enable all
}

Result<zbar_result_t> ZbarAPI::ExtractTextsCapture(const capture_result_t& cap)
{
    zbar_result_t        ret;
    std::vector<uint8_t> gray(size_t(cap.w) * cap.h);

    rgba_to_grayscale(cap.view().data(), gray.data(), cap.w, cap.h);

    zbar::Image image(cap.w,
                      cap.h,
                      "Y800",  // GRAYSCALE
                      gray.data(),
                      gray.size());

    if (m_scanner.scan(image) <= 0)
        return Err("Failed to scan image");

    for (auto sym = image.symbol_begin(); sym != image.symbol_end(); ++sym)
    {
        ret.datas.push_back(sym->get_data());
        ret.symbologies[sym->get_type_name()]++;
    }

    if (ret.datas.empty() || ret.symbologies.empty())
        return Err("Failed to decode barcode from image");

    // Prevent ZBar from freeing the buffer
    image.set_data(nullptr, 0);

    return Ok(std::move(ret));
}

bool ZbarAPI::SetConfig(zbar::zbar_symbol_type_e zbar_code, int enable)
{
    return m_scanner.set_config(zbar_code, zbar::ZBAR_CFG_ENABLE, enable) == 0;
}
