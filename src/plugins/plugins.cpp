#include <chrono>
#include <cstring>

#include "config.hpp"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "oshot_plugin.h"
#include "screenshot_tool.hpp"
#include "util.hpp"

struct oshot_host_api_t
{
    uint32_t    abi_version = OSHOT_API_VERSION;
    oshot_plugin_t plugin = {};
} g_api;

/* ------------------------------------------------------------------
 * ABI / identity
 * ------------------------------------------------------------------ */
uint32_t oshot_get_abi_version()
{
    return g_api.abi_version;
}

/* ------------------------------------------------------------------
 * oshot_str_t lifecycle
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * Logging / host messaging
 * ------------------------------------------------------------------ */
void oshot_display_msg(const OSLogLevel lvl, const oshot_str_t str)
{
    const std::string& out = fmt::format("{}: {}", g_api.plugin.name, std::string_view(str.p, str.len));
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

    const std::string& out = fmt::format("[{}] {}: {}", now, g_api.plugin.name, std::string_view(str.p, str.len));
    switch (lvl)
    {
        case OSLogLevel::OSHOT_LOG_DEBUG: spdlog::debug("{}", out); break;
        case OSLogLevel::OSHOT_LOG_INFO:  spdlog::info("{}", out); break;
        case OSLogLevel::OSHOT_LOG_WARN:  spdlog::warn("{}", out); break;
        case OSLogLevel::OSHOT_LOG_ERROR: spdlog::error("{}", out); break;
    }
}

void oshot_debug(oshot_str_t str)
{
    oshot_log(OSLogLevel::OSHOT_LOG_DEBUG, std::move(str));
}

/* ------------------------------------------------------------------
 * Config
 * ------------------------------------------------------------------ */
oshot_str_t oshot_config_get_string(const char* key, oshot_str_t fallback)
{
    const std::string& str = g_config->GetValue<std::string>(fmt::format("plugins.{}.{}", g_api.plugin.name, key),
                                                             std::string(fallback.p, fallback.len));
    return oshot_str_new(str.c_str(), str.length());
}

bool oshot_config_get_bool(const char* key, bool fallback)
{
    bool val = g_config->GetValue<bool>(fmt::format("plugins.{}.{}", g_api.plugin.name, key), fallback);
    return val;
}

int64_t oshot_config_get_int64(const char* key, int64_t fallback)
{
    int64_t val = g_config->GetValue<int64_t>(fmt::format("plugins.{}.{}", g_api.plugin.name, key), fallback);
    return val;
}

float oshot_config_get_float(const char* key, float fallback)
{
    float val = g_config->GetValue<float>(fmt::format("plugins.{}.{}", g_api.plugin.name, key), fallback);
    return val;
}

double oshot_config_get_double(const char* key, double fallback)
{
    double val = g_config->GetValue<double>(fmt::format("plugins.{}.{}", g_api.plugin.name, key), fallback);
    return val;
}

size_t oshot_config_get_array(const char* key, oshot_value_t** out, size_t max)
{
    const toml::array* arr = g_config->GetValueArray(fmt::format("plugins.{}.{}", g_api.plugin.name, key));
    if (!arr)
        return 0;

    size_t i = 0;
    for (auto&& el : *arr)
    {
        if (i == max)
            break;

        oshot_value_t& val = *out[i];
        switch (el.type())
        {
            case toml::node_type::string:
                val.kind = OSValueKind::OSHOT_VAL_STRING;
                val.s    = oshot_str_new(el.as_string()->get().c_str(), el.as_string()->get().length());
                break;

            case toml::node_type::boolean:
                val.kind = OSValueKind::OSHOT_VAL_BOOL;
                val.b    = el.as_boolean()->get();
                break;

            case toml::node_type::integer:
                val.kind = OSValueKind::OSHOT_VAL_INT64;
                val.i    = el.as_integer()->get();
                break;

            case toml::node_type::floating_point:
                val.kind = OSValueKind::OSHOT_VAL_DOUBLE;
                val.d    = el.as_floating_point()->get();
                break;

            default:
                continue;  // unsupported TOML type: skip, don't consume a slot
        }
        i++;
    }

    return i;
}

void oshot_value_array_free(oshot_value_t* arr, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (arr[i].kind == OSValueKind::OSHOT_VAL_STRING)
            oshot_str_free(&arr[i].s);
}

/* ------------------------------------------------------------------
 * ImGui-bound text buffers
 * ------------------------------------------------------------------ */
bool oshot_get_text(const char* imgui_id, oshot_str_t* ret)
{
    auto& t = g_ss_tool.GetImGuiIDTexts();
    auto  p = t.find(imgui_id);
    if (p == t.end())
        return false;

    ret->p = static_cast<const char*>(std::malloc(p->second->length()));
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

/* ------------------------------------------------------------------
 * Capture acquisition
 * ------------------------------------------------------------------ */
oshot_capture_t oshot_get_capture(void)
{
    const capture_result_t& cap = g_ss_tool.GetFinalImage();

    oshot_capture_t ret{};  // zero-inits

    if (cap.data.empty() || cap.w <= 0 || cap.h <= 0)
        return ret;  // plugin must check ret.rgba == nullptr before use

    ret.rgba = static_cast<uint8_t*>(std::malloc(cap.data.size()));
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
