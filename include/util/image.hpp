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

#ifndef _UTIL_IMAGE_HPP_
#define _UTIL_IMAGE_HPP_

// Image files: loading, encoding, saving

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "util/paths.hpp"
#include "util/result.hpp"

struct capture_result_t;

// ------------------------------
// Macros
// ------------------------------

/* stbi_failure_reason(), or a placeholder when stb_image didn't set one */
#define STBI_ERROR (stbi_failure_reason() ? stbi_failure_reason() : "Unknown stbi Error")

// ------------------------------
// Types
// ------------------------------

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageExt
{
    PNG,
    JPEG,
    BMP,
    TGA,
    COUNT
};

/* What to do with the image once the selection is done */
// NOLINTNEXTLINE(performance-enum-size)
enum class SavingOp
{
    kNone,
    Clipboard,
    File
};

// Names of the ImageExt values, as written in the config file
inline constexpr std::array<std::pair<ImageExt, const char*>, static_cast<size_t>(ImageExt::COUNT)> IMAGE_EXTS_STR = {
    { { ImageExt::PNG, "PNG" }, { ImageExt::JPEG, "JPEG" }, { ImageExt::BMP, "BMP" }, { ImageExt::TGA, "TGA" } }
};

// ------------------------------
// Functions
// ------------------------------

/**
 * Load an image as RGBA
 * @param path The image file, or "-" to read it from stdin
 **/
Result<capture_result_t> load_image_rgba(const std::string& path);

/**
 * Encode an image in a file format
 * @param cap The RGBA image
 * @param ext The format to encode to
 * @return The encoded file, empty on failure
 **/
std::vector<uint8_t> encode_to_image(const capture_result_t& cap, ImageExt ext);

/**
 * Encode an image and write it to a file
 * @param path The file to write
 * @param img The RGBA image
 * @param ext The format to encode to
 **/
Result<> write_image_file(const std::string& path, const capture_result_t& img, ImageExt ext);

/**
 * Write an image in a directory, named after the image-out-fmt option.
 * An existing file is never replaced, the name gets a "_2", "_3"... suffix instead
 * @param dir The directory, created if missing
 * @param img The RGBA image
 * @param ext The format to encode to
 * @return The path of the written file
 **/
Result<std::string> save_image_to_dir(const fs::path& dir, const capture_result_t& img, ImageExt ext);

/**
 * Copy an image to the clipboard or save it, then send a notification.
 * Saving goes to the --instant-save directory, else asks where with a save dialog
 * @param op Clipboard or File
 * @param img The RGBA image
 * @param ext The format to encode to
 * @return false if the user canceled the save dialog
 **/
Result<bool> save_image(SavingOp op, const capture_result_t& img, ImageExt ext);

/**
 * Get the file name for a new image, from the image-out-fmt option and the current local time
 * @return The file name, with the extension of image-out-type
 **/
Result<std::string> get_config_image_out_fmt();

/**
 * Shrink an image in place to fit the screen (g_scr_w x g_scr_h), keeping its aspect ratio.
 * Smaller images are left alone
 * @param img The RGBA image
 **/
void fit_to_screen(capture_result_t& img);

/**
 * Convert an RGBA image to 8-bit grayscale (ITU-R BT.601 luminance)
 * @param rgba The RGBA pixels
 * @param result Where to write `width * height` gray pixels
 * @param width The image width
 * @param height The image height
 **/
void rgba_to_grayscale(const uint8_t* rgba, uint8_t* result, int width, int height);

#endif  // _UTIL_IMAGE_HPP_
