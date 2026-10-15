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

#ifndef _TEXT_EXTRACTION_HPP_
#define _TEXT_EXTRACTION_HPP_

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <zbar.h>

#include <memory>
#include <optional>
#include <string>

#include "screen_capture.hpp"
#include "util.hpp"

// ------------------------------
// OCR (tesseract img2text)
// ------------------------------
struct ocr_result_t
{
    std::string data;
    int         confidence = -1;  // 0..100
    int         psm;
    std::string psm_str;
};

class OcrAPI
{
public:
    OcrAPI() : m_api(std::make_unique<tesseract::TessBaseAPI>()) {};
    ~OcrAPI();

    // non-copyable (Tesseract is stateful)
    OcrAPI(const OcrAPI&)            = delete;
    OcrAPI& operator=(const OcrAPI&) = delete;

    Result<>             Configure(const char*              data_path,
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

    using TextPtr = std::unique_ptr<char, void (*)(char*)>;

    std::unique_ptr<tesseract::TessBaseAPI> m_api;
    std::optional<ocr_config_t>             m_config;
    bool                                    m_initialized = false;
};

// ------------------------------
// Zbar (QR/Bar Codes)
// ------------------------------
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
