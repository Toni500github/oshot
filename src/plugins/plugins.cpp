#include <chrono>
#include <cstring>

#include "fmt/chrono.h"
#include "fmt/format.h"
#include "oshot_plugin.h"
#include "screenshot_tool.hpp"
#include "util.hpp"

struct oshot_host_api_t
{
    uint32_t    abi_version = OSHOT_API_VERSION;
    const char* plugin_name = "<no plugin loaded>";
} g_api;

uint32_t oshot_get_abi_version()
{
    return g_api.abi_version;
}

oshot_str_t oshot_str_new(const char* str, size_t len)
{
    oshot_str_t r;
    r.p = reinterpret_cast<char*>(malloc(len));
    if (r.p)
        memcpy(const_cast<char*>(r.p), str, len);
    r.len = r.p ? len : 0;
    return r;
}

void oshot_str_free(oshot_str_t* str)
{
    free((void*)str->p);
    str->p   = nullptr;
    str->len = 0;
}

void oshot_display_msg(const OSLogLevel lvl, const oshot_str_t str)
{
    std::string_view   msg(str.p, str.len);
    const std::string& out = fmt::format("{}: {}", g_api.plugin_name, msg);
    switch (lvl)
    {
        case OSLogLevel::OSHOT_LOG_DEBUG:
            spdlog::warn("OSHOT_LOG_DEBUG is not allowed when displaying a message.");
            break;
        case OSLogLevel::OSHOT_LOG_INFO:  info("{}", out); break;
        case OSLogLevel::OSHOT_LOG_WARN:  warn("{}", out); break;
        case OSLogLevel::OSHOT_LOG_ERROR: error("{}", out); break;
    }
}

void oshot_log(OSLogLevel lvl, oshot_str_t str)
{
    auto now = std::chrono::system_clock::now();

    std::string_view   msg(str.p, str.len);
    const std::string& out = fmt::format("[{}] {}: {}", now, g_api.plugin_name, msg);
    switch (lvl)
    {
        case OSLogLevel::OSHOT_LOG_DEBUG: spdlog::debug("{}", out); break;
        case OSLogLevel::OSHOT_LOG_INFO:  spdlog::info("{}", out); break;
        case OSLogLevel::OSHOT_LOG_WARN:  spdlog::warn("{}", out); break;
        case OSLogLevel::OSHOT_LOG_ERROR: spdlog::error("{}", out); break;
    }
}

oshot_capture_t oshot_get_capture(bool render_anns)
{
    const capture_result_t& cap = g_ss_tool.GetFinalImage(render_anns);

    oshot_capture_t ret{};  // zero-inits

    if (cap.data.empty() || cap.w <= 0 || cap.h <= 0)
        return ret;  // plugin must check ret.rgba == nullptr before use

    ret.rgba = reinterpret_cast<uint8_t*>(std::malloc(cap.data.size()));
    if (!ret.rgba)
        return ret;

    std::memcpy(ret.rgba, cap.data.data(), cap.data.size());
    ret.w = cap.w;
    ret.h = cap.h;
    return ret;
}

void oshot_capture_free(oshot_capture_t* cap)
{
    std::free(cap->rgba);
    cap->rgba = nullptr;
    cap->w = cap->h = 0;
}

bool oshot_get_text(const char* imgui_id, oshot_str_t* ret)
{
    auto& t = g_ss_tool.GetImGuiIDTexts();
    auto  p = t.find(imgui_id);
    if (p == t.end())
        return false;

    ret->p = reinterpret_cast<const char*>(std::malloc(p->second->length()));
    if (!ret->p)
        return false;

    std::memcpy(const_cast<char*>(ret->p), p->second->c_str(), p->second->length());
    ret->len = p->second->length();
    return true;
}

void oshot_set_text(const char* imgui_id, const oshot_str_t value)
{
    auto& t  = g_ss_tool.GetImGuiIDTexts();
    auto  it = t.find(imgui_id);
    if (it == t.end())
        return;  // unknown id -- nothing registered to write into

    it->second->assign(value.p, value.len);  // mutate the live buffer, no temporaries
}
