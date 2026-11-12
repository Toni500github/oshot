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

#pragma once

#ifndef DISABLE_PLUGINS
#  include <atomic>
#  include <chrono>
#  include <condition_variable>
#  include <cstddef>
#  include <cstdint>
#  include <filesystem>
#  include <functional>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <span>
#  include <string>
#  include <string_view>
#  include <utility>
#  include <variant>
#  include <vector>

#  ifndef OSHOT_HOST
#    define OSHOT_HOST
#  endif
#  include "../src/plugins/oshot_plugin.h"
#  include "dylib.hpp"
#  include "toml_api.hpp"
#  include "util/paths.hpp"
#  include "util/result.hpp"

class StateManager;
struct capture_result_t;

// Whether a plugin's descriptor has `field`, since one built with an older
// oshot_plugin.h ends before the fields added since
#  define OSHOT_PLUGIN_HAS(plugin, field) \
      (offsetof(oshot_plugin_t, field) + sizeof(((oshot_plugin_t*)0)->field) <= (plugin)->struct_size)

// The value of a declarative setting (CHOICE settings hold the chosen string)
using setting_value_t = std::variant<bool, int64_t, double, std::string>;

// Mistakes in a plugin's UI code, each logged only once per plugin
enum class UiWarning : uint32_t  // NOLINT
{
    OutsideRender = 1 << 0,
    OffThread     = 1 << 1,
    OpenSection   = 1 << 2,
    ExtraEnd      = 1 << 3,
    ImGuiStack    = 1 << 4,
    SlowTransform = 1 << 5,
    BadTransform  = 1 << 6,
};

// An action from register_action()
struct plugin_action_t
{
    // The button's image (RGBA, icon_w * icon_h * 4 bytes), empty for none
    std::vector<uint8_t> icon;

    std::string label;
    void (*fn)(void* user, int32_t action) = nullptr;
    void*         user                     = nullptr;
    int32_t       id                       = 0;
    int32_t       icon_w                   = 0;
    int32_t       icon_h                   = 0;
    oshot_place_t place                    = OSHOT_PLACE_TOOLBAR;
};

// What report_progress() or report_result() last said about an action, shown in the overlay
struct plugin_report_t
{
    // The progress text, or the result's message
    std::string text;

    // When the result came, for hiding it after a while
    std::chrono::steady_clock::time_point finished;

    std::string copyable;
    float       fraction = -1.0f;  // negative: unknown
    int32_t     action   = 0;      // 0: not tied to an action
    bool        done     = false;  // a result, not progress
    bool        ok       = false;
};

// A request_final_image() call waiting for the frame to be read back
struct plugin_image_request_t
{
    void (*on_ready)(void* user, oshot_capture_t image);
    void* user;
};

// A child process run_process() started, see oshot_plugins.cc
struct plugin_process_t;

// A plugin's run_process() children, shared with the threads waiting for
// them, which only reach the plugin through here (they can outlive it)
struct plugin_processes_t
{
    std::mutex mutex;

    // Signaled when a process exits
    std::condition_variable cv;

    // The on_exit calls of the processes that exited, run by RunUiTasks()
    std::vector<std::function<void()>> exited;

    // Set when the plugin unloads. No more processes start after that, and on_exit isn't called
    bool closed = false;

    std::map<int64_t, std::shared_ptr<plugin_process_t>> running;
    int64_t                                              next_id = 1;
};

// A loaded plugin. Lives in g_plugins from load until oshot exits, and its address
// is the oshot_ctx_t the plugin gets, so it must never move
struct plugin_runtime_t
{
    // Declared first, so the library is unloaded after everything else here
    dylib::library lib;

    // ~/.config/oshot/plugins/<id>: library, config.toml, plugin data
    fs::path dir;

    // Static in the library, per the ABI contract
    const oshot_plugin_t* plugin = nullptr;

    // Owned by the plugin, released by plugin->destroy()
    void* state = nullptr;

    // The plugin's config.toml, written to config_path by SaveConfig() when config_dirty
    TomlAPI config;

    // run_on_ui_thread() queue, emptied by RunUiTasks()
    std::mutex                         tasks_mutex;
    std::vector<std::function<void()>> tasks;

    std::shared_ptr<plugin_processes_t> processes = std::make_shared<plugin_processes_t>();

    // plugin->settings as they're in config.toml, and as Preferences shows them until saved
    std::vector<setting_value_t> settings_saved;
    std::vector<setting_value_t> settings_editing;

    // True while render() or render_preferences() runs, the only time the ui_* functions work
    bool in_ui = false;

    // The ui_begin_section() calls not ended yet, and whether each one is open
    std::vector<bool> sections;

    // UiWarning bits already logged, atomic because the ui_* functions can be called from any thread
    std::atomic<uint32_t> warned = 0;

    // register_action()'s actions, in the order they were added. UI thread only
    std::vector<plugin_action_t> actions;

    // The ID the next register_action() returns. Atomic since report_*() check IDs against it from any thread
    std::atomic<int32_t> next_action_id = 1;

    // report_progress() and report_result(), one entry per action, written from any thread
    std::mutex                   reports_mutex;
    std::vector<plugin_report_t> reports;

    // request_final_image() calls waiting for DeliverFinalImage(). UI thread only
    std::vector<plugin_image_request_t> image_requests;

    std::string id;
    fs::path    config_path;
    bool        initialized  = false;
    bool        config_dirty = false;
    bool        in_capture   = false;  // between CaptureStart() and CaptureEnd()

    plugin_runtime_t(std::string id, fs::path dir, const oshot_plugin_t* plugin, dylib::library lib)
        : lib(std::move(lib)), dir(std::move(dir)), plugin(plugin), id(std::move(id))
    {}

    // Ends its capture, kills its processes, calls plugin->destroy() if it was initialized, then SaveConfig()
    ~plugin_runtime_t();

    plugin_runtime_t(const plugin_runtime_t&)            = delete;
    plugin_runtime_t& operator=(const plugin_runtime_t&) = delete;

    // Runs the queued run_on_ui_thread() tasks and on_exit calls. Call it on the UI thread
    void RunUiTasks();

    /* Call plugin->on_capture_start(), if it has one */
    void CaptureStart();

    /**
     * Call plugin->on_capture_end() if CaptureStart() was called since the last
     * time, after answering the pending request_final_image() calls with no image
     * and dropping the reports
     **/
    void CaptureEnd();

    /**
     * Answer the pending request_final_image() calls, each with its own copy
     * @param image The annotated selection, or an empty one when there's none
     **/
    void DeliverFinalImage(const capture_result_t& image);

    /**
     * Store what report_progress() or report_result() said, replacing the
     * action's last report
     * @param report The new report
     **/
    void Report(plugin_report_t report);

    /**
     * Find an action by ID
     * @return The action, or nullptr
     **/
    [[nodiscard]] const plugin_action_t* FindAction(int32_t id) const;

    /**
     * Kill the processes run_process() started, wait a moment for them to
     * exit, and drop their on_exit calls. Implemented in oshot_plugins.cc
     **/
    void KillProcesses();

    /**
     * Call plugin->render() inside the plugin's ImGui ID scope, with the ui_*
     * functions enabled, and close what it left open. Call it inside a window
     **/
    void Render();

    /**
     * The same for plugin->render_preferences()
     * @return What render_preferences() returned (true while it has unsaved changes)
     **/
    bool RenderPreferences();

    /**
     * Log a mistake in the plugin's UI code, the first time it happens
     * @param warning Which mistake
     * @param msg The message, after the plugin's ID
     **/
    void WarnOnce(UiWarning warning, std::string_view msg);

    /* The declarative settings, empty if the plugin has none or is too old for them */
    [[nodiscard]] std::span<const oshot_setting_t> Settings() const;

    /* Read the settings' values from config.toml into settings_saved and settings_editing */
    void LoadSettings();

    /* Write settings_editing into config.toml (on the next SaveConfig()) */
    void StoreSettings();

    void SaveConfig();
};

// A plugin's transform_text() in the chain, see arrange_text_transformers()
struct text_transformer_t
{
    plugin_runtime_t* rt;
    bool              enabled;
};

// One step of run_text_steps(): a transformer by its plugin's ID
struct text_step_t
{
    std::string                                    id;
    std::function<bool(oshot_str_t, oshot_str_t*)> fn;
};

// What inspect_plugin_library() read from a plugin's descriptor
struct plugin_info_t
{
    std::string id;
    std::string name;
};

// The loaded plugins, by ID
extern std::map<std::string, plugin_runtime_t> g_plugins;

// The host function table every plugin gets in init()
extern const oshot_host_t g_oshot_host;

// The loaded plugins that have a transform_text(), in the order the user set
extern std::vector<text_transformer_t> g_text_transformers;

/**
 * Get the directory plugins are installed in
 * @return get_config_dir() / "plugins"
 **/
fs::path get_plugins_dir();

/**
 * Check a plugin descriptor against what this oshot supports (ABI, ImGui version, ID)
 * @param plugin The descriptor oshot_plugin_entry() returned
 **/
Result<> validate_plugin_descriptor(const oshot_plugin_t* plugin);

/**
 * Load a plugin library without initializing it, and read its descriptor after
 * validate_plugin_descriptor(). Used by oshotpm before installing
 * @param path The plugin library
 **/
Result<plugin_info_t> inspect_plugin_library(const fs::path& path);

/**
 * Load every enabled plugin in state.toml into g_plugins.
 * Call it on the thread that will draw them, which becomes their UI thread
 * @param state The installed plugins
 **/
void load_plugins(const StateManager& state);

/**
 * Unload every plugin. That means on_capture_end() if a capture is open, killing
 * their processes, then destroy(). Call it before main() returns, while the logger exists
 **/
void unload_plugins();

/* Check whether the caller is on the thread plugins were loaded on (their UI thread) */
bool on_plugin_ui_thread();

/**
 * Set whether an overlay is open, so report_result() knows to fall back to a notification
 * @param open True from a capture's start to its end
 **/
void set_plugin_overlay_open(bool open);

/* Check whether an overlay is open, from any thread */
bool plugin_overlay_open();

/**
 * Rebuild g_text_transformers from the loaded plugins and the order in state.toml
 * @param state The installed plugins, with their [text-transformers] order
 **/
void sort_text_transformers(const StateManager& state);

/**
 * Pass text through transformers one after another, freeing what each returned
 * @param text What the scan read
 * @param steps The transformers, in order
 * @param slow How long a step may take before on_slow is called
 * @param on_slow Called with a step that took longer than `slow`, and how long it took
 * @param on_bad Called with a step that returned true without valid text, which is then skipped
 * @return The text after the last step
 **/
std::string run_text_steps(std::string                                                               text,
                           std::span<const text_step_t>                                              steps,
                           std::chrono::milliseconds                                                 slow,
                           const std::function<void(const text_step_t&, std::chrono::milliseconds)>& on_slow,
                           const std::function<void(const text_step_t&)>&                            on_bad);

/**
 * Run the enabled transformers of g_text_transformers over a scan's text, logging
 * the ones that are slow or broken once per plugin
 * @param text What the scan read
 * @param buffer_id OSHOT_TEXT_OCR or OSHOT_TEXT_BARCODE
 * @return The transformed text
 **/
std::string run_text_transformers(std::string text, const char* buffer_id);

#endif
