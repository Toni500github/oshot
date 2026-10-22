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
#  include <condition_variable>
#  include <cstddef>
#  include <functional>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <span>
#  include <string>
#  include <thread>
#  include <utility>
#  include <variant>
#  include <vector>

#  ifndef OSHOT_HOST
#    define OSHOT_HOST
#  endif
#  include "../src/plugins/oshot_plugin.h"
#  include "dylib.hpp"
#  include "toml_api.hpp"

class StateManager;

// Whether a plugin's descriptor has `field`: one built with an older
// oshot_plugin.h is shorter and ends before the fields added since
#  define OSHOT_PLUGIN_HAS(plugin, field) \
      (offsetof(oshot_plugin_t, field) + sizeof(((oshot_plugin_t*)0)->field) <= (plugin)->struct_size)

// The value of a declarative setting: CHOICE settings hold the chosen string
using setting_value_t = std::variant<bool, int64_t, double, std::string>;

// Mistakes in a plugin's UI code, each logged only once per plugin
enum class UiWarning : uint32_t  // NOLINT
{
    OutsideRender = 1 << 0,
    OffThread     = 1 << 1,
    OpenSection   = 1 << 2,
    ExtraEnd      = 1 << 3,
    ImGuiStack    = 1 << 4,
};

// A child process run_process() started, see oshot_plugins.cc
struct plugin_process_t;

// A plugin's run_process() children, shared with the threads waiting for
// them, which only reach the plugin through here: they can outlive it
struct plugin_processes_t
{
    std::mutex mutex;

    // Signaled when a process exits
    std::condition_variable cv;

    // The on_exit calls of the processes that exited, run by RunUiTasks()
    std::vector<std::function<void()>> exited;

    // Set when the plugin unloads: no more processes start, and on_exit isn't called
    bool closed = false;

    std::map<int64_t, std::shared_ptr<plugin_process_t>> running;
    int64_t                                              next_id = 1;
};

// A loaded plugin. Lives in g_plugins from load until oshot exits; its address
// is the oshot_ctx_t the plugin gets, so it must never move.
struct plugin_runtime_t
{
    // Declared first so it's destroyed last: the library is unloaded after everything else here
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

    // While render() or render_preferences() runs: the ui_* functions only work then
    bool in_ui = false;

    // The ui_begin_section() calls not ended yet, and whether each one is open
    std::vector<bool> sections;

    // UiWarning bits already logged; atomic because the ui_* functions can be called from any thread
    std::atomic<uint32_t> warned = 0;

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

    /* Call plugin->on_capture_end() if CaptureStart() was called since the last time */
    void CaptureEnd();

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
     * @return What render_preferences() returned: true while it has unsaved changes
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
 * Unload every plugin: on_capture_end() if a capture is open, kill their
 * processes, destroy(). Call it before main() returns, while the logger exists
 **/
void unload_plugins();

/* Check whether the caller is on the thread plugins were loaded on (their UI thread) */
bool on_plugin_ui_thread();

#endif
