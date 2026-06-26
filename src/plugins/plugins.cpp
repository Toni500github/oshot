#include <chrono>
#include <cstring>

#include "fmt/chrono.h"
#include "fmt/format.h"
#include "oshot_plugin.h"
#include "screenshot_tool.hpp"
#include "util.hpp"

struct oshot_host_api_t
{
    uint32_t    abi_version;
    const char* plugin_name;
} g_api;

uint32_t oshot_get_abi_version()
{
    return g_api.abi_version;
}

void oshot_display_msg(const OSLogLevel lvl, const oshot_str_t str)
{
    const std::string& fmt = fmt::format("{}: {}", g_api.plugin_name, str.p);
    switch (lvl)
    {
        case OSLogLevel::OSHOT_LOG_DEBUG:
            spdlog::warn("OSHOT_LOG_DEBUG is not allowed when displaying a message.");
            break;
        case OSLogLevel::OSHOT_LOG_INFO:  info("{}", fmt); break;
        case OSLogLevel::OSHOT_LOG_WARN:  warn("{}", fmt); break;
        case OSLogLevel::OSHOT_LOG_ERROR: error("{}", fmt); break;
    }
}

void oshot_log(const OSLogLevel lvl, const oshot_str_t str)
{
    auto now = std::chrono::system_clock::now();

    const std::string& fmt = fmt::format("[{:%%F_%%H-%%M-%%S}] {}: {}", now, g_api.plugin_name, str.p);
    switch (lvl)
    {
        case OSLogLevel::OSHOT_LOG_DEBUG: spdlog::debug("{}", fmt); break;
        case OSLogLevel::OSHOT_LOG_INFO:  spdlog::info("{}", fmt); break;
        case OSLogLevel::OSHOT_LOG_WARN:  spdlog::warn("{}", fmt); break;
        case OSLogLevel::OSHOT_LOG_ERROR: spdlog::error("{}", fmt); break;
    }
}

oshot_capture_t oshot_get_capture(bool render_anns)
{
    const capture_result_t& cap = g_ss_tool.GetFinalImage(render_anns);

    oshot_capture_t ret;
    std::memcpy(ret.rgba, cap.data.data(), cap.data.size());
    ret.h = cap.h;
    ret.w = cap.w;

    return ret;
}

bool oshot_get_text(const char* imgui_id, oshot_str_t* ret)
{
    auto t = g_ss_tool.GetImGuiIDTexts();
    if (auto p = t.find(imgui_id); p != t.end())
    {
        ret->p = p->second.c_str();
        ret->len = p->second.length();
        return true;
    }
    return false;
}

void oshot_set_text(const char* imgui_id, const oshot_str_t ret)
{
    g_ss_tool.GetImGuiIDTexts().insert_or_assign(imgui_id, std::string(ret.p, ret.len));
}
