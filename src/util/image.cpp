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

#include "util/image.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cache.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "i18n.hpp"
#include "nvdialog/nvdialog_notification.h"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "util.hpp"
#include "util/color.hpp"
#include "util/dialogs.hpp"
#include "util/file_dialogs.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/strings.hpp"
#include "util/window.hpp"

#if OSHOT_WINDOWS
#  include <fcntl.h>
#  include <io.h>
#endif

// stb_image_write's own deflate is slow and compresses poorly (a 1080p screenshot took 143 ms
// and 160 KB), so PNGs go through zlib instead (27 ms, 91 KB). stb frees the result with free()
static unsigned char* zlib_compress(unsigned char* data, int data_len, int* out_len, int level)
{
    uLongf len = compressBound(uLong(data_len));
    auto*  out = static_cast<unsigned char*>(std::malloc(len));
    if (!out || compress2(out, &len, data, uLong(data_len), level) != Z_OK)
    {
        std::free(out);
        return nullptr;
    }
    *out_len = int(len);
    return out;
}

#define STBI_WRITE_NO_STDIO
#define STBIW_ZLIB_COMPRESS zlib_compress
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

// Level 4 is zlib's first lazy-matching level. On screenshots it gets within 4% of level 6's size,
// and on photo-like content (wallpapers) it's 4x faster (181 vs 824 ms at 1080p). Every row uses
// the "Up" filter, since stb's default tries all five per row, which cost as much as the
// compression itself for a file about 5% smaller
[[maybe_unused]] static const bool png_settings = []() noexcept {
    stbi_write_png_compression_level = 4;
    stbi_write_force_png_filter      = 2;
    return true;
}();

std::vector<uint8_t> encode_to_image(const capture_result_t& cap, ImageExt ext)
{
    std::vector<uint8_t> out;
    if (cap.w <= 0 || cap.h <= 0 || cap.data.empty())
        return out;

    auto callback = [](void* context, void* data, int size) {
        auto* buf   = static_cast<std::vector<uint8_t>*>(context);
        auto* bytes = static_cast<uint8_t*>(data);

        buf->insert(buf->end(), bytes, bytes + size);
    };

    out.reserve(size_t(cap.w) * cap.h * 4);

    switch (ext)
    {
        case ImageExt::PNG:
            stbi_write_png_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data(), cap.w * 4);
            break;

        case ImageExt::JPEG:
            stbi_write_jpg_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data(), 90);
            break;

        case ImageExt::BMP: stbi_write_bmp_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data()); break;
        case ImageExt::TGA: stbi_write_tga_to_func(callback, &out, cap.w, cap.h, STBIR_RGBA, cap.data.data()); break;

        default: break;
    }

    return out;
}

void fit_to_screen(capture_result_t& img)
{
    if (img.w <= g_scr_w && img.h <= g_scr_h)
        return;

    float scale = std::min(float(g_scr_w) / float(img.w), float(g_scr_h) / float(img.h));

    int new_w = int(std::round(float(img.w) * scale));
    int new_h = int(std::round(float(img.h) * scale));

    std::vector<uint8_t> resized(size_t(new_w) * new_h * 4);

    bool ok = stbir_resize_uint8_linear(img.data.data(), img.w, img.h, 0, resized.data(), new_w, new_h, 0, STBIR_RGBA);
    if (!ok)
    {
        spdlog::warn("Failed to resize image: {}", STBI_ERROR);
        return;
    }

    img.data = std::move(resized);
    img.w    = new_w;
    img.h    = new_h;
}

static std::vector<uint8_t> read_stdin_binary()
{
    std::vector<uint8_t> buffer;

    uint8_t temp[UINT16_MAX];  // NOLINT
    while (true)
    {
        size_t n = fread(temp, 1, sizeof(temp), stdin);
        if (n == 0)
            break;
        buffer.insert(buffer.end(), temp, temp + n);
    }

    return buffer;
}

Result<capture_result_t> load_image_rgba(const std::string& path)
{
    capture_result_t result{};

    int width    = 0;
    int height   = 0;
    int channels = 0;

    // Force RGBA output (4 channels)
    stbi_uc* pixels = nullptr;
    if (path != "-")
    {
        pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        // stb only says "can't fopen", but fopen() left the reason in errno
        if (!pixels && std::string_view(STBI_ERROR) == "can't fopen")
            return Err("{}: {}", path, strerror(errno));
    }
    else
    {
#if OSHOT_WINDOWS
        _setmode(_fileno(stdin), _O_BINARY);
#endif

        const std::vector<uint8_t>& input = read_stdin_binary();
        if (input.empty())
            return Err("No image data received from stdin");

        pixels = stbi_load_from_memory(input.data(), int(input.size()), &width, &height, &channels, STBI_rgb_alpha);
    }

    if (!pixels)
        return Err("{}: {}", path == "-" ? "stdin" : path, STBI_ERROR);

    result.w = width;
    result.h = height;

    const size_t size = size_t(width) * height * 4;
    result.data.assign(pixels, pixels + size);

    stbi_image_free(pixels);

    return Ok(std::move(result));
}

Result<std::string> get_config_image_out_fmt()
{
    // fmt formats a system_clock time_point as UTC, so convert to local time first
    const std::time_t now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm           now{};
#if OSHOT_WINDOWS
    localtime_s(&now, &now_t);
#else
    localtime_r(&now_t, &now);
#endif

    std::string out_path;
    try
    {
        out_path = fmt::format(
            fmt::runtime(g_config->File.image_out_fmt + "." + str_tolower(g_config->File.image_out_type.first)), now);
    }
    catch (fmt::format_error& err)
    {
        return Err("Bad image output format string: {}", std::string(err.what()));
    }
    return Ok(out_path);
}

Result<> write_image_file(const std::string& path, const capture_result_t& img, ImageExt ext)
{
    const std::vector<uint8_t>& data = encode_to_image(img, ext);
    if (data.empty())
        return Err("Failed to encode the image ({}x{} px)", img.w, img.h);

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp)
        return Err("Failed to open {} to write the image: {}", path, strerror(errno));

    size_t written = fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    if (written != data.size())
        return Err("Failed to write image data to {}", path);
    return Ok();
}

Result<std::string> save_image_to_dir(const fs::path& dir, const capture_result_t& img, ImageExt ext)
{
    const Result<std::string>& name = get_config_image_out_fmt();
    TRY(name);

    // image-out-fmt may add folders of its own
    const fs::path  base   = dir / name.get();
    const fs::path& folder = base.parent_path();

    std::error_code err;
    fs::create_directories(folder, err);
    if (err)
        return Err("Failed to create the folder {}: {}", folder.string(), err.message());

    // The default name changes once a minute, so a second save in the same minute gets "_2" and so on
    fs::path path = base;
    for (int n = 2; fs::exists(path, err); ++n)
        path = folder / fmt::format("{}_{}{}", base.stem().string(), n, base.extension().string());

    TRY(write_image_file(path.string(), img, ext));
    return Ok(path.string());
}

Result<bool> save_image(SavingOp op, const capture_result_t& img, ImageExt ext)
{
    // Dialogs are set up after the copy or save, so starting GTK doesn't delay it
    const auto notify = [&](const char* title, const char* body) {
        if (dialog::init())
        {
            NvdNotification* p = nvd_notification_new(title, body, NVD_NOTIFICATION_SIMPLE);
            nvd_send_notification(p);
            nvd_delete_notification(p);
        }
    };

    if (op == SavingOp::Clipboard)
    {
        TRY(g_clipboard.CopyImage(img, ext));
        notify(_("Copied!"), _("Screenshot copied to clipboard"));
        return Ok(true);
    }

    // --instant-save=DIR: no dialog
    if (!g_config->Runtime.instant_save_dir.empty())
    {
        const Result<std::string>& saved = save_image_to_dir(g_config->Runtime.instant_save_dir, img, ext);
        TRY(saved);
        spdlog::info("Saved to {}", saved.get());
        notify(_("Saved!"), _("Screenshot saved successfully"));
        return Ok(true);
    }

    const Result<std::string>& fmt = get_config_image_out_fmt();
    TRY(fmt);

    const bool        hidden         = hide_window();
    const fs::path&   saved_path_dir = g_cache->GetValue(CacheEntry::ImgSavePath, get_home_pictures_dir().string());
    const std::string ext_name       = str_tolower(g_config->File.image_out_type.first);

    const SDL_DialogFileFilter        filter = { .name = _("Images"), .pattern = ext_name.c_str() };
    const std::optional<std::string>& save_path =
        dialog::save_file(_("Save File"), (saved_path_dir / fmt.get()).string(), filter);

    if (hidden)
        show_window();

    if (!save_path)
        return Ok(false);

    TRY(write_image_file(*save_path, img, ext));
    notify(_("Saved!"), _("Screenshot saved successfully"));
    g_cache->SetValue(CacheEntry::ImgSavePath, fs::path(*save_path).parent_path().string());

    return Ok(true);
}

void rgba_to_grayscale(const uint8_t* src, uint8_t* result, int width, int height)
{
    const int pixels = width * height;
    for (int i = 0; i < pixels; ++i)
    {
        rgba_t c = load_rgba(src + static_cast<ptrdiff_t>(i * 4));
        // ITU-R BT.601 luminance
        result[i] = uint8_t((77 * c.r + 150 * c.g + 29 * c.b) >> 8);
    }
}
