#include "ocr.hpp"

#include <leptonica/allheaders.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

PIX* rgba_to_pix(const uint8_t* rgba_data, int width, int height)
{
    PIX* pix = pixCreate(width, height, 32);  // 32-bit for RGBA

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int      index = (y * width + x) * 4;
            l_uint32 pixel = (rgba_data[index + 0] << 24) |  // R
                             (rgba_data[index + 1] << 16) |  // G
                             (rgba_data[index + 2] << 8) |   // B
                             rgba_data[index + 3];           // A
            pixSetPixel(pix, x, y, pixel);
        }
    }

    return pix;
}

bool OcrAPI::Init()
{
    m_api = std::make_unique<tesseract::TessBaseAPI>();
    return m_api->Init(nullptr, "eng") == 0;
}

std::optional<std::string> OcrAPI::ExtractText()
{
    std::string result;

    m_pix = rgba_to_pix(m_ss.data.data(), m_ss.region.width, m_ss.region.height);
    if (!m_pix)
        return nullptr;

    m_api->SetImage(m_pix);
    m_api->SetSourceResolution(300);
    char* str = m_api->GetUTF8Text();
    if (!str)
        return nullptr;
    result = str;

    delete[] str;
    pixDestroy(&m_pix);
    m_api->End();
    return result;
}
