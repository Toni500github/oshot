#ifndef _OCR_HPP_
#define _OCR_HPP_

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "screen_capture.hpp"

class OcrAPI
{
public:
    OcrAPI();
    ~OcrAPI();

    // non-copyable (Tesseract is stateful)
    OcrAPI(const OcrAPI&)            = delete;
    OcrAPI& operator=(const OcrAPI&) = delete;

    bool Configure(const char* data_path, const char* model, tesseract::OcrEngineMode oem = tesseract::OEM_LSTM_ONLY);

    std::optional<std::string> RecognizeCapture(const capture_result_t& cap);

private:
    struct OcrConfig
    {
        std::string path;
        std::string model;

        bool operator==(const OcrConfig&) const = default;
    };

    struct PixDeleter
    {
        void operator()(PIX* pix) const
        {
            if (pix)
                pixDestroy(&pix);
        }
    };

    using PixPtr  = std::unique_ptr<PIX, PixDeleter>;
    using TextPtr = std::unique_ptr<char, void (*)(char*)>;

    std::unique_ptr<tesseract::TessBaseAPI> m_api;
    std::optional<OcrConfig>                m_config;
    bool                                    m_initialized = false;

    static PixPtr rgba_to_pix(std::span<const uint8_t> rgba, int w, int h);
};

#endif  // !_OCR_HPP_
