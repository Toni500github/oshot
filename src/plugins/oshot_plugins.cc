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

// The oshot_host_t table every plugin gets in init(). Plugins only reach
// oshot through these functions, with their oshot_ctx_t telling which plugin
// is calling; nothing here is exported for them to link against.

#include <cstdlib>
#include <cstring>

#include "cache.hpp"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "plugin.hpp"
#include "screenshot_tool.hpp"
#include "util.hpp"

static plugin_runtime_t& rt_of(oshot_ctx_t* ctx)
{
    return *reinterpret_cast<plugin_runtime_t*>(ctx);
}

/* ------------------------------------------------------------------
 * Strings
 * ------------------------------------------------------------------ */
static oshot_str_t str_new(const char* str, size_t len)
{
    oshot_str_t r{ .p = nullptr, .len = 0 };
    char*       buf = static_cast<char*>(std::malloc(len + 1));
    if (!buf)
        return r;
    if (str && len)
        std::memcpy(buf, str, len);
    buf[len] = '\0';
    r.p      = buf;
    r.len    = len;
    return r;
}

static oshot_str_t str_new(const std::string& s)
{
    return str_new(s.data(), s.size());
}

static void str_free(oshot_str_t* str)
{
    if (!str)
        return;
    std::free(const_cast<char*>(str->p));
    str->p   = nullptr;
    str->len = 0;
}

static void value_array_free(oshot_value_t* arr, size_t n)
{
    for (size_t i = 0; arr && i < n; ++i)
        if (arr[i].kind == OSHOT_VAL_STRING)
            str_free(&arr[i].s);
}

static std::string to_string(oshot_str_t s)
{
    return s.p ? std::string(s.p, s.len) : std::string();
}

/* ------------------------------------------------------------------
 * Any thread
 * ------------------------------------------------------------------ */
static void host_log(oshot_ctx_t* ctx, oshot_log_level_t lvl, oshot_str_t msg)
{
    const std::string_view text(msg.p ? msg.p : "", msg.p ? msg.len : 0);
    const std::string&     id = rt_of(ctx).id;
    switch (lvl)
    {
        case OSHOT_LOG_DEBUG: spdlog::debug("{}: {}", id, text); break;
        case OSHOT_LOG_INFO:  spdlog::info("{}: {}", id, text); break;
        case OSHOT_LOG_WARN:  spdlog::warn("{}: {}", id, text); break;
        default:              spdlog::error("{}: {}", id, text); break;
    }
}

static oshot_str_t get_data_dir(oshot_ctx_t* ctx)
{
    return str_new(rt_of(ctx).dir.string());
}

static void run_on_ui_thread(oshot_ctx_t* ctx, void (*fn)(void*), void* user)
{
    if (!fn)
        return;
    plugin_runtime_t& rt = rt_of(ctx);
    std::scoped_lock  lock(rt.tasks_mutex);
    rt.tasks.emplace_back(fn, user);
}

/* ------------------------------------------------------------------
 * UI thread only
 * ------------------------------------------------------------------ */

// Logs and returns false when called from the wrong thread, instead of racing the UI
static bool ui_thread_check(oshot_ctx_t* ctx, const char* func)
{
    if (on_plugin_ui_thread())
        return true;
    spdlog::error("{}: {}() called outside the UI thread, ignored. Use run_on_ui_thread()", rt_of(ctx).id, func);
    return false;
}

// The config lives in the plugin's own file; the cache in oshot's, under "plugins.<id>."
static TomlAPI& store_of(oshot_ctx_t* ctx, oshot_store_t store, std::string& key)
{
    plugin_runtime_t& rt = rt_of(ctx);
    if (store == OSHOT_STORE_CACHE)
    {
        key.insert(0, fmt::format("plugins.{}.", rt.id));
        return *g_cache;
    }
    return rt.config;
}

// For lookups straight in the table, which skip TomlAPI::BuildKey(): Cache keeps
// everything under [cache]
static const toml::node* find_node(oshot_ctx_t* ctx, oshot_store_t store, const char* key)
{
    std::string    k(key);
    const TomlAPI& api = store_of(ctx, store, k);
    if (store == OSHOT_STORE_CACHE)
        k.insert(0, "cache.");
    return api.GetTbl().at_path(k).node();
}

template <typename T>
static T get_typed(oshot_ctx_t* ctx, oshot_store_t store, const char* key, const T& fallback, const char* func)
{
    if (!key || !ui_thread_check(ctx, func))
        return fallback;
    std::string k(key);
    return store_of(ctx, store, k).GetValue<T>(k, fallback, true);
}

template <typename T>
static void set_typed(oshot_ctx_t* ctx, oshot_store_t store, const char* key, const T& val, const char* func)
{
    if (!key || !ui_thread_check(ctx, func))
        return;
    std::string k(key);
    store_of(ctx, store, k).SetValue<T>(k, val);
    if (store == OSHOT_STORE_CONFIG)
        rt_of(ctx).config_dirty = true;
}

static oshot_str_t get_string(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_str_t fallback)
{
    return str_new(get_typed<std::string>(ctx, store, key, to_string(fallback), "get_string"));
}

static bool get_bool(oshot_ctx_t* ctx, oshot_store_t store, const char* key, bool fallback)
{
    return get_typed<bool>(ctx, store, key, fallback, "get_bool");
}

static int64_t get_int64(oshot_ctx_t* ctx, oshot_store_t store, const char* key, int64_t fallback)
{
    return get_typed<int64_t>(ctx, store, key, fallback, "get_int64");
}

static double get_double(oshot_ctx_t* ctx, oshot_store_t store, const char* key, double fallback)
{
    return get_typed<double>(ctx, store, key, fallback, "get_double");
}

// Converts a TOML value, returns false for types the ABI doesn't have
static bool node_to_value(const toml::node& node, oshot_value_t& val)
{
    switch (node.type())
    {
        case toml::node_type::string:
            val.kind = OSHOT_VAL_STRING;
            val.s    = str_new(node.as_string()->get());
            return true;
        case toml::node_type::boolean:
            val.kind = OSHOT_VAL_BOOL;
            val.b    = node.as_boolean()->get();
            return true;
        case toml::node_type::integer:
            val.kind = OSHOT_VAL_INT64;
            val.i    = node.as_integer()->get();
            return true;
        case toml::node_type::floating_point:
            val.kind = OSHOT_VAL_DOUBLE;
            val.d    = node.as_floating_point()->get();
            return true;
        default: return false;
    }
}

static size_t get_array(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_value_t* out, size_t max)
{
    if (!key || !out || !ui_thread_check(ctx, "get_array"))
        return 0;

    const toml::node*  node = find_node(ctx, store, key);
    const toml::array* arr  = node ? node->as_array() : nullptr;
    if (!arr)
        return 0;

    size_t n = 0;
    for (const toml::node& el : *arr)
    {
        if (n == max)
            break;
        if (node_to_value(el, out[n]))
            ++n;
    }
    return n;
}

static oshot_value_t get_value(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_value_t fallback)
{
    oshot_value_t ret = fallback;
    if (fallback.kind == OSHOT_VAL_STRING)
        ret.s = str_new(fallback.s.p, fallback.s.p ? fallback.s.len : 0);

    if (!key || !ui_thread_check(ctx, "get_value"))
        return ret;

    const toml::node* node = find_node(ctx, store, key);
    oshot_value_t     found{};
    if (node && node_to_value(*node, found))
    {
        value_array_free(&ret, 1);
        return found;
    }
    return ret;
}

static void set_string(oshot_ctx_t* ctx, oshot_store_t store, const char* key, oshot_str_t val)
{
    set_typed<std::string>(ctx, store, key, to_string(val), "set_string");
}

static void set_bool(oshot_ctx_t* ctx, oshot_store_t store, const char* key, bool val)
{
    set_typed<bool>(ctx, store, key, val, "set_bool");
}

static void set_int64(oshot_ctx_t* ctx, oshot_store_t store, const char* key, int64_t val)
{
    set_typed<int64_t>(ctx, store, key, val, "set_int64");
}

static void set_double(oshot_ctx_t* ctx, oshot_store_t store, const char* key, double val)
{
    set_typed<double>(ctx, store, key, val, "set_double");
}

static void set_value(oshot_ctx_t* ctx, oshot_store_t store, const char* key, const oshot_value_t* val)
{
    if (!val)
        return;
    switch (val->kind)
    {
        case OSHOT_VAL_STRING: set_string(ctx, store, key, val->s); break;
        case OSHOT_VAL_INT64:  set_int64(ctx, store, key, val->i); break;
        case OSHOT_VAL_BOOL:   set_bool(ctx, store, key, val->b); break;
        case OSHOT_VAL_DOUBLE: set_double(ctx, store, key, val->d); break;
        default:               spdlog::error("{}: set_value() with an unknown value kind", rt_of(ctx).id); break;
    }
}

static bool get_text(oshot_ctx_t* ctx, const char* id, oshot_str_t* out)
{
    if (!id || !out || !ui_thread_check(ctx, "get_text"))
        return false;

    auto& texts = g_ss_tool.GetImGuiIDTexts();
    auto  it    = texts.find(id);
    if (it == texts.end())
        return false;

    *out = str_new(*it->second);
    return out->p != nullptr;
}

static bool set_text(oshot_ctx_t* ctx, const char* id, oshot_str_t value)
{
    if (!id || !ui_thread_check(ctx, "set_text"))
        return false;

    auto& texts = g_ss_tool.GetImGuiIDTexts();
    auto  it    = texts.find(id);
    if (it == texts.end())
    {
        spdlog::error(
            "{}: set_text(): unknown text id '{}' (use OSHOT_TEXT_OCR or OSHOT_TEXT_BARCODE)", rt_of(ctx).id, id);
        return false;
    }

    it->second->assign(to_string(value));
    return true;
}

static oshot_capture_t get_capture(oshot_ctx_t* ctx)
{
    oshot_capture_t ret{};  // rgba == nullptr: no capture
    if (!ui_thread_check(ctx, "get_capture"))
        return ret;

    const capture_result_t& cap = g_ss_tool.GetFinalImage();
    if (cap.data.empty() || cap.w <= 0 || cap.h <= 0)
        return ret;

    ret.rgba = static_cast<uint8_t*>(std::malloc(cap.data.size()));
    if (!ret.rgba)
        return ret;

    std::memcpy(ret.rgba, cap.data.data(), cap.data.size());
    ret.w = cap.w;
    ret.h = cap.h;
    return ret;
}

static void capture_free(oshot_capture_t* cap)
{
    if (!cap)
        return;
    std::free(cap->rgba);
    cap->rgba = nullptr;
    cap->w = cap->h = 0;
}

/* ------------------------------------------------------------------
 * Widgets: UI thread, inside render() and render_preferences()
 * ------------------------------------------------------------------ */

// Logs once and returns false anywhere a plugin's UI isn't being drawn
static bool ui_check(oshot_ctx_t* ctx, const char* func)
{
    plugin_runtime_t& rt = rt_of(ctx);
    if (!on_plugin_ui_thread())
    {
        rt.WarnOnce(UiWarning::OffThread,
                    fmt::format("{}() called outside the UI thread, ignored. Use run_on_ui_thread()", func));
        return false;
    }
    if (!rt.in_ui)
    {
        rt.WarnOnce(UiWarning::OutsideRender,
                    fmt::format("{}() called outside render() and render_preferences(), ignored", func));
        return false;
    }
    return true;
}

static void ui_text(oshot_ctx_t* ctx, oshot_str_t text, uint32_t flags)
{
    if (!ui_check(ctx, "ui_text"))
        return;

    const char* begin = text.p ? text.p : "";
    const char* end   = begin + (text.p ? text.len : 0);
    if (flags & OSHOT_UI_DISABLED)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (flags & OSHOT_UI_WRAPPED)
        ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(begin, end);
    if (flags & OSHOT_UI_WRAPPED)
        ImGui::PopTextWrapPos();
    if (flags & OSHOT_UI_DISABLED)
        ImGui::PopStyleColor();
}

static bool ui_button(oshot_ctx_t* ctx, oshot_str_t label)
{
    return ui_check(ctx, "ui_button") && ImGui::Button(to_string(label).c_str());
}

static bool ui_checkbox(oshot_ctx_t* ctx, oshot_str_t label, bool* value)
{
    return value && ui_check(ctx, "ui_checkbox") && ImGui::Checkbox(to_string(label).c_str(), value);
}

static bool ui_input_text(oshot_ctx_t* ctx, oshot_str_t label, oshot_str_t* value, uint32_t flags)
{
    if (!value || !ui_check(ctx, "ui_input_text"))
        return false;

    // ImGui keeps its own copy while the field is being edited, so a fresh buffer every frame is fine
    std::string       buf = to_string(*value);
    const std::string lbl = to_string(label);
    const bool        changed =
        (flags & OSHOT_UI_MULTILINE)
            ? ImGui::InputTextMultiline(lbl.c_str(), &buf)
            : ImGui::InputText(lbl.c_str(), &buf, (flags & OSHOT_UI_PASSWORD) ? ImGuiInputTextFlags_Password : 0);
    if (!changed)
        return false;

    oshot_str_t str = str_new(buf);
    if (!str.p)
        return false;  // out of memory: the plugin keeps its old string
    str_free(value);
    *value = str;
    return true;
}

static bool ui_combo(oshot_ctx_t* ctx, oshot_str_t label, int32_t* current, const oshot_str_t* items, size_t n)
{
    if (!current || (!items && n > 0) || !ui_check(ctx, "ui_combo"))
        return false;

    const bool        valid   = *current >= 0 && static_cast<size_t>(*current) < n;
    const std::string preview = valid ? to_string(items[*current]) : std::string();
    bool              changed = false;
    if (ImGui::BeginCombo(to_string(label).c_str(), preview.c_str()))
    {
        for (size_t i = 0; i < n; ++i)
        {
            const bool selected = valid && static_cast<size_t>(*current) == i;
            ImGui::PushID(static_cast<int>(i));  // items may repeat
            if (ImGui::Selectable(to_string(items[i]).c_str(), selected))
            {
                *current = static_cast<int32_t>(i);
                changed  = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static bool ui_slider_int(oshot_ctx_t* ctx, oshot_str_t label, int64_t* value, int64_t min, int64_t max)
{
    return value && ui_check(ctx, "ui_slider_int") &&
           ImGui::SliderScalar(
               to_string(label).c_str(), ImGuiDataType_S64, value, &min, &max, nullptr, ImGuiSliderFlags_AlwaysClamp);
}

static bool ui_slider_float(oshot_ctx_t* ctx, oshot_str_t label, double* value, double min, double max)
{
    return value && ui_check(ctx, "ui_slider_float") &&
           ImGui::SliderScalar(
               to_string(label).c_str(), ImGuiDataType_Double, value, &min, &max, "%.3f", ImGuiSliderFlags_AlwaysClamp);
}

static bool ui_color(oshot_ctx_t* ctx, oshot_str_t label, float rgba[4])
{
    return rgba && ui_check(ctx, "ui_color") && ImGui::ColorEdit4(to_string(label).c_str(), rgba);
}

static void ui_progress(oshot_ctx_t* ctx, float fraction, oshot_str_t overlay)
{
    if (!ui_check(ctx, "ui_progress"))
        return;
    const std::string text = to_string(overlay);
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), text.empty() ? nullptr : text.c_str());
}

static void ui_separator(oshot_ctx_t* ctx, oshot_str_t label)
{
    if (!ui_check(ctx, "ui_separator"))
        return;
    if (!label.p || label.len == 0)
        ImGui::Separator();
    else
        ImGui::SeparatorText(to_string(label).c_str());
}

static void ui_same_line(oshot_ctx_t* ctx)
{
    if (ui_check(ctx, "ui_same_line"))
        ImGui::SameLine();
}

static void ui_tooltip(oshot_ctx_t* ctx, oshot_str_t text)
{
    if (ui_check(ctx, "ui_tooltip"))
        ImGui::SetItemTooltip("%s", to_string(text).c_str());
}

static bool ui_begin_section(oshot_ctx_t* ctx, oshot_str_t label)
{
    if (!ui_check(ctx, "ui_begin_section"))
        return false;
    const bool open = ImGui::TreeNodeEx(to_string(label).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
    rt_of(ctx).sections.push_back(open);
    return open;
}

static void ui_end_section(oshot_ctx_t* ctx)
{
    if (!ui_check(ctx, "ui_end_section"))
        return;

    plugin_runtime_t& rt = rt_of(ctx);
    if (rt.sections.empty())
    {
        rt.WarnOnce(UiWarning::ExtraEnd, "ui_end_section() without ui_begin_section(), ignored");
        return;
    }
    if (rt.sections.back())
        ImGui::TreePop();
    rt.sections.pop_back();
}

const oshot_host_t g_oshot_host = {
    .struct_size   = sizeof(oshot_host_t),
    .abi_version   = OSHOT_API_VERSION,
    .oshot_version = VERSION,
    .str_new       = [](const char* s, size_t n) { return str_new(s, n); },
    .str_free         = str_free,
    .value_array_free = value_array_free,
    .log              = host_log,
    .get_data_dir     = get_data_dir,
    .run_on_ui_thread = run_on_ui_thread,
    .get_string       = get_string,
    .get_bool         = get_bool,
    .get_int64        = get_int64,
    .get_double       = get_double,
    .get_array        = get_array,
    .get_value        = get_value,
    .set_string       = set_string,
    .set_bool         = set_bool,
    .set_int64        = set_int64,
    .set_double       = set_double,
    .set_value        = set_value,
    .get_text         = get_text,
    .set_text         = set_text,
    .get_capture      = get_capture,
    .capture_free     = capture_free,
    .ui_text          = ui_text,
    .ui_button        = ui_button,
    .ui_checkbox      = ui_checkbox,
    .ui_input_text    = ui_input_text,
    .ui_combo         = ui_combo,
    .ui_slider_int    = ui_slider_int,
    .ui_slider_float  = ui_slider_float,
    .ui_color         = ui_color,
    .ui_progress      = ui_progress,
    .ui_separator     = ui_separator,
    .ui_same_line     = ui_same_line,
    .ui_tooltip       = ui_tooltip,
    .ui_begin_section = ui_begin_section,
    .ui_end_section   = ui_end_section,
};
