#ifndef _TESTS_COMMON_HPP_
#define _TESTS_COMMON_HPP_

#include "text_extraction.hpp"

// The OCR engine every test shares, so a model loads once; main() ends it before exiting
OcrAPI& test_ocr();

#endif  // !_TESTS_COMMON_HPP_
