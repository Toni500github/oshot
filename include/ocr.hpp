#ifndef _OCR_HPP_
#define _OCR_HPP_

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <memory>
#include <optional>
#include <string>

#include "screen_capture.hpp"

class OcrAPI
{
public:
    bool                       Init(const std::string& path, const std::string& model);
    void                       SetImage(const capture_result_t& ss) { m_ss = ss; }
    std::optional<std::string> ExtractText();

private:
    std::unique_ptr<tesseract::TessBaseAPI> m_api;
    capture_result_t                        m_ss;
    Pix*                                    m_pix = nullptr;
};

#endif  // !_OCR_HPP_
