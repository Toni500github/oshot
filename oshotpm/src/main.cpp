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

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <utility>

#include "fmt/compile.h"
#include "fmt/os.h"
#include "fmt/ranges.h"
#include "manifest.hh"
#include "plugin_manager.hh"
#include "state_manager.hh"
#include "texts.hpp"
#include "util.hpp"

/* If include before manifest.hpp
 * /usr/include/bits/getopt_core.h:91:12: error: declaration of ‘int getopt(int, char* const*, const char*) noexcept’ has a different exception specifier
 *  91 | extern int getopt (int ___argc, char *const *___argv, const char *__shortopts)
 *     |            ^~~~~~
 * In file included from oshot/oshotpm/src/main.cpp:33:
 * oshot/include/libs/getopt_port/getopt.h:50:5: note: from previous declaration ‘int getopt(int, char* const*, const char*)’
 *  50 | int getopt(int argc, char* const argv[], const char* optstring);
 *     |     ^~~~~~
 */
// clang-format off
#include "getopt_port/getopt.h"
// clang-format on

enum OPs : std::uint8_t
{
    NONE,
    INSTALL,
    UPDATE,
    LIST,
    ENABLE,
    DISABLE,
    UNINSTALL,
    GEN_MANIFEST,
    HELP
} op = NONE;

constexpr std::array<std::pair<std::string_view, OPs>, 8> map{ {
    { "install", INSTALL },
    { "update", UPDATE },
    { "list", LIST },
    { "help", HELP },
    { "enable", ENABLE },
    { "disable", DISABLE },
    { "uninstall", UNINSTALL },
    { "gen-manifest", GEN_MANIFEST },
} };

OPs str_to_enum(const std::string_view name)
{
    if (auto it = std::ranges::find(map, name, &std::pair<std::string_view, OPs>::first); it != map.end())
        return it->second;
    return NONE;
}

void version()
{
    fmt::print(FMT_COMPILE("{}"), version_infos);

    // if only everyone would not return error when querying the program version :(
    std::exit(EXIT_SUCCESS);
}

void help(int invalid_opt = false)
{
    fmt::print(FMT_COMPILE("{}"), oshotpm_help);

    std::exit(invalid_opt);
}

void help_install(int invalid_opt = false)
{
    fmt::print(FMT_COMPILE("{}"), oshotpm_help_install);

    std::exit(invalid_opt);
}

void help_list(int invalid_opt = false)
{
    fmt::print(FMT_COMPILE("{}"), oshotpm_help_list);

    std::exit(invalid_opt);
}

// NOLINTBEGIN
bool parse_install_args(int argc, char* argv[])
{
    // clang-format off
    const struct option long_opts[] = {
        {"force", no_argument, nullptr, 'f'},
        {"help",  no_argument, nullptr, 'h'},
        {"yes",   no_argument, nullptr, 'y'},
        {0, 0, 0, 0}
    };
    // clang-format on

    int opt;
    while ((opt = getopt_long(argc, argv, "+yfh", long_opts, nullptr)) != -1)
    {
        switch (opt)
        {
            case 'h': help_install(EXIT_SUCCESS); break;
            case '?': help_install(EXIT_FAILURE); break;

            case 'f': options.install_force = true; break;
            case 'y': options.install_shut_up = true; break;
        }
    }

    for (int i = optind; i < argc; ++i)
        options.arguments.emplace_back(argv[i]);

    if (options.arguments.empty())
        die("install: no repositories/paths given");

    return true;
}

bool parse_list_args(int argc, char* argv[])
{
    // clang-format off
    const struct option long_opts[] = {
        {"verbose", no_argument, nullptr, 'v'},
        {"help",    no_argument, nullptr, 'h'},
        {0, 0, 0, 0}
    };
    // clang-format on

    int opt;
    while ((opt = getopt_long(argc, argv, "+vh", long_opts, nullptr)) != -1)
    {
        switch (opt)
        {
            case 'v': options.list_verbose = true; break;
            case 'h': help_list(EXIT_SUCCESS); break;
            case '?': help_list(EXIT_FAILURE); break;
        }
    }

    return true;
}

bool parse_general_command_args(int argc, char* argv[])
{
    // clang-format off
    const struct option long_opts[] = {
        {"help",  no_argument, nullptr, 'h'},
        {0, 0, 0, 0}
    };
    // clang-format on

    int opt;
    while ((opt = getopt_long(argc, argv, "+h", long_opts, nullptr)) != -1)
    {
        switch (opt)
        {
            case 'h': help(EXIT_SUCCESS); break;
            case '?': help_install(EXIT_FAILURE); break;
        }
    }

    for (int i = optind; i < argc; ++i)
        options.arguments.emplace_back(argv[i]);

    return true;
}

static bool parseargs(int argc, char* argv[])
{
    // clang-format off
    int opt = 0;
    static const struct option opts[] = {
        {"version", no_argument, 0, 'V'},
        {"help",    no_argument, 0, 'h'},
        {0,0,0,0}
    };

    // clang-format on
    optind = 1;
    while ((opt = getopt_long(argc, argv, "+Vh", opts, nullptr)) != -1)
    {
        switch (opt)
        {
            case 0:   break;
            case '?': help(EXIT_FAILURE); break;

            case 'V': version(); break;
            case 'h': help(); break;
            default:  return false;
        }
    }

    if (optind >= argc)
        help(EXIT_FAILURE);  // no subcommand

    std::string_view cmd      = argv[optind];
    int              sub_argc = argc - optind;
    char**           sub_argv = argv + optind;

    op = str_to_enum(cmd);
    switch (op)
    {
        case INSTALL: optind = 0; return parse_install_args(sub_argc, sub_argv);
        case LIST:    optind = 0; return parse_list_args(sub_argc, sub_argv);
        case HELP:    break;
        default:      optind = 0; return parse_general_command_args(sub_argc, sub_argv);
    }

    if (op == HELP)
    {
        if (sub_argc >= 2)
        {
            const std::string_view target(sub_argv[1]);
            if (target == "install")
                help_install();
            else if (target == "list")
                help_list();
            else
                die("Couldn't find help text for subcommand '{}'", cmd);
        }
        else
        {
            help(EXIT_FAILURE);
        }
    }

    return true;
}
// NOLINTEND

static void switch_plugin_path(const std::string&     arg,
                               fs::path&              base_path,
                               bool                   switch_,
                               const std::string_view switch_str)

{
    if (base_path.extension() == ".disabled")
        base_path.replace_extension();  // normalize to enabled form

    const fs::path& enabled_path  = base_path;
    const fs::path& disabled_path = base_path.string() + ".disabled";

    fs::path current_path;
    if (fs::exists(enabled_path))
        current_path = enabled_path;
    else if (fs::exists(disabled_path))
        current_path = disabled_path;
    else
    {
        warn("Plugin library '{}' not found. Skipping", base_path.string());
        return;
    }

    const fs::path& target_path = switch_ ? enabled_path : disabled_path;
    if (current_path == target_path)
    {
        warn("{} is already {}ed", arg, switch_str);
        return;
    }

    fs::rename(current_path, target_path);
    info("{}ed {}!", switch_str, arg);
}

void switch_plugin(const StateManager& state, bool switch_)
{
    const std::string_view switch_str = switch_ ? "Enabl" : "Disabl";  // e/ed/ing

    for (const std::string& arg : options.arguments)
    {
        const size_t slash_pos = arg.find('/');
        if (slash_pos == arg.npos)
        {
            const toml::table* repositories = state.GetState()["repositories"].as_table();
            if (!repositories)
                return;

            for (const auto& [repo_name, repo_node] : *repositories)
            {
                const toml::table* repo_tbl = repo_node.as_table();
                if (!repo_tbl)
                    continue;

                if (const toml::array* plugins = repo_tbl->get_as<toml::array>("plugins"))
                {
                    for (const auto& plugin_node : *plugins)
                    {
                        const toml::table* plugin_tbl = plugin_node.as_table();
                        if (!plugin_tbl)
                            continue;
                        TomlAPI plugin_api(*plugin_tbl);
                        if (arg == plugin_api.GetValue<std::string>("id", UNKNOWN))
                        {
                            fs::path base_path = plugin_api.GetValue<std::string>("library", UNKNOWN);
                            switch_plugin_path(arg, base_path, switch_, switch_str);
                        }
                    }
                }
            }
        }
        else
        {
            const std::string& repo   = arg.substr(0, slash_pos);
            const std::string& plugin = arg.substr(slash_pos + 1);

            const toml::table* repo_tbl = state.GetState()["repositories"][repo].as_table();
            if (!repo_tbl)
                die("No such repository '{}'", repo);
            if (const auto* plugins_arr_tbl = repo_tbl->get_as<toml::array>("plugins"))
            {
                for (const auto& plugin_node : *plugins_arr_tbl)
                {
                    const toml::table* plugin_tbl = plugin_node.as_table();
                    if (!plugin_tbl || TomlAPI(*plugin_tbl).GetValue<std::string>("name", "(unknown)") != plugin)
                        continue;

                    fs::path base_path = TomlAPI(*plugin_tbl).GetValue<std::string>("library", UNKNOWN);
                    switch_plugin_path(arg, base_path, switch_, switch_str);
                }
            }
        }
    }
}

void list_all_plugins(const StateManager& state)
{
    const auto& is_plugin_disabled = [&](const std::string& manifest_name, const std::string& plugin_name) {
        const auto* repo_tbl = state.GetState()["repositories"][manifest_name].as_table();
        if (!repo_tbl)
            die("No such repository '{}'", manifest_name);
        if (const auto* plugins_arr_tbl = repo_tbl->get_as<toml::array>("plugins"))
        {
            for (const auto& plugin_node : *plugins_arr_tbl)
            {
                const toml::table* plugin_tbl = plugin_node.as_table();
                if (!plugin_tbl || TomlAPI(*plugin_tbl).GetValue<std::string>("name", "(unknown)") != plugin_name)
                    continue;

                const fs::path base_path = TomlAPI(*plugin_tbl).GetValue<std::string>("library", UNKNOWN);
                if (fs::exists(base_path.string() + ".disabled"))
                    return true;
            }
        }
        return false;
    };

    if (options.list_verbose)
    {
        for (const manifest_t& manifest : state.GetAllRepos())
        {
            fmt::println("\033[1;32mRepository:\033[0m {}", manifest.name);
            fmt::println("\033[1;33mURL:\033[0m {}", manifest.url.empty() ? "(none)" : manifest.url);
            fmt::println("\033[1;33mSource:\033[0m {}", manifest.source == RepoSource::LocalPlugin ? "local" : "git");
            fmt::println("\033[1;34mPlugins:");
            for (const plugin_t& plugin : manifest.plugins)
            {
                fmt::println("\033[1;34m - {} ({})\033[0m", plugin.name, plugin.id);
                fmt::println("\t\033[1;35mDescription:\033[0m {}", plugin.description);
                fmt::println("\t\033[1;36mAuthor(s):\033[0m {}", fmt::join(plugin.authors, ", "));
                fmt::println("\t\033[1;38;2;255;100;220mDisabled:\033[0m {}",
                             is_plugin_disabled(manifest.name, plugin.name));
                fmt::println("\t\033[1;38;2;220;220;220mLicense(s):\033[0m {}", fmt::join(plugin.licenses, ", "));
            }
            fmt::print("\033[0m");
        }
    }
    else
    {
        for (const manifest_t& manifest : state.GetAllRepos())
        {
            const char* source_tag =
                manifest.source == RepoSource::LocalPlugin ? " \033[1;38;5;244m[local]\033[0m" : "";
            fmt::println(
                "\033[1;32mRepository:\033[0m {} (\033[1;33m{}\033[0m){}", manifest.name, manifest.url, source_tag);
            fmt::println("\033[1;34mPlugins:");
            for (const plugin_t& plugin : manifest.plugins)
            {
                fmt::print("   \033[1;34m{} - \033[1;35m{}", plugin.name, plugin.description);
                if (is_plugin_disabled(manifest.name, plugin.name))
                    fmt::print(" \033[1;31m(DISABLED)");
                fmt::print("\n");
            }
            fmt::print("\033[0m");
        }
    }
}

namespace ansi
{
constexpr const char* reset  = "\033[0m";
constexpr const char* cyan   = "\033[1;36m";
constexpr const char* blue   = "\033[1;34m";
constexpr const char* green  = "\033[1;32m";
constexpr const char* yellow = "\033[1;33m";
constexpr const char* red    = "\033[1;31m";
}  // namespace ansi

static int oshotpm_main(int argc, char** argv)
{
    if (!parseargs(argc, argv))
        return -1;

    fs::create_directories({ get_home_cache_dir() / "oshotpm" });
    fs::create_directories({ get_config_dir() / "plugins" });

    // For logging methods in util.hpp to not popup
    // a dialog box
    disable_dialogs();

    PluginCallbacks cb;
    cb.on_status  = [](const std::string_view msg) { fmt::print("{}==> {}...{}\n", ansi::blue, msg, ansi::reset); };
    cb.on_success = [](const std::string_view msg) { fmt::print("{}[OK] {}{}\n", ansi::green, msg, ansi::reset); };
    cb.on_info    = [](const std::string_view msg) { fmt::print("{}[INFO] {}{}\n", ansi::cyan, msg, ansi::reset); };
    cb.on_error   = [](const std::string_view msg) { fmt::print("{}[ERR] {}{}\n", ansi::red, msg, ansi::reset); };
    cb.on_warning = [](const std::string_view msg) { fmt::print("{}[WARN] {}{}\n", ansi::yellow, msg, ansi::reset); };
    cb.confirm    = [](const std::string_view prompt, bool default_answer) {
        if (options.install_shut_up)
            return default_answer;
        return ask_user_yn(default_answer, "{}", prompt);
    };

    PluginManager       plugin_manager(StateManager{}, cb, /*is_cli=*/true);
    const StateManager& state = plugin_manager.GetStateManager();
    switch (op)
    {
        case INSTALL:
        {
            if (options.arguments.size() < 1)
                die("Please provide a plugin repository, local path, or archive to install");
            for (const std::string& arg : options.arguments)
                MUST_OK(plugin_manager.Install(arg), error("Failed to install '{}': {}", arg, _r.error_v()));
            break;
        }
        case LIST:
        {
            list_all_plugins(state);
            break;
        }
        case GEN_MANIFEST:
        {
            if (fs::exists(MANIFEST_NAME) && !ask_user_yn(false, "{} already exists. Overwrite it?", MANIFEST_NAME))
                return EXIT_FAILURE;
            auto f = fmt::output_file(MANIFEST_NAME, fmt::file::CREATE | fmt::file::WRONLY | fmt::file::TRUNC);
            f.print("{}", AUTO_MANIFEST);
            f.close();
            break;
        }
        case ENABLE:
        {
            switch_plugin(state, true);
            break;
        }
        case DISABLE:
        {
            switch_plugin(state, false);
            break;
        }
        case UPDATE:
        {
            MUST_OK(plugin_manager.UpdateRepos(), error("Failed to update repositories: {}", _r.error_v()));
            break;
        }
        case UNINSTALL:
        {
            if (options.arguments.size() < 1)
                die("Please provide a plugin repository to uninstall");

            for (const std::string& arg : options.arguments)
                MUST_OK(plugin_manager.RemoveRepo(arg), error("Failed to remove repository: {}", _r.error_v()));
            break;
        }
        default: warn("uh?");
    }

    return EXIT_SUCCESS;
}

int main(int argc, char* argv[])
{
    // e.g. std::filesystem or fmt::output_file errors: report them instead of std::terminate()
    try
    {
        return oshotpm_main(argc, argv);
    }
    catch (const std::exception& e)
    {
        error("{}", e.what());
        return EXIT_FAILURE;
    }
}
