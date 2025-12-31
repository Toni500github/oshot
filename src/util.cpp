#include "util.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "config.hpp"
#include "fmt/compile.h"
#include "fmt/format.h"
#include "frozen/string.h"
#include "langs.hpp"

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

std::string replace_str(std::string str, const std::string_view from, const std::string_view to)
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
