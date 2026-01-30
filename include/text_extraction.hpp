#ifndef _TEXT_EXTRACTION_HPP_
#define _TEXT_EXTRACTION_HPP_

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <zbar.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "screen_capture.hpp"
#include "util.hpp"

struct ocr_result_t
{
    std::string data;
    int         confidence = 0;  // 0..100
};

class OcrAPI
{
public:
    OcrAPI();
    ~OcrAPI();

    // non-copyable (Tesseract is stateful)
    OcrAPI(const OcrAPI&)            = delete;
    OcrAPI& operator=(const OcrAPI&) = delete;

    Result<bool>         Configure(const char*              data_path,
                                   const char*              model,
                                   tesseract::OcrEngineMode oem = tesseract::OEM_LSTM_ONLY);
    Result<ocr_result_t> ExtractTextCapture(const capture_result_t& cap);

private:
    struct ocr_config_t
    {
        std::string path;
        std::string model;

        bool operator==(const ocr_config_t&) const = default;
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
    std::optional<ocr_config_t>             m_config;
    bool                                    m_initialized = false;

    static PixPtr RgbaToPix(std::span<const uint8_t> rgba, int w, int h);
};

struct zbar_result_t
{
    std::vector<std::string>             datas;        // decoded payload
    std::unordered_map<std::string, int> symbologies;  // e.g. "QRCODE", "EAN-13", ...
};

class ZbarAPI
{
public:
    ZbarAPI();

    Result<zbar_result_t> ExtractTextsCapture(const capture_result_t& cap);
    bool                  SetConfig(zbar::zbar_symbol_type_e zbar_code, int enable);

private:
    zbar::ImageScanner m_scanner;
};

#endif  // !_TEXT_EXTRACTION_HPP_
