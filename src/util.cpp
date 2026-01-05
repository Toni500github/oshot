#include "util.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "config.hpp"
#include "fmt/compile.h"
#include "fmt/format.h"
#include "frozen/string.h"
#include "langs.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "socket.hpp"
#include "tinyfiledialogs.h"

#define SVPNG_OUTPUT std::vector<uint8_t>* output
#define SVPNG_PUT(u) output->push_back(static_cast<uint8_t>(u))
#include "svpng.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"

extern int scr_w, scr_h;

#if __linux__
std::vector<uint8_t> ximage_to_rgba(XImage* image, int width, int height)
{
    std::vector<uint8_t> rgba_data(width * height * 4);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            uint64_t pixel = XGetPixel(image, x, y);

            int i            = (y * width + x) * 4;
            rgba_data[i + 0] = (pixel >> 16) & 0xff;  // R
            rgba_data[i + 1] = (pixel >> 8) & 0xff;   // G
            rgba_data[i + 2] = (pixel) & 0xff;        // B
            rgba_data[i + 3] = 0xff;                  // A
        }
    }

    return rgba_data;
}
#endif

std::vector<uint8_t> ppm_to_rgba(uint8_t* ppm, int width, int height)
{
    std::vector<uint8_t> rgba_data(width * height * 4);

    for (int i = 0; i < width * height; ++i)
    {
        rgba_data[i * 4 + 0] = ppm[i * 3 + 0];  // R
        rgba_data[i * 4 + 1] = ppm[i * 3 + 1];  // G
        rgba_data[i * 4 + 2] = ppm[i * 3 + 2];  // B
        rgba_data[i * 4 + 3] = 0xff;            // A
    }

    return rgba_data;
}

std::vector<uint8_t> rgba_to_ppm(const std::vector<uint8_t>& rgba, int width, int height)
{
    const std::string& header = "P6\n" + fmt::to_string(width) + " " + fmt::to_string(height) + "\n255\n";

    std::vector<uint8_t> ppm_data;
    ppm_data.reserve(header.size() + (width * height * 3));
    ppm_data.insert(ppm_data.end(), header.begin(), header.end());

    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        ppm_data.push_back(rgba[i]);      // R
        ppm_data.push_back(rgba[i + 1]);  // G
        ppm_data.push_back(rgba[i + 2]);  // B
        // Skip rgba[i + 3] (alpha channel)
    }

    return ppm_data;
}

void fit_to_screen(capture_result_t& img, int screen_w, int screen_h)
{
    const int img_w = img.region.width;
    const int img_h = img.region.height;

    if (img_w <= screen_w && img_h <= screen_h)
        return;

    float scale = std::min(static_cast<float>(screen_w) / img_w, static_cast<float>(screen_h) / img_h);

    int new_w = static_cast<int>(img_w * scale);
    int new_h = static_cast<int>(img_h * scale);

    std::vector<uint8_t> resized(new_w * new_h * 4);

    bool ok = stbir_resize_uint8_linear(img.data.data(), img_w, img_h, 0, resized.data(), new_w, new_h, 0, STBIR_RGBA);

    if (!ok)
    {
        img.success   = false;
        img.error_msg = "Image resize failed";
        return;
    }

    img.data          = std::move(resized);
    img.region.width  = new_w;
    img.region.height = new_h;
}

static std::vector<uint8_t> read_stdin_binary()
{
    std::vector<uint8_t> buffer;

    uint8_t temp[4096];
    while (true)
    {
        size_t n = fread(temp, 1, sizeof(temp), stdin);
        if (n == 0)
            break;
        buffer.insert(buffer.end(), temp, temp + n);
    }

    return buffer;
}

capture_result_t load_image_rgba(bool stdin_has_data, const std::string& path)
{
    capture_result_t result{};

    int width    = 0;
    int height   = 0;
    int channels = 0;

    // Force RGBA output (4 channels)
    stbi_uc* pixels = nullptr;
    if (!stdin_has_data)
    {
        pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    }
    else
    {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif

        std::vector<uint8_t> input = read_stdin_binary();
        if (input.empty())
        {
            result.success   = false;
            result.error_msg = "stdin reported data but was empty";
            return result;
        }

        pixels = stbi_load_from_memory(
            input.data(), static_cast<int>(input.size()), &width, &height, &channels, STBI_rgb_alpha);
    }

    if (!pixels)
    {
        result.success   = false;
        result.error_msg = stbi_failure_reason();
        return result;
    }

    result.region.width  = width;
    result.region.height = height;

    const size_t size = static_cast<size_t>(width) * height * 4;
    result.data.assign(pixels, pixels + size);

    result.success = true;

    stbi_image_free(pixels);

    fit_to_screen(result, scr_w, scr_h);
    return result;
}

bool save_png(SavingOp op, const capture_result_t& img)
{
    std::vector<uint8_t> data;
    data.reserve(img.region.width * img.region.height * 4);
    svpng(&data, img.region.width, img.region.height, img.data.data(), 1);
    size_t size = data.size();

    if (op == SavingOp::SAVE_CLIPBOARD)
    {
        if (sender->IsFailed())
            die("Couldn't copy image into clipboard: launcher not respoding/opened");
        return sender->Send(SendMsg::COPY_IMAGE, data.data(), size);
    }

    auto        now       = std::chrono::system_clock::now();
    const char* filter[]  = { "*.png" };
    const char* save_path = tinyfd_saveFileDialog("Save File",
                                                  fmt::format("oshot_{:%F_%H-%M}.png", now).c_str(),  // default path
                                                  1,                // number of filter patterns
                                                  filter,           // file filters
                                                  "Images (*.png)"  // filter description
    );
    if (!save_path)
        return false;

    FILE* fp = fopen(save_path, "wb");
    if (!fp)
        die("Failed to open file");

    fwrite(data.data(), 1, size, fp);
    fclose(fp);
    return true;
}

std::string replace_str(std::string& str, const std::string_view from, const std::string_view to)
{
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();  // Handles case where 'to' is a substring of 'from'
    }
    return str;
}

std::string expandVar(std::string ret, bool dont)
{
    if (ret.empty() || dont)
        return ret;

    const char* env;
    if (ret.front() == '~')
    {
        env = std::getenv("HOME");
        if (env == nullptr)
            die(_("FATAL: $HOME enviroment variable is not set (how?)"));

        ret.replace(0, 1, env);  // replace ~ with the $HOME value
    }
    else if (ret.front() == '$')
    {
        ret.erase(0, 1);

        std::string   temp;
        const size_t& pos = ret.find('/');
        if (pos != std::string::npos)
        {
            temp = ret.substr(pos);
            ret.erase(pos);
        }

        env = std::getenv(ret.c_str());
        if (env == nullptr)
            die(_("No such enviroment variable: {}"), ret);

        ret = env;
        ret += temp;
    }

    return ret;
}

std::filesystem::path getHomeConfigDir()
{
#if __unix__
    const char* dir = std::getenv("XDG_CONFIG_HOME");
    if (dir != NULL && dir[0] != '\0' && std::filesystem::exists(dir))
    {
        return std::filesystem::path(dir);
    }
    else
    {
        const char* home = std::getenv("HOME");
        if (home == nullptr)
            die(_("Failed to find $HOME, set it to your home directory!"));

        return std::filesystem::path(home) / ".config";
    }
#else
    PWSTR widePath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &widePath)))
    {
        // Get required buffer size
        int         size = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, NULL, 0, NULL, NULL);
        std::string narrowPath(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, widePath, -1, &narrowPath[0], size, NULL, NULL);
        CoTaskMemFree(widePath);

        // Remove null terminator from string
        narrowPath.pop_back();
        return narrowPath;
    }
    const char* dir = std::getenv("APPDATA");
    if (dir != NULL && dir[0] != '\0' && std::filesystem::exists(dir))
        return std::filesystem::path(dir);
    else
        die("Failed to get %APPDATA% path");
#endif
}

std::filesystem::path getConfigDir()
{
    return getHomeConfigDir() / "oshot";
}

std::filesystem::path get_font_path(const std::string& font)
{
#ifdef _WIN32
    static constexpr std::array<std::string_view, 2> default_search_paths = {
        "C:\\Windows\\Fonts\\",
        "C:\\Windows\\Resources\\Themes\\Fonts\\",
    };
#else
    static constexpr std::array<std::string_view, 4> default_search_paths = {
        "/usr/share/fonts/",
        "/usr/local/share/fonts/",
        "~/.fonts/",
        "~/.local/share/fonts/",
    };
#endif
    if (std::filesystem::path(font).is_absolute())
        return font;

    for (const std::string_view path : default_search_paths)
    {
        const std::string& font_path = expandVar(fmt::format(FMT_COMPILE("{}{}"), path, font));
        if (std::filesystem::exists(font_path))
            return font_path;
    }

    return {};
}

std::filesystem::path get_lang_font_path(const std::string& lang)
{
    if (config->lang_fonts_paths.find(lang) != config->lang_fonts_paths.end())
    {
        const std::filesystem::path font_path_config(config->lang_fonts_paths[lang]);
        if (font_path_config.is_absolute())
            return font_path_config;

        return get_font_path(font_path_config.string());
    }

    const auto& it = lang_fonts.find(frozen::string(lang));
    if (it != lang_fonts.end())
    {
        for (const frozen::string font : it->second)
        {
            const auto& path = get_font_path(font.data());
            if (!path.empty())
                return path;
        }
    }

    return {};
}
