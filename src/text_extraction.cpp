#include "text_extraction.hpp"

#include <zbar.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <utility>
#include <vector>

#include "config.hpp"
#include "screen_capture.hpp"
#include "util.hpp"

// OCR
static tesseract::PageSegMode choose_psm(int w, int h)
{
    if (g_config->Runtime.preferred_psm != 0)
        return static_cast<tesseract::PageSegMode>(g_config->Runtime.preferred_psm);

    const size_t area   = static_cast<size_t>(w) * h;
    const float  aspect = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;

    // Single character: tiny and roughly square
    if (area < 2'500 && aspect > 0.3f && aspect < 3.0f)
        return tesseract::PSM_SINGLE_CHAR;

    // Single word: small area, not excessively wide
    if (area < 15'000 && aspect < 5.0f)
        return tesseract::PSM_SINGLE_WORD;

    // Single line: wide-and-short, or very small height regardless of width.
    // Guard with absolute height: a wide multi-line block also has high aspect,
    // so aspect alone is not enough. A single text line is never taller than ~80px
    // at normal DPI; after the 2x upscale in preprocess_pix that becomes ~160px.
    if (aspect > 4.0f && h < w / 4 && h < 160)
        return tesseract::PSM_SINGLE_LINE;

    // Vertical text: tall and narrow (e.g. rotated sidebar labels)
    if (aspect < 0.25f)
        return tesseract::PSM_SINGLE_BLOCK_VERT_TEXT;

    // Large/sparse region (full screen, desktop, busy UI panel)
    // Sparse text avoids Tesseract choking on whitespace-heavy layouts
    if (area > 500'000)
        return tesseract::PSM_SPARSE_TEXT;

    // Mid-size blocks: paragraphs, dialog boxes, panels
    return tesseract::PSM_SINGLE_BLOCK;
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

    // Sauvola binarization
    PIX* bin = nullptr;
    if (pixSauvolaBinarize(result_gray, 20, 0.35f, 1, nullptr, nullptr, nullptr, &bin) != 0 || !bin)
        bin = pixThresholdToBinary(result_gray, 128);

    pixDestroy(&result_gray);
    return bin;
}

static Pix* rgba_to_pix(std::span<const uint8_t> rgba, int w, int h)
{
    const size_t required = static_cast<size_t>(w) * h * 4;
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
        uint32_t* row = dst + y * stride;
        for (int x = 0; x < w; ++x)
        {
            const uint8_t* p = src + (static_cast<size_t>(y) * w + x) * 4;
            // Leptonica 32bpp word layout (big-endian word): R G B A
            SET_DATA_FOUR_BYTES(row,
                                x,
                                (static_cast<uint32_t>(p[0]) << 24) |      // R
                                    (static_cast<uint32_t>(p[1]) << 16) |  // G
                                    (static_cast<uint32_t>(p[2]) << 8) |   // B
                                    (static_cast<uint32_t>(p[3])));        // A
        }
    }

    return pix;
}

OcrAPI::OcrAPI() : m_api(std::make_unique<tesseract::TessBaseAPI>())
{}

OcrAPI::~OcrAPI()
{
    if (m_api && m_initialized)
        m_api->End();
}

Result<> OcrAPI::Configure(const char* data_path, const char* model, tesseract::OcrEngineMode oem)
{
    ocr_config_t next{ data_path, model };

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
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
}

Result<ocr_result_t> OcrAPI::ExtractTextCapture(const capture_result_t& cap)
{
    ocr_result_t ret;

    if (!m_initialized)
        return Err("Initialize the engine first");

    if (cap.view().empty() || cap.w <= 0 || cap.h <= 0)
        return Err("Image is empty");

    const size_t required = static_cast<size_t>(cap.w) * cap.h * 4;
    if (cap.view().size() < required)
        return Err("Image size is larger than required");

    PixPtr raw_pix(rgba_to_pix(cap.view(), cap.w, cap.h));
    if (!raw_pix)
        return Err("Failed to convert image into Pix format");

    // Preprocess: deskew + Sauvola binarize
    // The result is 1bpp which makes Tesseract skip its own (worse) binarization
    PixPtr pix(preprocess_pix(raw_pix.get()));
    if (!pix)
        return Err("Failed to preprocess image");

    // Use the binarized pix dimensions for PSM (they may differ after deskew rotation)
    const int proc_w = pixGetWidth(pix.get());
    const int proc_h = pixGetHeight(pix.get());

    tesseract::PageSegMode psm = choose_psm(proc_w, proc_h);

    float scale         = std::min(static_cast<float>(g_scr_w) / cap.w, static_cast<float>(g_scr_h) / cap.h);
    int   effective_dpi = static_cast<int>(get_screen_dpi() * scale);
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
        return Err("String is empty");

    ret.data = std::move(data);

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

        ret.confidence = count ? static_cast<int>(std::round(sum / count)) : 0;
        delete ri;
    }
    else
    {
        ret.confidence = m_api->MeanTextConf();
    }

    return Ok(std::move(ret));
}

// Zbar
ZbarAPI::ZbarAPI()
{
    SetConfig(zbar::ZBAR_NONE, true);  // all
    SetConfig(zbar::ZBAR_I25, false);
}

Result<zbar_result_t> ZbarAPI::ExtractTextsCapture(const capture_result_t& cap)
{
    zbar_result_t        ret;
    std::vector<uint8_t> gray(cap.w * cap.h);

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
