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
#include <vector>

#include "screen_capture.hpp"
#include "util.hpp"

// ------------------------------
// OCR (tesseract img2text)
// ------------------------------
// A word Tesseract read, with its box in the pixels of the image that was read
struct ocr_word_t
{
    std::string text;
    float       x = 0, y = 0, w = 0, h = 0;
    int         confidence = 0;

    // Text line and block, counted from 0 over the whole result
    int line  = 0;
    int block = 0;
};

struct ocr_result_t
{
    // Mean word confidence, 0..100, or -1 if nothing was read yet
    int confidence = -1;

    // The tesseract::PageSegMode the text was read with, and its name
    int         psm;
    std::string psm_str;

    std::string data;

    std::vector<ocr_word_t> words;
};

class OcrAPI
{
public:
    OcrAPI() : m_api(std::make_unique<tesseract::TessBaseAPI>()) {};
    ~OcrAPI();

    // non-copyable (Tesseract is stateful)
    OcrAPI(const OcrAPI&)            = delete;
    OcrAPI& operator=(const OcrAPI&) = delete;

    /**
     * (Re)initialize the engine with a model. Does nothing if it already uses it
     * @param data_path The directory with the .traineddata models
     * @param model The model name, e.g. "eng" for eng.traineddata
     * @param oem The engine mode
     **/
    Result<> Configure(const char*              data_path,
                       const char*              model,
                       tesseract::OcrEngineMode oem = tesseract::OEM_LSTM_ONLY);

    /**
     * Read the text in an image, after preparing it for Tesseract (scaled, padded,
     * dark text on white). Configure() must have succeeded first
     * @param cap The RGBA image
     **/
    Result<ocr_result_t> ExtractTextCapture(const capture_result_t& cap);

    /**
     * Free the engine and its model; the next Configure() loads it again.
     * Call it before the process exits: Tesseract's own static caches are
     * destroyed before a global OcrAPI, and ending it after them aborts
     **/
    void End();

private:
    struct ocr_config_t
    {
        std::string path;
        std::string model;

        bool operator==(const ocr_config_t&) const = default;
    };

    using TextPtr = std::unique_ptr<char, void (*)(char*)>;

    // What the engine was last initialized with
    std::optional<ocr_config_t> m_config;

    std::unique_ptr<tesseract::TessBaseAPI> m_api;
    bool                                    m_initialized = false;
};

// ------------------------------
// Zbar (QR/Bar Codes)
// ------------------------------
// A decoded code, with the box around its location points in the pixels of the scanned image
struct barcode_t
{
    std::string type;  // e.g. "QR-Code", "EAN-13"
    std::string data;
    float       x = 0, y = 0, w = 0, h = 0;
};

struct zbar_result_t
{
    std::vector<std::string>             datas;        // decoded payloads, without duplicates
    std::unordered_map<std::string, int> symbologies;  // how many of each, e.g. "QRCODE", "EAN-13", ...
    std::vector<barcode_t>               codes;        // every code found, duplicates included
};

class ZbarAPI
{
public:
    // Every symbology is enabled
    ZbarAPI();

    /**
     * Decode the QR codes and barcodes in an image. Also tries the inverted image
     * (light codes on dark) and, for small images, a 2x upscale
     * @param cap The RGBA image
     **/
    Result<zbar_result_t> ExtractTextsCapture(const capture_result_t& cap);

    /**
     * Enable or disable a symbology
     * @param zbar_code The symbology, or ZBAR_NONE for all of them
     * @param enable 1 to enable it, 0 to disable it
     * @return false if ZBar refused the setting
     **/
    bool SetConfig(zbar::zbar_symbol_type_e zbar_code, int enable);

private:
    zbar::ImageScanner m_scanner;
};

#endif  // !_TEXT_EXTRACTION_HPP_
