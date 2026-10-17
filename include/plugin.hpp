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
#  include <map>
#  include <mutex>
#  include <thread>
#  include <utility>
#  include <vector>

#  ifndef OSHOT_HOST
#    define OSHOT_HOST
#  endif
#  include "../src/plugins/oshot_plugin.h"
#  include "dylib.hpp"
#  include "toml_api.hpp"

class StateManager;

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
    std::mutex                                     tasks_mutex;
    std::vector<std::pair<void (*)(void*), void*>> tasks;

    std::string id;
    fs::path    config_path;
    bool        initialized  = false;
    bool        config_dirty = false;

    plugin_runtime_t(std::string id, fs::path dir, const oshot_plugin_t* plugin, dylib::library lib)
        : lib(std::move(lib)), dir(std::move(dir)), plugin(plugin), id(std::move(id))
    {}

    // Calls plugin->destroy() if it was initialized, then SaveConfig()
    ~plugin_runtime_t();

    plugin_runtime_t(const plugin_runtime_t&)            = delete;
    plugin_runtime_t& operator=(const plugin_runtime_t&) = delete;

    // Runs the queued run_on_ui_thread() tasks. Call it on the UI thread
    void RunUiTasks();

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

/* Check whether the caller is on the thread plugins were loaded on (their UI thread) */
bool on_plugin_ui_thread();

#endif
