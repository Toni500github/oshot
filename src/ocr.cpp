#include "ocr.hpp"

#include <cstring>

#include "util.hpp"

static tesseract::PageSegMode choose_psm(int w, int h)
{
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
    OcrConfig next{ data_path, model };

    if (m_config && *m_config == next)
        return true;  // nothing to do

    // Tear down old engine
    if (m_initialized)
    {
        m_api->End();
        m_initialized = false;
    }

    if (m_api->Init(data_path, model, oem) != 0)
    {
        return false;
    }

    m_config      = std::move(next);
    m_initialized = true;
    return true;
}

std::optional<std::string> OcrAPI::RecognizeCapture(const capture_result_t& cap)
{
    if (!m_initialized || cap.view().empty() || cap.region.width <= 0 || cap.region.height <= 0)
        return std::nullopt;

    const size_t required = static_cast<size_t>(cap.region.width) * cap.region.height * 4;

    if (cap.view().size() < required)
        return std::nullopt;

    tesseract::PageSegMode psm = choose_psm(cap.region.width, cap.region.height);
    PixPtr                 pix = rgba_to_pix(cap.view(), cap.region.width, cap.region.height);
    if (!pix)
        return std::nullopt;

    float scale = std::min(static_cast<float>(scr_w) / cap.region.width, static_cast<float>(scr_h) / cap.region.height);

    int effective_dpi = static_cast<int>(get_screen_dpi() * scale);
    effective_dpi     = std::clamp(effective_dpi, 70, 300);

    m_api->SetPageSegMode(psm);
    m_api->SetImage(pix.get());
    m_api->SetSourceResolution(effective_dpi);

    TextPtr text(m_api->GetUTF8Text(), [](char* p) { delete[] p; });

    if (!text)
        return std::nullopt;

    return std::string(text.get());
}

OcrAPI::PixPtr OcrAPI::rgba_to_pix(std::span<const uint8_t> rgba, int w, int h)
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
            const auto* p   = &rgba[4 * (y * w + x)];
            uint32_t*   dst = data + y * stride + x;

            // RGBA → Leptonica BGRA
            *dst = (p[2] << 24) |  // B
                   (p[1] << 16) |  // G
                   (p[0] << 8) |   // R
                   (p[3]);         // A
        }
    }

    return PixPtr(pix);
}
