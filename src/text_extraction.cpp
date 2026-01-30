#include "text_extraction.hpp"

#include <zbar.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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

    const int   area   = w * h;
    const float aspect = (h > 0) ? float(w) / h : 1.0f;

    // Extremely small selections (icons, buttons, single words)
    if (area < 20'000 && aspect < 2.0f)
        return tesseract::PSM_SINGLE_WORD;

    // Short, wide regions (menu entries, labels)
    if (aspect > 4.0f && h < 120)
        return tesseract::PSM_SINGLE_LINE;

    // Typical UI panels, paragraphs
    if (area < 300'000)
        return tesseract::PSM_SINGLE_BLOCK;

    // Large regions / near-full window
    return tesseract::PSM_AUTO;
}

OcrAPI::OcrAPI() : m_api(std::make_unique<tesseract::TessBaseAPI>())
{}

OcrAPI::~OcrAPI()
{
    if (m_api && m_initialized)
        m_api->End();
}

Result<bool> OcrAPI::Configure(const char* data_path, const char* model, tesseract::OcrEngineMode oem)
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
static void trim_in_place(std::string& s)
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

    tesseract::PageSegMode psm = choose_psm(cap.w, cap.h);
    PixPtr                 pix = RgbaToPix(cap.view(), cap.w, cap.h);
    if (!pix)
        return Err("Failed to convert image into Pix format");

    float scale = std::min(static_cast<float>(g_scr_w) / cap.w, static_cast<float>(g_scr_h) / cap.h);

    int effective_dpi = static_cast<int>(get_screen_dpi() * scale);
    effective_dpi     = std::clamp(effective_dpi, 70, 300);

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
    trim_in_place(data);
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
    }
    else
    {
        ret.confidence = m_api->MeanTextConf();
    }

    return Ok(ret);
}

OcrAPI::PixPtr OcrAPI::RgbaToPix(std::span<const uint8_t> rgba, int w, int h)
{
    const size_t required = static_cast<size_t>(w) * h * 4;
    if (rgba.size() < required)
        return PixPtr(nullptr);

    PIX* pix = pixCreate(w, h, 32);
    if (!pix)
        return PixPtr(nullptr);

    uint32_t* data   = pixGetData(pix);
    int       stride = pixGetWpl(pix);

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            size_t      index = static_cast<size_t>(y) * w + x;
            const auto* p     = &rgba[index * 4];
            uint32_t*   dst   = data + y * stride + x;

            // RGBA → Leptonica BGRA
            *dst = (p[2] << 24) |  // B
                   (p[1] << 16) |  // G
                   (p[0] << 8) |   // R
                   (p[3]);         // A
        }
    }

    return PixPtr(pix);
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

    if (!ret.datas.empty() || !ret.symbologies.empty())
        return Err("Failed to decode barcode from image");

    // Prevent ZBar from freeing the buffer
    image.set_data(nullptr, 0);

    return Ok(ret);
}

bool ZbarAPI::SetConfig(zbar::zbar_symbol_type_e zbar_code, int enable)
{
    return m_scanner.set_config(zbar_code, zbar::ZBAR_CFG_ENABLE, enable) == 0;
}
