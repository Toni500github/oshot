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
#  include <cmath>
#  include <cstddef>
#  include <cstring>
#  include <iterator>
#  include <optional>
#  include <set>

#  include "imgui.h"
#  include "imgui_internal.h"
#  include "manifest.hh"
#  include "plugin.hpp"
#  include "screen_capture.hpp"
#  include "state_manager.hh"
#  include "util.hpp"

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
static constexpr std::chrono::milliseconds SLOW_TRANSFORM{ 100 };

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
    auto it = std::ranges::find(reports, report.action, &plugin_report_t::action);
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
    const auto stacks = std::to_array<std::pair<short ImGuiErrorRecoveryState::*, const char*>>({
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

// Runs a UI callback of the plugin inside its own ID scope, with the ui_* or
// draw_* functions enabled (`enabled`), then closes the sections and ImGui
// stacks it left open so they don't break the rest of oshot's frame
template <typename F>
static bool call_plugin_ui(plugin_runtime_t& rt, bool plugin_runtime_t::* enabled, F&& fn)
{
    ImGui::PushID(rt.id.c_str());
    ImGuiErrorRecoveryState before;
    ImGui::ErrorRecoveryStoreState(&before);

    rt.*enabled    = true;
    const bool ret = fn();
    rt.*enabled    = false;

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
    call_plugin_ui(*this, &plugin_runtime_t::in_ui, [this] {
        plugin->render(state);
        return false;
    });
}

bool plugin_runtime_t::RenderPreferences()
{
    return call_plugin_ui(*this, &plugin_runtime_t::in_ui, [this] { return plugin->render_preferences(state); });
}

void plugin_runtime_t::RenderOverlay(const oshot_overlay_info_t& info)
{
    call_plugin_ui(*this, &plugin_runtime_t::in_overlay, [this, &info] {
        plugin->render_overlay(state, &info);
        return false;
    });
}

const plugin_ocr_provider_t* plugin_runtime_t::FindOcrProvider(std::string_view provider_id) const
{
    auto it = std::ranges::find(ocr_providers, provider_id, &plugin_ocr_provider_t::id);
    return it != ocr_providers.end() ? &*it : nullptr;
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
            default:                   return Err("setting '{}' has an unknown kind {}", s.key, static_cast<int>(s.kind));
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
    // 0: it draws with the ui_* host functions or not at all
    if (plugin->imgui_version != 0 && plugin->imgui_version != IMGUI_VERSION_NUM)
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
        const auto  took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
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

// ------------------------------
// OCR engines
// ------------------------------

// Every OCR job until it's done and the overlay dropped it, so unload_plugins() can wait for them. UI thread only
static std::vector<std::shared_ptr<OcrJob>> g_ocr_jobs;

// The ID rules of oshot-plugin.toml's plugin IDs, without the '.' they require
static bool is_valid_provider_id(std::string_view id)
{
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

Result<> validate_ocr_provider(const oshot_ocr_provider_t* provider)
{
    // The fields of the first version, which every provider has
    constexpr size_t min_size = offsetof(oshot_ocr_provider_t, user) + sizeof(void*);

    if (!provider)
        return Err("the provider is NULL");
    if (provider->struct_size < min_size)
        return Err("its struct_size is {}, expected at least {}", provider->struct_size, min_size);
    if (!provider->id || !is_valid_provider_id(provider->id))
        return Err("its ID '{}' is invalid: use lowercase letters, digits, '-', '_' and '.', at most 64",
                   provider->id ? provider->id : "(null)");
    if (!provider->name || provider->name[0] == '\0')
        return Err("it has no name");
    if (!provider->recognize)
        return Err("recognize is NULL");
    if (provider->n_languages > 0 && !provider->languages)
        return Err("n_languages is {}, but languages is NULL", provider->n_languages);
    if (provider->n_languages > 1024)
        return Err("it has {} languages, more than the 1024 allowed", provider->n_languages);
    for (size_t i = 0; i < provider->n_languages; ++i)
        if (!provider->languages[i] || provider->languages[i][0] == '\0')
            return Err("language {} is NULL or empty", i);
    return Ok();
}

std::string ocr_engine_key(std::string_view plugin_id, std::string_view provider_id)
{
    return fmt::format("{}/{}", plugin_id, provider_id);
}

OcrEngineKind resolve_ocr_engine(std::string_view engine, std::span<const std::string> available)
{
    const size_t slash = engine.find('/');
    if (engine.empty() || engine == "tesseract" || slash == std::string_view::npos || slash == 0 ||
        slash + 1 == engine.size())
        return OcrEngineKind::Tesseract;
    return std::ranges::find(available, engine) != available.end() ? OcrEngineKind::Plugin : OcrEngineKind::Missing;
}

const plugin_ocr_provider_t* find_ocr_provider(std::string_view engine, plugin_runtime_t** rt)
{
    const size_t slash = engine.find('/');
    if (slash == std::string_view::npos)
        return nullptr;

    auto it = g_plugins.find(std::string(engine.substr(0, slash)));
    if (it == g_plugins.end())
        return nullptr;
    const plugin_ocr_provider_t* provider = it->second.FindOcrProvider(engine.substr(slash + 1));
    if (provider && rt)
        *rt = &it->second;
    return provider;
}

std::string pick_ocr_language(std::span<const std::string> languages,
                              const std::string&           picked,
                              const std::string&           tesseract_model)
{
    for (const std::string* want : { &picked, &tesseract_model })
        if (!want->empty() && std::ranges::find(languages, *want) != languages.end())
            return *want;
    return languages.empty() ? "" : languages.front();
}

std::vector<std::string> list_ocr_providers()
{
    std::vector<std::string> ret;
    for (const auto& [id, rt] : g_plugins)
        for (const plugin_ocr_provider_t& p : rt.ocr_providers)
            ret.push_back(ocr_engine_key(id, p.id));
    return ret;
}

void free_ocr_output(oshot_ocr_output_t& out)
{
    if (out.words)
        for (size_t i = 0; i < out.n_words; ++i)
            g_oshot_host.str_free(&out.words[i].text);
    g_oshot_host.mem_free(out.words);
    g_oshot_host.str_free(&out.text);
    out.words   = nullptr;
    out.n_words = 0;
}

ocr_result_t ocr_result_from_output(oshot_ocr_output_t&   out,
                                    int                   img_w,
                                    int                   img_h,
                                    const ocr_prepared_t* prep,
                                    bool*                 bad_words)
{
    ocr_result_t ret;
    ret.psm        = -1;
    ret.confidence = 0;
    bool bad       = !out.words && out.n_words > 0;

    const float src_w = prep ? float(prep->src_w) : float(img_w);
    const float src_h = prep ? float(prep->src_h) : float(img_h);
    for (size_t i = 0; out.words && i < out.n_words; ++i)
    {
        const oshot_ocr_word_t& w = out.words[i];
        const bool finite = std::isfinite(w.x) && std::isfinite(w.y) && std::isfinite(w.w) && std::isfinite(w.h) &&
                            w.w >= 0 && w.h >= 0;
        if (!finite || !w.text.p || w.text.len == 0)
        {
            bad = true;
            continue;
        }

        ocr_word_t word;
        if (prep)
        {
            word = map_prepared_box(*prep, w.x, w.y, w.w, w.h);
        }
        else
        {
            const float x0 = std::clamp(w.x, 0.0f, src_w), y0 = std::clamp(w.y, 0.0f, src_h);
            word.x = x0;
            word.y = y0;
            word.w = std::clamp(w.x + w.w, x0, src_w) - x0;
            word.h = std::clamp(w.y + w.h, y0, src_h) - y0;
        }
        word.text       = std::string(w.text.p, w.text.len);
        word.confidence = std::clamp(w.confidence, 0, 100);
        word.line       = std::max(w.line, 0);
        word.block      = std::max(w.block, 0);
        ret.words.push_back(std::move(word));
    }

    if (out.text.p && out.text.len > 0)
    {
        ret.data.assign(out.text.p, out.text.len);
    }
    else
    {
        // Spaces within a line, a line break between lines and an empty line between blocks
        for (size_t i = 0; i < ret.words.size(); ++i)
        {
            if (i > 0)
            {
                const ocr_word_t& prev = ret.words[i - 1];
                if (ret.words[i].block != prev.block)
                    ret.data += "\n\n";
                else if (ret.words[i].line != prev.line)
                    ret.data += '\n';
                else
                    ret.data += ' ';
            }
            ret.data += ret.words[i].text;
        }
    }

    if (out.confidence >= 0)
    {
        ret.confidence = std::min(out.confidence, 100);
    }
    else if (!ret.words.empty())
    {
        double sum = 0;
        for (const ocr_word_t& w : ret.words)
            sum += w.confidence;
        ret.confidence = int(std::round(sum / double(ret.words.size())));
    }

    if (bad_words)
        *bad_words = bad;
    free_ocr_output(out);
    return ret;
}

OcrJob::OcrJob(std::string plugin_id, plugin_ocr_provider_t provider, capture_result_t image, std::string language)
    : plugin_id(std::move(plugin_id)),
      provider(std::move(provider)),
      m_thread(&OcrJob::Run, this, std::move(image), std::move(language))
{}

OcrJob::~OcrJob()
{
    Cancel();
    Wait();
}

void OcrJob::Wait()
{
    if (m_thread.joinable())
        m_thread.join();
}

void OcrJob::Cancel()
{
    m_cancelled = true;
}

Result<ocr_result_t> OcrJob::TakeResult()
{
    if (!m_done)
        return Err("the OCR engine is still reading");
    return std::exchange(m_result, Err("already taken"));
}

void OcrJob::Run(capture_result_t image, std::string language)
{
    std::optional<ocr_prepared_t> prep;
    if (provider.flags & OSHOT_OCR_PREPARED)
    {
        Result<ocr_prepared_t> p = prepare_ocr_image(image);
        if (!p.ok())
        {
            m_result = Err(p.error_v());
            m_done   = true;
            return;
        }
        prep  = std::move(p.get());
        image = {};
    }

    const capture_result_t& img = prep ? prep->image : image;
    oshot_capture_t         cap{ .w = img.w, .h = img.h, .rgba = const_cast<uint8_t*>(img.data.data()) };

    const oshot_ocr_request_t req{
        .image    = &cap,
        .language = { .p = language.c_str(), .len = language.size() },
        .cancelled =
            [](const oshot_ocr_request_t* r) { return static_cast<const std::atomic<bool>*>(r->host_data)->load(); },
        .host_data = &m_cancelled,
    };

    oshot_ocr_output_t out{ .words = nullptr, .n_words = 0, .text = { .p = nullptr, .len = 0 }, .confidence = -1 };
    oshot_str_t        err{ .p = nullptr, .len = 0 };
    const auto         start = std::chrono::steady_clock::now();
    const oshot_err_t  code  = provider.recognize(provider.user, &req, &out, &err);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    if (m_cancelled)
    {
        spdlog::debug("{}: OCR engine '{}' cancelled after {} ms", plugin_id, provider.id, took.count());
        free_ocr_output(out);
        m_result = Err("cancelled");
    }
    else if (code != OSHOT_OK)
    {
        std::string msg = err.p && err.len > 0 ? std::string(err.p, err.len) : fmt::format("error {}", int(code));
        spdlog::warn("{}: OCR engine '{}' failed: {}", plugin_id, provider.id, msg);
        free_ocr_output(out);
        m_result = Err(std::move(msg));
    }
    else
    {
        bool         bad_words = false;
        ocr_result_t res       = ocr_result_from_output(out, img.w, img.h, prep ? &*prep : nullptr, &bad_words);
        if (bad_words)
            spdlog::warn("{}: OCR engine '{}' returned words without text or with an invalid box, skipped them",
                         plugin_id,
                         provider.id);
        spdlog::debug(
            "{}: OCR engine '{}' read {} words in {} ms", plugin_id, provider.id, res.words.size(), took.count());
        res.engine = provider.name;
        m_result   = Ok(std::move(res));
    }
    g_oshot_host.str_free(&err);
    m_done = true;
}

std::shared_ptr<OcrJob> start_ocr_job(const plugin_runtime_t&      rt,
                                      const plugin_ocr_provider_t& provider,
                                      capture_result_t             image,
                                      std::string                  language)
{
    reap_ocr_jobs();
    auto job = std::make_shared<OcrJob>(rt.id, provider, std::move(image), std::move(language));
    g_ocr_jobs.push_back(job);
    return job;
}

void cancel_ocr_jobs(std::string_view plugin_id, std::string_view provider_id)
{
    for (const std::shared_ptr<OcrJob>& job : g_ocr_jobs)
        if (job->plugin_id == plugin_id && (provider_id.empty() || job->provider.id == provider_id))
            job->Cancel();
}

void reap_ocr_jobs()
{
    // A job nobody else holds was cancelled or taken; only its thread may still run
    std::erase_if(g_ocr_jobs, [](const std::shared_ptr<OcrJob>& job) { return job.use_count() == 1 && job->Done(); });
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
    // Their recognize() calls into the plugins, so they end first
    for (const std::shared_ptr<OcrJob>& job : g_ocr_jobs)
        job->Cancel();
    for (const std::shared_ptr<OcrJob>& job : g_ocr_jobs)
        job->Wait();
    g_ocr_jobs.clear();

    g_text_transformers.clear();
    g_plugins.clear();
}

#endif
