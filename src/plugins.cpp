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

#  include <cstddef>

#  include "imgui.h"
#  include "manifest.hh"
#  include "plugin.hpp"
#  include "state_manager.hh"
#  include "util.hpp"

static_assert(OSHOT_IMGUI_VERSION == IMGUI_VERSION_NUM,
              "Update OSHOT_IMGUI_VERSION in src/plugins/oshot_plugin.h together with imgui");

// From libcimgui. oshot itself doesn't need cimgui, but calling into it makes
// liboshot_common depend on it, so it's loaded before any plugin is: plugins
// then find its symbols without linking it (or without finding it on their
// own, e.g. inside the AppImage).
extern "C" const char* igGetVersion(void);

static std::thread::id g_plugin_ui_thread;

fs::path get_plugins_dir()
{
    return get_config_dir() / "plugins";
}

bool on_plugin_ui_thread()
{
    return std::this_thread::get_id() == g_plugin_ui_thread;
}

plugin_runtime_t::~plugin_runtime_t()
{
    if (initialized && plugin->destroy)
        plugin->destroy(state);
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
    std::vector<std::pair<void (*)(void*), void*>> todo;
    {
        std::scoped_lock lock(tasks_mutex);
        todo.swap(tasks);
    }
    for (auto& [fn, user] : todo)
        fn(user);
}

Result<> validate_plugin_descriptor(const oshot_plugin_t* plugin)
{
    if (!plugin)
        return Err("oshot_plugin_entry() returned NULL");
    if (plugin->abi_version != OSHOT_API_VERSION)
        return Err("it's built for plugin ABI {}, but this oshot has ABI {}", plugin->abi_version, OSHOT_API_VERSION);
    if (plugin->struct_size < sizeof(oshot_plugin_t))
        return Err("its oshot_plugin_t is {} bytes, expected at least {}: rebuild it with the current oshot_plugin.h",
                   plugin->struct_size,
                   sizeof(oshot_plugin_t));
    if (plugin->imgui_version == 0)
    {
        if (plugin->render || plugin->render_preferences)
            return Err("imgui_version is 0, but it has render functions: set it to OSHOT_IMGUI_VERSION");
    }
    else if (plugin->imgui_version != IMGUI_VERSION_NUM)
    {
        return Err("it's built for Dear ImGui {}, but oshot uses {}: rebuild it with this oshot's SDK",
                   plugin->imgui_version,
                   IMGUI_VERSION_NUM);
    }
    if (!plugin->id || !Manifest::IsValidID(plugin->id))
        return Err("its ID '{}' is invalid", plugin->id ? plugin->id : "(null)");
    if (!plugin->name || plugin->name[0] == '\0')
        return Err("it has no name");
    return Ok();
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
}

#endif
