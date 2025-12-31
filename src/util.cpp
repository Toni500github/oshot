#include "util.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "fmt/format.h"

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
    std::string header = "P6\n" + fmt::to_string(width) + " " + fmt::to_string(height) + "\n255\n";

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
    {
        return std::filesystem::path(dir);
    }
    else
    {
        die("Failed to get %APPDATA% path");
    }
#endif
}

std::filesystem::path getConfigDir()
{
    return getHomeConfigDir() / "oshot";
}
