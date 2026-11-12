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

#ifndef DISABLE_PLUGINS

#  include <algorithm>
#  include <array>
#  include <atomic>
#  include <chrono>
#  include <cstdint>
#  include <cstdlib>
#  include <cstring>
#  include <filesystem>
#  include <functional>
#  include <iterator>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <optional>
#  include <set>
#  include <span>
#  include <string>
#  include <string_view>
#  include <system_error>
#  include <thread>
#  include <utility>
#  include <variant>
#  include <vector>

#  include "dylib.hpp"
#  include "fmt/format.h"
#  include "imgui.h"
#  include "imgui_internal.h"
#  include "manifest.hh"
#  include "plugin.hpp"
#  include "screen_capture.hpp"
#  include "spdlog/spdlog.h"
#  include "state_manager.hh"
#  include "toml_api.hpp"
#  include "util.hpp"

using namespace std::chrono_literals;

static_assert(OSHOT_IMGUI_VERSION == IMGUI_VERSION_NUM,
              "Update OSHOT_IMGUI_VERSION in src/plugins/oshot_plugin.h together with imgui");

// From libcimgui. oshot itself doesn't need cimgui, but calling into it makes
// liboshot_common depend on it, so it's loaded before any plugin is. Plugins
// then find its symbols without linking it (or without finding it on their
// own, e.g. inside the AppImage).
extern "C" const char* igGetVersion(void);

static std::thread::id   g_plugin_ui_thread;
static std::atomic<bool> g_plugin_overlay_open = false;

std::vector<text_transformer_t> g_text_transformers;

// A transformer slower than this is logged, since the chain blocks the UI thread
static constexpr auto SLOW_TRANSFORM = 200ms;

// The size of oshot_plugin_t in the plugin API's first version, which every
// plugin's descriptor has at least; fields after it are checked with OSHOT_PLUGIN_HAS
static constexpr size_t PLUGIN_DESC_MIN_SIZE = offsetof(oshot_plugin_t, settings);

fs::path get_plugins_dir()
{
    return get_config_dir() / "plugins";
}

bool on_plugin_ui_thread()
{
    return std::this_thread::get_id() == g_plugin_ui_thread;
}

void set_plugin_overlay_open(bool open)
{
    g_plugin_overlay_open = open;
}

bool plugin_overlay_open()
{
    return g_plugin_overlay_open;
}

plugin_runtime_t::~plugin_runtime_t()
{
    if (initialized)
    {
        // Unloading drops them instead of calling them with no image
        image_requests.clear();
        CaptureEnd();
        KillProcesses();
        if (plugin->destroy)
            plugin->destroy(state);
    }
    SaveConfig();
}

void plugin_runtime_t::SaveConfig()
{
    if (!config_dirty)
        return;
    if (!config.SaveFile(config_path.string()))
        spdlog::error("Failed to save {}", config_path.string());
    config_dirty = false;
}

void plugin_runtime_t::RunUiTasks()
{
    std::vector<std::function<void()>> todo;
    {
        std::scoped_lock lock(tasks_mutex);
        todo.swap(tasks);
    }
    {
        std::scoped_lock lock(processes->mutex);
        std::ranges::move(processes->exited, std::back_inserter(todo));
        processes->exited.clear();
    }
    for (const std::function<void()>& fn : todo)
        fn();
}

void plugin_runtime_t::CaptureStart()
{
    in_capture = true;
    if (OSHOT_PLUGIN_HAS(plugin, on_capture_start) && plugin->on_capture_start)
        plugin->on_capture_start(state);
}

void plugin_runtime_t::CaptureEnd()
{
    // The overlay closed before the frame was read back
    DeliverFinalImage({});
    {
        std::scoped_lock lock(reports_mutex);
        reports.clear();
    }

    if (!std::exchange(in_capture, false))
        return;
    if (OSHOT_PLUGIN_HAS(plugin, on_capture_end) && plugin->on_capture_end)
        plugin->on_capture_end(state);
}

void plugin_runtime_t::DeliverFinalImage(const capture_result_t& image)
{
    // Swapped out first, so an on_ready() that asks again waits for the next frame
    std::vector<plugin_image_request_t> todo;
    todo.swap(image_requests);
    for (const plugin_image_request_t& req : todo)
    {
        oshot_capture_t cap{};
        if (!image.data.empty() && image.w > 0 && image.h > 0)
        {
            cap.rgba = static_cast<uint8_t*>(std::malloc(image.data.size()));
            if (cap.rgba)
            {
                std::memcpy(cap.rgba, image.data.data(), image.data.size());
                cap.w = image.w;
                cap.h = image.h;
            }
        }
        req.on_ready(req.user, cap);
    }
}

void plugin_runtime_t::Report(plugin_report_t report)
{
    std::scoped_lock lock(reports_mutex);
    auto             it = std::ranges::find(reports, report.action, &plugin_report_t::action);
    if (it != reports.end())
        *it = std::move(report);
    else
        reports.push_back(std::move(report));
}

const plugin_action_t* plugin_runtime_t::FindAction(int32_t action_id) const
{
    auto it = std::ranges::find(actions, action_id, &plugin_action_t::id);
    return it != actions.end() ? &*it : nullptr;
}

void plugin_runtime_t::WarnOnce(UiWarning warning, std::string_view msg)
{
    const auto bit = static_cast<uint32_t>(warning);
    if (!(warned.fetch_or(bit) & bit))
        spdlog::error("{}: {} (logged once)", id, msg);
}

// The ImGui stacks whose size differs, e.g. "window, ID"
static std::string unbalanced_stacks(const ImGuiErrorRecoveryState& a, const ImGuiErrorRecoveryState& b)
{
    constexpr std::array stacks = std::to_array<std::pair<short ImGuiErrorRecoveryState::*, const char*>>({
        { &ImGuiErrorRecoveryState::SizeOfWindowStack, "window" },
        { &ImGuiErrorRecoveryState::SizeOfIDStack, "ID" },
        { &ImGuiErrorRecoveryState::SizeOfTreeStack, "tree" },
        { &ImGuiErrorRecoveryState::SizeOfColorStack, "style color" },
        { &ImGuiErrorRecoveryState::SizeOfStyleVarStack, "style var" },
        { &ImGuiErrorRecoveryState::SizeOfFontStack, "font" },
        { &ImGuiErrorRecoveryState::SizeOfFocusScopeStack, "focus scope" },
        { &ImGuiErrorRecoveryState::SizeOfGroupStack, "group" },
        { &ImGuiErrorRecoveryState::SizeOfItemFlagsStack, "item flags" },
        { &ImGuiErrorRecoveryState::SizeOfBeginPopupStack, "popup" },
        { &ImGuiErrorRecoveryState::SizeOfDisabledStack, "disabled" },
    });

    std::string ret;
    for (const auto& [field, name] : stacks)
        if (a.*field != b.*field)
            ret += ret.empty() ? name : fmt::format(", {}", name);
    return ret;
}

// Runs a UI callback of the plugin inside its own ID scope, with the ui_*
// functions enabled, then closes the sections and ImGui stacks it left open
// so they don't break the rest of oshot's frame
template <typename F>
static bool call_plugin_ui(plugin_runtime_t& rt, F&& fn)
{
    ImGui::PushID(rt.id.c_str());
    ImGuiErrorRecoveryState before;
    ImGui::ErrorRecoveryStoreState(&before);

    rt.in_ui       = true;
    const bool ret = fn();
    rt.in_ui       = false;

    if (!rt.sections.empty())
    {
        rt.WarnOnce(UiWarning::OpenSection, "ui_begin_section() without ui_end_section(), closed by oshot");
        for (; !rt.sections.empty(); rt.sections.pop_back())
            if (rt.sections.back())
                ImGui::TreePop();
    }

    // The stacks only differ if the plugin called cimgui
    ImGuiErrorRecoveryState after;
    ImGui::ErrorRecoveryStoreState(&after);
    if (std::memcmp(&before, &after, sizeof(before)) != 0)
    {
        rt.WarnOnce(UiWarning::ImGuiStack,
                    fmt::format("left Dear ImGui's {} stack unbalanced (a Begin or Push without its End or Pop), "
                                "oshot closed what it could",
                                unbalanced_stacks(before, after)));

        // Recover without asserting or showing ImGui's error tooltip every frame, it's already logged above
        ImGuiIO&   io      = ImGui::GetIO();
        const bool assert  = std::exchange(io.ConfigErrorRecoveryEnableAssert, false);
        const bool tooltip = std::exchange(io.ConfigErrorRecoveryEnableTooltip, false);
        ImGui::ErrorRecoveryTryToRecoverState(&before);
        io.ConfigErrorRecoveryEnableAssert  = assert;
        io.ConfigErrorRecoveryEnableTooltip = tooltip;
    }

    ImGui::PopID();
    return ret;
}

void plugin_runtime_t::Render()
{
    call_plugin_ui(*this, [this] {
        plugin->render(state);
        return false;
    });
}

bool plugin_runtime_t::RenderPreferences()
{
    return call_plugin_ui(*this, [this] { return plugin->render_preferences(state); });
}

std::span<const oshot_setting_t> plugin_runtime_t::Settings() const
{
    if (!OSHOT_PLUGIN_HAS(plugin, n_settings) || !plugin->settings)
        return {};
    return { plugin->settings, plugin->n_settings };
}

void plugin_runtime_t::LoadSettings()
{
    settings_saved.clear();
    for (const oshot_setting_t& s : Settings())
    {
        switch (s.kind)
        {
            case OSHOT_SETTING_BOOL:
                settings_saved.emplace_back(config.GetValue<bool>(s.key, s.fallback.b, true));
                break;
            case OSHOT_SETTING_INT:
                settings_saved.emplace_back(config.GetValue<int64_t>(s.key, s.fallback.i, true));
                break;
            case OSHOT_SETTING_FLOAT:
                settings_saved.emplace_back(config.GetValue<double>(s.key, s.fallback.d, true));
                break;
            default:
                const std::string fallback = s.fallback.s.p ? std::string(s.fallback.s.p, s.fallback.s.len) : "";
                settings_saved.emplace_back(config.GetValue<std::string>(s.key, fallback, true));
                break;
        }
    }
    settings_editing = settings_saved;
}

void plugin_runtime_t::StoreSettings()
{
    const std::span<const oshot_setting_t> settings = Settings();
    for (size_t i = 0; i < settings.size() && i < settings_editing.size(); ++i)
        std::visit([&](const auto& val) { config.SetValue(settings[i].key, val); }, settings_editing[i]);
    settings_saved = settings_editing;
    config_dirty   = true;
}

// The declarative settings are drawn and saved by oshot, so they're checked
// up front instead of when they're drawn
static Result<> validate_settings(const oshot_plugin_t* plugin)
{
    if (!OSHOT_PLUGIN_HAS(plugin, n_settings) || plugin->n_settings == 0)
        return Ok();
    if (!plugin->settings)
        return Err("n_settings is {}, but settings is NULL", plugin->n_settings);
    if (plugin->n_settings > 256)
        return Err("it has {} settings, more than the 256 allowed", plugin->n_settings);

    std::set<std::string_view> keys;
    for (size_t i = 0; i < plugin->n_settings; ++i)
    {
        const oshot_setting_t& s = plugin->settings[i];
        if (!s.key || s.key[0] == '\0')
            return Err("setting {} has no key", i);
        if (!keys.insert(s.key).second)
            return Err("two settings have the key '{}'", s.key);
        if (!s.label)
            return Err("setting '{}' has no label", s.key);

        oshot_value_kind_t kind;
        switch (s.kind)
        {
            case OSHOT_SETTING_BOOL:   kind = OSHOT_VAL_BOOL; break;
            case OSHOT_SETTING_INT:    kind = OSHOT_VAL_INT64; break;
            case OSHOT_SETTING_FLOAT:  kind = OSHOT_VAL_DOUBLE; break;
            case OSHOT_SETTING_STRING:
            case OSHOT_SETTING_CHOICE: kind = OSHOT_VAL_STRING; break;
            default: return Err("setting '{}' has an unknown kind {}", s.key, static_cast<int>(s.kind));
        }
        if (s.fallback.kind != kind)
            return Err("setting '{}' has a fallback of the wrong kind", s.key);

        if (s.kind == OSHOT_SETTING_CHOICE)
        {
            if (!s.choices || s.n_choices == 0)
                return Err("setting '{}' is a CHOICE without choices", s.key);
            for (size_t j = 0; j < s.n_choices; ++j)
                if (!s.choices[j])
                    return Err("setting '{}' has a NULL choice", s.key);
        }
        if ((s.kind == OSHOT_SETTING_INT || s.kind == OSHOT_SETTING_FLOAT) && !(s.min <= s.max))
            return Err("setting '{}' has a min greater than its max", s.key);
    }
    return Ok();
}

Result<> validate_plugin_descriptor(const oshot_plugin_t* plugin)
{
    if (!plugin)
        return Err("oshot_plugin_entry() returned NULL");
    if (plugin->abi_version != OSHOT_API_VERSION)
        return Err("it's built for plugin ABI {}, but this oshot has ABI {}", plugin->abi_version, OSHOT_API_VERSION);
    if (plugin->struct_size < PLUGIN_DESC_MIN_SIZE)
        return Err("its oshot_plugin_t is {} bytes, expected at least {}: rebuild it with the current oshot_plugin.h",
                   plugin->struct_size,
                   PLUGIN_DESC_MIN_SIZE);

    if (plugin->imgui_version != OSHOT_NO_IMGUI && plugin->imgui_version != IMGUI_VERSION_NUM)
        return Err("it uses cimgui built for Dear ImGui {}, but oshot {} uses {}: rebuild it with this oshot's SDK",
                   plugin->imgui_version,
                   VERSION,
                   IMGUI_VERSION_NUM);

    if (!plugin->id || !Manifest::IsValidID(plugin->id))
        return Err("its ID '{}' is invalid", plugin->id ? plugin->id : "(null)");
    if (!plugin->name || plugin->name[0] == '\0')
        return Err("it has no name");
    return validate_settings(plugin);
}

// The library and its validated descriptor
static Result<std::pair<dylib::library, const oshot_plugin_t*>> open_plugin(const fs::path& path)
{
    try
    {
        dylib::library        lib(path);
        const oshot_plugin_t* plugin = lib.get_function<const oshot_plugin_t*()>("oshot_plugin_entry")();
        TRY(validate_plugin_descriptor(plugin));
        return Ok(std::make_pair(std::move(lib), plugin));
    }
    catch (const dylib::load_error& e)
    {
        return Err("can't load it: {}", e.what());
    }
    catch (const dylib::symbol_error&)
    {
        return Err("it doesn't export oshot_plugin_entry()");
    }
}

Result<plugin_info_t> inspect_plugin_library(const fs::path& path)
{
    auto res = open_plugin(path);
    if (!res.ok())
        return Err(res.error_v());
    const oshot_plugin_t* plugin = res.get().second;
    return Ok(plugin_info_t{ .id = plugin->id, .name = plugin->name });
}

// Directories left behind by an uninstall that couldn't delete a loaded
// library (Windows), and libraries replaced while they were loaded
static void remove_leftovers(const StateManager& state)
{
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(get_plugins_dir(), ec))
    {
        const std::string name = entry.path().filename().string();
        if (!entry.is_directory(ec) || !Manifest::IsValidID(name))
            continue;

        if (!state.FindPlugin(name))
        {
            spdlog::info("Removing the leftovers of uninstalled plugin '{}'", name);
            remove_tree(entry.path());
            continue;
        }

        for (const fs::directory_entry& file : fs::directory_iterator(entry.path(), ec))
            if (file.path().extension() == ".old")
                fs::remove(file.path(), ec);
    }
}

static void load_plugin(const plugin_t& info)
{
    auto res = open_plugin(info.library);
    if (!res.ok())
    {
        spdlog::error("Not loading plugin '{}' ({}): {}", info.id, info.library.string(), res.error_v());
        return;
    }

    auto& [lib, plugin] = res.get();
    if (info.id != plugin->id)
    {
        spdlog::error("Not loading plugin '{}': its library says its ID is '{}'", info.id, plugin->id);
        return;
    }

    auto [it, inserted] = g_plugins.try_emplace(info.id, info.id, info.library.parent_path(), plugin, std::move(lib));
    if (!inserted)
        return;

    plugin_runtime_t& rt = it->second;
    rt.config_path       = rt.dir / "config.toml";

    std::error_code ec;
    if (fs::exists(rt.config_path, ec))
        MUST_OK(rt.config.LoadFile(rt.config_path.string()),
                spdlog::error("Plugin '{}' config: {}. Using defaults", info.id, _r.error_v()));
    rt.LoadSettings();

    if (plugin->init)
    {
        void* state = nullptr;
        if (!plugin->init(&g_oshot_host, reinterpret_cast<oshot_ctx_t*>(&rt), &state))
        {
            spdlog::error("Plugin '{}' failed to initialize, not loading it", info.id);
            rt.config_dirty = false;
            g_plugins.erase(it);
            return;
        }
        rt.state = state;
    }
    rt.initialized = true;

    spdlog::info("Loaded plugin '{}' from {}", info.id, info.library.string());
}

void sort_text_transformers(const StateManager& state)
{
    std::vector<std::string> available;
    for (const auto& [id, rt] : g_plugins)
        if (OSHOT_PLUGIN_HAS(rt.plugin, transform_text) && rt.plugin->transform_text)
            available.push_back(id);

    g_text_transformers.clear();
    for (const auto& [id, enabled] : arrange_text_transformers(available, state.GetTextTransformers()))
        g_text_transformers.push_back({ .rt = &g_plugins.at(id), .enabled = enabled });
}

std::string run_text_steps(std::string                                                               text,
                           std::span<const text_step_t>                                              steps,
                           std::chrono::milliseconds                                                 slow,
                           const std::function<void(const text_step_t&, std::chrono::milliseconds)>& on_slow,
                           const std::function<void(const text_step_t&)>&                            on_bad)
{
    for (const text_step_t& step : steps)
    {
        oshot_str_t out{ .p = nullptr, .len = 0 };
        const auto  start   = std::chrono::steady_clock::now();
        const bool  changed = step.fn({ .p = text.c_str(), .len = text.size() }, &out);
        const auto  took =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (took > slow)
            on_slow(step, took);

        if (changed && (out.p || out.len == 0))
            text.assign(out.p ? out.p : "", out.p ? out.len : 0);
        else if (changed)
            on_bad(step);
        // Freed even when it returned false, in case it allocated anyway
        g_oshot_host.str_free(&out);
    }
    return text;
}

std::string run_text_transformers(std::string text, const char* buffer_id)
{
    std::vector<text_step_t> steps;
    for (const text_transformer_t& t : g_text_transformers)
    {
        if (!t.enabled)
            continue;
        plugin_runtime_t* rt = t.rt;
        steps.push_back({ .id = rt->id, .fn = [rt, buffer_id](oshot_str_t in, oshot_str_t* out) {
                             return rt->plugin->transform_text(rt->state, buffer_id, in, out);
                         } });
    }
    if (steps.empty())
        return text;

    auto on_slow = [](const text_step_t& step, std::chrono::milliseconds took) {
        g_plugins.at(step.id).WarnOnce(
            UiWarning::SlowTransform,
            fmt::format("transform_text() took {} ms, which freezes the overlay. Keep it under {} ms",
                        took.count(),
                        SLOW_TRANSFORM.count()));
    };
    auto on_bad = [](const text_step_t& step) {
        g_plugins.at(step.id).WarnOnce(UiWarning::BadTransform,
                                       "transform_text() returned true with a NULL `out`, its text was ignored");
    };
    return run_text_steps(std::move(text), steps, SLOW_TRANSFORM, on_slow, on_bad);
}

void load_plugins(const StateManager& state)
{
    g_plugin_ui_thread = std::this_thread::get_id();

    std::error_code ec;
    fs::create_directories(get_plugins_dir(), ec);
    remove_leftovers(state);

    spdlog::debug("Loading plugins (cimgui for Dear ImGui {})", igGetVersion());
    for (const manifest_t& repo : state.GetAllRepos())
    {
        for (const plugin_t& plugin : repo.plugins)
        {
            if (!plugin.enabled)
                spdlog::info("Plugin '{}' is disabled", plugin.id);
            else if (plugin.library.empty())
                spdlog::warn("Plugin '{}' has no library in state.toml, reinstall it", plugin.id);
            else
                load_plugin(plugin);
        }
    }
    sort_text_transformers(state);
}

void unload_plugins()
{
    g_text_transformers.clear();
    g_plugins.clear();
}

#endif
