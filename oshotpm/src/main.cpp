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
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>

#if OSHOT_WINDOWS
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

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
    SEARCH,
    INSTALL,
    UPDATE,
    LIST,
    ENABLE,
    DISABLE,
    UNINSTALL,
    GEN_MANIFEST,
    HELP
} op = NONE;

struct command_t
{
    std::string_view name;
    OPs              op;
    std::string_view help;
    const char*      short_opts;  // after the leading '+'
};

constexpr auto commands = std::to_array<command_t>({
    { .name = "search", .op = SEARCH, .help = oshotpm_help_search, .short_opts = "vh" },
    { .name = "install", .op = INSTALL, .help = oshotpm_help_install, .short_opts = "fyh" },
    { .name = "update", .op = UPDATE, .help = oshotpm_help_update, .short_opts = "yh" },
    { .name = "list", .op = LIST, .help = oshotpm_help_list, .short_opts = "vh" },
    { .name = "enable", .op = ENABLE, .help = oshotpm_help_enable, .short_opts = "h" },
    { .name = "disable", .op = DISABLE, .help = oshotpm_help_enable, .short_opts = "h" },
    { .name = "uninstall", .op = UNINSTALL, .help = oshotpm_help_uninstall, .short_opts = "h" },
    { .name = "gen-manifest", .op = GEN_MANIFEST, .help = oshotpm_help_gen_manifest, .short_opts = "h" },
    { .name = "help", .op = HELP, .help = oshotpm_help, .short_opts = "h" },
});

static const command_t* g_cmd = nullptr;  // the command being run

static const command_t* find_command(const std::string_view name)
{
    auto it = std::ranges::find(commands, name, &command_t::name);
    return it != commands.end() ? &*it : nullptr;
}

[[noreturn]] static void print_help(const std::string_view text, int exit_code)
{
    fmt::print(exit_code == EXIT_SUCCESS ? stdout : stderr, "{}", text);
    std::exit(exit_code);
}

// ANSI colors only on a terminal that understands them, and not with NO_COLOR
static bool g_color = false;

static void setup_colors()
{
    if (const char* no_color = std::getenv("NO_COLOR"); no_color && *no_color)
        return;
#if OSHOT_WINDOWS
    if (!_isatty(_fileno(stdout)))
        return;
    HANDLE out  = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    g_color     = GetConsoleMode(out, &mode) && SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    g_color = isatty(STDOUT_FILENO);
#endif
}

static const char* col(const char* code)
{
    return g_color ? code : "";
}

namespace ansi
{
constexpr const char* reset  = "\033[0m";
constexpr const char* dim    = "\033[2m";
constexpr const char* cyan   = "\033[1;36m";
constexpr const char* blue   = "\033[1;34m";
constexpr const char* green  = "\033[1;32m";
constexpr const char* yellow = "\033[1;33m";
constexpr const char* red    = "\033[1;31m";
}  // namespace ansi

// NOLINTBEGIN
static void parse_command_args(const command_t& cmd, int argc, char* argv[])
{
    // clang-format off
    const struct option long_opts[] = {
        {"force",   no_argument, nullptr, 'f'},
        {"yes",     no_argument, nullptr, 'y'},
        {"verbose", no_argument, nullptr, 'v'},
        {"help",    no_argument, nullptr, 'h'},
        {0, 0, 0, 0}
    };
    // clang-format on

    const std::string short_opts = fmt::format("+{}", cmd.short_opts);
    int               opt;
    while ((opt = getopt_long(argc, argv, short_opts.c_str(), long_opts, nullptr)) != -1)
    {
        // A long option this command doesn't take
        if (opt != '?' && std::string_view(cmd.short_opts).find(static_cast<char>(opt)) == std::string_view::npos)
        {
            fmt::print(stderr, "oshotpm {}: unknown option '{}'\n\n", cmd.name, argv[optind - 1]);
            print_help(cmd.help, EXIT_FAILURE);
        }

        switch (opt)
        {
            case 'h': print_help(cmd.help, EXIT_SUCCESS);
            case 'f': options.install_force = true; break;
            case 'y': options.install_shut_up = true; break;
            case 'v': options.list_verbose = true; break;
            default:  fmt::print(stderr, "\n"); print_help(cmd.help, EXIT_FAILURE);
        }
    }

    for (int i = optind; i < argc; ++i)
        options.arguments.emplace_back(argv[i]);
}

static void parseargs(int argc, char* argv[])
{
    // clang-format off
    static const struct option opts[] = {
        {"version", no_argument, 0, 'V'},
        {"help",    no_argument, 0, 'h'},
        {0,0,0,0}
    };
    // clang-format on

    int opt = 0;
    optind  = 1;
    while ((opt = getopt_long(argc, argv, "+Vh", opts, nullptr)) != -1)
    {
        switch (opt)
        {
            case 'V': fmt::print("{}", version_infos); std::exit(EXIT_SUCCESS);
            case 'h': print_help(oshotpm_help, EXIT_SUCCESS);
            default:  fmt::print(stderr, "\n"); print_help(oshotpm_help, EXIT_FAILURE);
        }
    }

    if (optind >= argc)
        print_help(oshotpm_help, EXIT_FAILURE);

    const std::string_view name = argv[optind];
    const command_t*       cmd  = find_command(name);
    if (!cmd)
    {
        fmt::print(stderr, "oshotpm: unknown command '{}'\n\n", name);
        print_help(oshotpm_help, EXIT_FAILURE);
    }

    op                 = cmd->op;
    g_cmd              = cmd;
    const int sub_argc = argc - optind;
    char**    sub_argv = argv + optind;
    optind             = 0;
    parse_command_args(*cmd, sub_argc, sub_argv);
}
// NOLINTEND

static void list_all_plugins(const StateManager& state)
{
    const std::vector<manifest_t>& repos = state.GetAllRepos();
    if (repos.empty())
    {
        fmt::println("No plugins installed. Install some with: oshotpm install <SOURCE>");
        return;
    }

    for (const manifest_t& repo : repos)
    {
        const bool local = repo.source == RepoSource::LocalPlugin;
        fmt::print("{}{}{}", col(ansi::green), repo.name, col(ansi::reset));
        if (!repo.url.empty())
            fmt::print(" ({}{}{})", col(ansi::yellow), repo.url, col(ansi::reset));
        if (local)
            fmt::print(" {}[local]{}", col(ansi::dim), col(ansi::reset));
        else if (options.list_verbose && !repo.git_hash.empty())
            fmt::print(" {}commit {:.7}{}", col(ansi::dim), repo.git_hash, col(ansi::reset));
        fmt::print("\n");

        for (const plugin_t& plugin : repo.plugins)
        {
            std::error_code ec;
            fmt::print("  {}{}{} {}{}{}",
                       col(ansi::blue),
                       plugin.name,
                       col(ansi::reset),
                       col(ansi::dim),
                       plugin.id,
                       col(ansi::reset));
            if (!plugin.version.empty())
                fmt::print(" {}", plugin.version);
            if (!plugin.enabled)
                fmt::print(" {}(disabled){}", col(ansi::red), col(ansi::reset));
            if (plugin.library.empty() || !fs::exists(plugin.library, ec))
                fmt::print(" {}(library missing, reinstall it){}", col(ansi::red), col(ansi::reset));
            fmt::print("\n");

            if (!plugin.description.empty())
                fmt::println("      {}", plugin.description);
            if (options.list_verbose)
            {
                fmt::println(
                    "      {}Authors:{}   {}", col(ansi::cyan), col(ansi::reset), fmt::join(plugin.authors, ", "));
                fmt::println(
                    "      {}Licenses:{}  {}", col(ansi::cyan), col(ansi::reset), fmt::join(plugin.licenses, ", "));
                if (!plugin.platforms.empty())
                    fmt::println("      {}Platforms:{} {}",
                                 col(ansi::cyan),
                                 col(ansi::reset),
                                 fmt::join(plugin.platforms, ", "));
                fmt::println("      {}Library:{}   {}", col(ansi::cyan), col(ansi::reset), plugin.library.string());
            }
        }
    }
}

static std::string lowercase(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Lists the index's plugins that match every search word
static bool search_index(const StateManager& state, const PluginCallbacks& cb)
{
    bool stale = false;
    auto index = PluginManager::FetchIndex(/*allow_cached=*/true, &stale);
    if (!index.ok())
    {
        cb.on_error(index.error_v());
        return false;
    }
    if (stale)
        cb.on_warning("Couldn't download the plugin index, this is the last downloaded copy");

    size_t shown = 0;
    for (const index_plugin_t& p : index.get().plugins)
    {
        const std::string haystack =
            lowercase(fmt::format("{} {} {} {}", p.id, p.name, p.description, fmt::join(p.tags, " ")));
        if (!std::ranges::all_of(options.arguments,
                                 [&](const std::string& w) { return haystack.find(lowercase(w)) != haystack.npos; }))
            continue;
        ++shown;

        std::string            why;
        const index_version_t* best      = pick_version(p, &why);
        const auto             installed = state.FindPlugin(p.id);

        fmt::print("{}{}{} {}{}{}", col(ansi::blue), p.name, col(ansi::reset), col(ansi::dim), p.id, col(ansi::reset));
        if (best)
            fmt::print(" {}", best->version);
        if (installed && !installed->version.empty())
        {
            const bool newer = best && compare_versions(best->version, installed->version) > 0;
            fmt::print(" {}(installed {}{}){}",
                       col(ansi::green),
                       installed->version,
                       newer ? ", update available" : "",
                       col(ansi::reset));
        }
        else if (installed)
        {
            fmt::print(" {}(installed from {}){}", col(ansi::green), installed->repository, col(ansi::reset));
        }
        if (p.network)
            fmt::print(" {}[uses the internet]{}", col(ansi::yellow), col(ansi::reset));
        fmt::print("\n");

        if (!p.description.empty())
            fmt::println("    {}", p.description);
        if (!best)
            fmt::println("    {}Can't be installed here: {}{}", col(ansi::red), why, col(ansi::reset));
        else if (best->artifacts.contains(OSHOT_TARGET))
            fmt::println("    {}Prebuilt for {}{}", col(ansi::dim), OSHOT_TARGET, col(ansi::reset));
        else
            fmt::println("    {}Built from source when installed (needs git, CMake and a compiler){}",
                         col(ansi::dim),
                         col(ansi::reset));

        if (options.list_verbose)
        {
            fmt::println("    {}Owners:{}   {}", col(ansi::cyan), col(ansi::reset), fmt::join(p.owners, ", "));
            fmt::println("    {}License:{}  {}", col(ansi::cyan), col(ansi::reset), p.license);
            fmt::println("    {}Homepage:{} {}", col(ansi::cyan), col(ansi::reset), p.homepage);
            for (const index_version_t& v : p.versions)
            {
                fmt::print("    {}{}{} {}", col(ansi::cyan), v.version, col(ansi::reset), v.date);
                if (v.yanked)
                    fmt::print(" {}withdrawn{}{}{}",
                               col(ansi::red),
                               v.yanked_reason.empty() ? "" : ": ",
                               v.yanked_reason,
                               col(ansi::reset));
                if (std::string why_not; !v.yanked && !is_installable(v, &why_not))
                    fmt::print(" {}(can't be installed: {}){}", col(ansi::red), why_not, col(ansi::reset));
                if (!v.changes.empty())
                    fmt::print(" - {}", v.changes);
                fmt::print("\n");
            }
        }
    }

    if (shown == 0)
        fmt::println("{}", options.arguments.empty() ? "The plugin index is empty" : "No plugin in the index matches");
    return true;
}

static int oshotpm_main(int argc, char** argv)
{
    setup_colors();
    parseargs(argc, argv);

    PluginCallbacks cb;
    cb.on_status = [](const std::string_view msg) {
        fmt::print("{}==>{} {}\n", col(ansi::blue), col(ansi::reset), msg);
    };
    cb.on_success = [](const std::string_view msg) {
        fmt::print("{}[OK]{} {}\n", col(ansi::green), col(ansi::reset), msg);
    };
    cb.on_info = [](const std::string_view msg) {
        fmt::print("{}[INFO]{} {}\n", col(ansi::cyan), col(ansi::reset), msg);
    };
    cb.on_error = [](const std::string_view msg) {
        std::fflush(stdout);  // after what came before it, e.g. a prompt
        fmt::print(stderr, "{}[ERR]{} {}\n", col(ansi::red), col(ansi::reset), msg);
    };
    cb.on_warning = [](const std::string_view msg) {
        fmt::print("{}[WARN]{} {}\n", col(ansi::yellow), col(ansi::reset), msg);
    };
    cb.on_output = [](const std::string_view out) {
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
    };
    cb.confirm = [](const std::string_view prompt, bool default_answer) {
        if (options.install_shut_up)
            return default_answer;
        return ask_user_yn(default_answer, "{}", prompt);
    };

    PluginManager       plugin_manager(StateManager{}, cb, /*is_cli=*/true);
    const StateManager& state = plugin_manager.GetStateManager();

    // Runs `fn` for every argument, reporting each failure, and returns whether all succeeded
    const auto for_each_arg = [&](const char* what, auto&& fn) {
        if (options.arguments.empty())
        {
            fmt::print(stderr, "oshotpm: give at least one {}\n\n", what);
            print_help(g_cmd->help, EXIT_FAILURE);
        }
        bool ok = true;
        for (const std::string& arg : options.arguments)
        {
            Result<> r = fn(arg);
            if (!r.ok())
            {
                cb.on_error(fmt::format("{}: {}", arg, r.error_v()));
                ok = false;
            }
        }
        return ok;
    };

    bool ok = true;
    switch (op)
    {
        case INSTALL:
            ok = for_each_arg("source", [&](const std::string& arg) { return plugin_manager.Install(arg); });
            if (ok)
                cb.on_info("The plugins load the next time oshot starts");
            break;

        case UPDATE:
        {
            Result<> r = plugin_manager.UpdateAll();
            if (!r.ok())
            {
                cb.on_error(r.error_v());
                ok = false;
            }
            break;
        }

        case UNINSTALL:
            ok = for_each_arg("plugin ID or repository name",
                              [&](const std::string& arg) { return plugin_manager.Uninstall(arg); });
            break;

        case ENABLE:
        case DISABLE:
            ok = for_each_arg("plugin",
                              [&](const std::string& arg) { return plugin_manager.SetEnabled(arg, op == ENABLE); });
            break;

        case LIST:   list_all_plugins(state); break;
        case SEARCH: ok = search_index(state, cb); break;

        case GEN_MANIFEST:
        {
            std::error_code ec;
            if (fs::exists(MANIFEST_NAME, ec) && !ask_user_yn(false, "{} already exists. Overwrite it?", MANIFEST_NAME))
                return EXIT_FAILURE;
            auto f = fmt::output_file(MANIFEST_NAME, fmt::file::CREATE | fmt::file::WRONLY | fmt::file::TRUNC);
            f.print("{}", AUTO_MANIFEST);
            f.close();
            cb.on_success(fmt::format("Wrote {}", MANIFEST_NAME));
            break;
        }

        case HELP:
        {
            if (options.arguments.empty())
                print_help(oshotpm_help, EXIT_SUCCESS);
            const command_t* cmd = find_command(options.arguments.front());
            if (!cmd)
            {
                fmt::print(stderr, "oshotpm help: unknown command '{}'\n\n", options.arguments.front());
                print_help(oshotpm_help, EXIT_FAILURE);
            }
            print_help(cmd->help, EXIT_SUCCESS);
        }

        default: print_help(oshotpm_help, EXIT_FAILURE);
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char* argv[])
{
    // It's a command line tool, so errors and questions go to the terminal, never to dialogs
    disable_dialogs();

    // e.g. std::filesystem or fmt::output_file errors. Report them instead of std::terminate()
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
