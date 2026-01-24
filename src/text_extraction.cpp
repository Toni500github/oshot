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

bool OcrAPI::Configure(const char* data_path, const char* model, tesseract::OcrEngineMode oem)
{
    ocr_config_t next{ data_path, model };

    if (m_config && *m_config == next)
        return true;  // nothing to do

    // Tear down old engine
    if (m_initialized)
    {
        m_api->End();
        m_initialized = false;
    }

    if (m_api->Init(data_path, model, oem) != 0)
        return false;

    m_config      = std::move(next);
    m_initialized = true;
    return true;
}

static inline void trim_in_place(std::string& s)
{
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    auto b      = std::find_if(s.begin(), s.end(), not_ws);
    auto e      = std::find_if(s.rbegin(), s.rend(), not_ws).base();
    if (b >= e)
    {
        s.clear();
        return;
    }
    s.assign(b, e);
}

ocr_result_t OcrAPI::ExtractTextCapture(const capture_result_t& cap)
{
    ocr_result_t ret;

    if (!m_initialized || cap.view().empty() || cap.region.width <= 0 || cap.region.height <= 0)
        return ret;

    const size_t required = static_cast<size_t>(cap.region.width) * cap.region.height * 4;
    if (cap.view().size() < required)
        return ret;

    tesseract::PageSegMode psm = choose_psm(cap.region.width, cap.region.height);
    PixPtr                 pix = RgbaToPix(cap.view(), cap.region.width, cap.region.height);
    if (!pix)
        return ret;

    float scale =
        std::min(static_cast<float>(g_scr_w) / cap.region.width, static_cast<float>(g_scr_h) / cap.region.height);

    int effective_dpi = static_cast<int>(get_screen_dpi() * scale);
    effective_dpi     = std::clamp(effective_dpi, 70, 300);

    m_api->SetPageSegMode(psm);
    m_api->SetImage(pix.get());
    m_api->SetSourceResolution(effective_dpi);

    // Make OCR + confidence deterministic
    if (m_api->Recognize(nullptr) != 0)
        return ret;

    TextPtr text(m_api->GetUTF8Text(), [](char* p) { delete[] p; });
    if (!text)
        return ret;

    std::string data(text.get());
    trim_in_place(data);
    if (data.empty())
        return ret;

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

    ret.success = true;
    return ret;
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

zbar_result_t ZbarAPI::ExtractTextsCapture(const capture_result_t& cap)
{
    zbar_result_t        ret;
    std::vector<uint8_t> gray(cap.region.width * cap.region.height);

    rgba_to_grayscale(cap.view().data(), gray.data(), cap.region.width, cap.region.height);

    zbar::Image image(cap.region.width,
                      cap.region.height,
                      "Y800",  // GRAYSCALE
                      gray.data(),
                      gray.size());

    if (m_scanner.scan(image) <= 0)
        return {};

    for (auto sym = image.symbol_begin(); sym != image.symbol_end(); ++sym)
    {
        ret.datas.push_back(sym->get_data());
        ret.symbologies[sym->get_type_name()]++;
    }

    ret.success = !ret.datas.empty() || !ret.symbologies.empty();

    // Prevent ZBar from freeing the buffer
    image.set_data(nullptr, 0);

    return ret;
}

bool ZbarAPI::SetConfig(zbar::zbar_symbol_type_e zbar_code, int enable)
{
    return m_scanner.set_config(zbar_code, zbar::ZBAR_CFG_ENABLE, enable) == 0;
}
