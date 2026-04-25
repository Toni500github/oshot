#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "fmt/format.h"

#ifndef _WIN32
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "clipboard.hpp"
#include "config.hpp"
#include "fmt/base.h"
#include "fmt/compile.h"
#include "getopt_port/getopt.h"
#include "oshot_png.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "switch_fnv1a.hpp"
#include "tray.hpp"
#include "util.hpp"

using namespace Tray;

// Avoid dragging glfw headers
struct GLFWwindow;

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and
// compatibility with old VS compilers. To link with VS2010-era libraries, VS2015+ requires linking with
// legacy_stdio_definitions.lib, which we do using this pragma. Your own project should not be affected, as you are
// likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#  pragma comment(lib, "legacy_stdio_definitions")
#endif

// clang-format off
// https://cfengine.com/blog/2021/optional-arguments-with-getopt-long/
// because "--opt-arg arg" won't work
// but "--opt-arg=arg" will
#define OPTIONAL_ARGUMENT_IS_PRESENT \
    ((optarg == NULL && optind < argc && argv[optind][0] != '-') \
     ? (bool) (optarg = argv[optind++]) \
     : (optarg != NULL))
// clang-format on

// Extern variables declariaions
std::deque<std::string> g_dropped_paths;
std::unique_ptr<Config> g_config;
bool                    g_is_systray = false;
int                     g_scr_w{}, g_scr_h{};
Clipboard               g_clipboard(SessionType::Unknown);

std::error_code ec;

// Print the version and some other infos, then exit successfully
static void version()
{
    fmt::print(FMT_COMPILE("{}"), version_infos);
    fmt::print("\n");
    std::exit(EXIT_SUCCESS);
}

// Print the args help menu, then exit with code depending if it's from invalid or -h arg
static void help(bool invalid_opt = false)
{
    fmt::print(FMT_COMPILE("{}"), oshot_help);
    fmt::print("\n");
    std::exit(invalid_opt);
}

// clang-format off
// parseargs() but only for parsing the user config path trough args
// and so we can directly construct Config
static fs::path parse_config_path(int argc, char* argv[], const fs::path& configDir)
{
    int opt = 0;
    int option_index = 0;
    opterr = 0;
    const char *optstring = "-C:";
    static const struct option opts[] = {
        {"config", required_argument, 0, 'C'},
        {0,0,0,0}
    };

    while ((opt = getopt_long(argc, argv, optstring, opts, &option_index)) != -1)
    {
        switch (opt)
        {
            // skip errors or anything else
            case 0:
            case '?':
                break;

            case 'C':
                if (!fs::exists(optarg))
                    die("config file '{}' doesn't exist", optarg);
                return optarg;
        }
    }

    return configDir / "config.toml";
}

static bool parseargs(int argc, char* argv[], const fs::path& configFile)
{
    int opt = 0;
    int option_index = 0;
    opterr = 1; // re-enable since before we disabled for "invalid option" error
    const char *optstring = "-Vhtgd:C:f:O:";
    static const struct option opts[] = {
        {"version", no_argument,       0, 'V'},
        {"help",    no_argument,       0, 'h'},
        {"tray",    no_argument,       0, 't'},
        {"gui",     no_argument,       0, 'g'},
        {"delay",   required_argument, 0, 'd'},
        {"config",  required_argument, 0, 'C'},
        {"source",  required_argument, 0, 'f'},
        {"override",required_argument, 0, 'O'},

        {"debug",      no_argument,       0, "debug"_fnv1a16},
        {"gen-config", optional_argument, 0, "gen-config"_fnv1a16},

        {0,0,0,0}
    };

    /* parse operation */
    optind = 1;
    while ((opt = getopt_long(argc, argv, optstring, opts, &option_index)) != -1)
    {
        switch (opt)
        {
            case 0:
            case 'C':
                break;
            case '?':
                help(EXIT_FAILURE); break;

            case 'V':
                version(); break;
            case 'h':
                help(); break;
            case 'f':
                g_config->Runtime.source_file = optarg; break;
            case 'O':
                g_config->OverrideOption(optarg); break;
            case 'd':
                g_config->OverrideOption("default.delay", std::atoi(optarg)); break;
            case 't':
                g_config->Runtime.only_launch_tray = true; break;
            case 'g':
                g_config->Runtime.only_launch_gui = true; break;
            case "debug"_fnv1a16:
                g_config->Runtime.debug_print = true; break;

            case "gen-config"_fnv1a16:
                if (OPTIONAL_ARGUMENT_IS_PRESENT)
                    g_config->GenerateConfig(optarg);
                else
                    g_config->GenerateConfig(configFile.string());
                exit(EXIT_SUCCESS);

            default:
                return false;
        }
    }

    return true;
}

// clang-format on
static std::mutex              mtx;
static std::condition_variable cv;
static std::atomic<bool>       quit{ false };
static bool                    do_capture = false;
static capture_result_t        pending_image;
static bool                    do_copy_image = false;

void exit_handler(int)
{
    quit.store(true);
    cv.notify_all();
#ifndef _WIN32
    if (g_sock > 0)
        shutdown(g_sock, SHUT_RDWR);
#endif
    extern_glfwTerminate();
    trayMaker.Exit();
    std::error_code ec;
    fs::remove(fs::temp_directory_path(ec) / fmt::format("oshot_{}.log", getpid()));
}
void exit_handler_nc()
{
    exit_handler(0);
}

int run_main_tool(const std::string& imgui_ini_path);

void glfw_error_callback(int i_error, const char* description)
{
    error("GLFW Error {}: {}", i_error, description);
}

void glfw_drop_callback(GLFWwindow*, int count, const char** paths)
{
    for (int i = 0; i < count; ++i)
        g_dropped_paths.emplace_back(paths[i]);
}

void capture_worker(const std::string& imgui_ini_path)
{
    while (!quit.load())
    {
        // wait for command
        std::unique_lock lk(mtx);
        cv.wait(lk, [] { return quit.load() || do_capture || do_copy_image; });
        if (quit.load())
            break;

        if (do_copy_image)
        {
            do_copy_image        = false;
            capture_result_t img = std::move(pending_image);
            lk.unlock();
            g_clipboard.CopyImage(img);
            continue;
        }

        do_capture = false;
        lk.unlock();
        run_main_tool(imgui_ini_path);
    }
}

#if defined(_WIN32) && !defined(WINDOWS_CMD)
static std::string wide_to_utf8(const wchar_t* w)
{
    if (!w)
        return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out;
    out.resize(static_cast<size_t>(needed) - 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static bool wants_cli_output(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (!a)
            continue;

        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0 || std::strcmp(a, "-V") == 0 ||
            std::strcmp(a, "--version") == 0 || std::strcmp(a, "-l") == 0 || std::strcmp(a, "--list") == 0)
            return true;
    }
    return false;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int       argc  = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::vector<std::string> argv_storage;
    std::vector<char*>       argv_ptrs;

    if (wargv && argc > 0)
    {
        argv_storage.reserve(argc);
        argv_ptrs.reserve(argc + 1);

        for (int i = 0; i < argc; ++i)
            argv_storage.push_back(wide_to_utf8(wargv[i]));

        for (auto& s : argv_storage)
            argv_ptrs.push_back(s.data());

        argv_ptrs.push_back(nullptr);

        LocalFree(wargv);
    }
    else
    {
        static char argv0[] = "oshot";
        argv_ptrs           = { argv0, nullptr };
        argc                = 1;
    }

    char** argv = argv_ptrs.data();

    // If we need to show CLI output, try to attach to parent console first.
    // This works when started from cmd.exe / powershell.
    if (wants_cli_output(argc, argv))
    {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
        {
            // MSYS2/mintty often isn't a real Windows console -> attach fails.
            // Allocate a console so stdout/stderr have somewhere to go.
            AllocConsole();
        }

        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        freopen_s(&dummy, "CONIN$", "r", stdin);

        // Line-buffer so help/version prints immediately
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IOLBF, 0);
    }

    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>("oshot.log", true);
#else
int main(int argc, char* argv[])
{
    const fs::path& log_path = fs::temp_directory_path(ec) / fmt::format("oshot_{}.log", getpid());
    fs::create_directories(log_path.parent_path(), ec);
    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string(), true);
#endif

#ifdef __linux__
    // AppRun prepends bundled libs to LD_LIBRARY_PATH so the AppImage is self-contained.
    // Child processes (zenity, grim, etc.) inherit it and resolve against the bundled
    // (older) libs instead of the host's, causing symbol version mismatches.
    // AppRun saves the original value here, restore it so that spawned system binaries
    // use the host libs.
    const char* orig = std::getenv("APPIMAGE_ORIG_LD_LIBRARY_PATH");
    if (orig)
        setenv("LD_LIBRARY_PATH", orig, 1);
    else
        unsetenv("LD_LIBRARY_PATH");  // not running from AppImage, clear any stale value
#endif

    atexit(exit_handler_nc);
    signal(SIGINT, exit_handler);
    signal(SIGTERM, exit_handler);
    signal(SIGABRT, exit_handler);

#ifndef _WIN32
    // Restore display then re-raise so the OS
    // still generates a core dump.
    signal(SIGSEGV, [](int sig) {
        extern_glfwTerminate();  // restore display mode
        signal(sig, SIG_DFL);    // reset to default
        raise(sig);              // re-raise for core dump
    });
#endif

    auto           console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::logger logger("oshot_logger", { console, file });
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(logger));

    // [2026-03-10 17:24:07.593] [DEBUG] XRandR capturing: monitor 1920x1080+0+0 (cursor at 1130,682)
    logger.set_pattern("[%Y-%m-%d %T.%e] [%l] %^%v%$");
    logger.flush_on(spdlog::level::trace);

    logger.info("=== oshot starting ===");
    logger.info("Log file path: {}", file->filename());
    logger.flush();
    spdlog::flush_every(std::chrono::seconds(1));

    g_clipboard.SetSession(get_session_type());

    // Check if demo build.
    // removing it once the hackaton has ended
    std::string configDir = get_config_dir().string();
    if (fs::exists("models", ec))
        configDir = ".";

    const std::string& configFile     = parse_config_path(argc, argv, configDir).string();
    const std::string& imgui_ini_path = configDir + "/imgui.ini";

    g_config    = std::make_unique<Config>(configFile, configDir);
    if (!parseargs(argc, argv, configFile))
        return EXIT_FAILURE;

    g_config->LoadConfigFile(configFile);
    if (!g_config->File.theme_file_path.empty())
        g_config->LoadThemeFile(g_config->File.theme_file_path);

    spdlog::set_level(g_config->Runtime.debug_print ? spdlog::level::debug : spdlog::level::info);

    const bool tray_lock_acquired = acquire_tray_lock();

    if (g_config->Runtime.only_launch_gui)
        return run_main_tool(imgui_ini_path);

    if (g_config->Runtime.only_launch_tray)
    {
        if (!tray_lock_acquired)
            return EXIT_FAILURE;

        g_is_systray = true;
    }
    else
    {
        if (!tray_lock_acquired)
            return run_main_tool(imgui_ini_path);

#if OSHOT_TOOL_ON_MAIN_THREAD
        run_main_tool(imgui_ini_path);
#else
        std::thread([&] { run_main_tool(imgui_ini_path); }).detach();
#endif
    }

#if !OSHOT_TOOL_ON_MAIN_THREAD
    // On macOS the tray loop polls do_capture on the main thread (required by
    // AppKit), so capture_worker must not run, because it would call run_main_tool
    // from a background thread and crash with NSInternalInconsistencyException.
    std::thread worker(capture_worker, imgui_ini_path);
#endif

    std::vector<TrayMenu*> menu;

#ifdef _WIN32
    TrayIcon tray = { "oshot.png", "oshot.ico", "oshot", menu };
#else
    // Basically create the icon.png in a temp directory and use
    // that for the systray icon. idfc, it works
    const fs::path& png_path = fs::temp_directory_path(ec) / "oshot.png";
    std::ofstream   out(png_path.string(), std::ios::binary | std::ios::out | std::ios::trunc);

    out.write(reinterpret_cast<const char*>(oshot_png), static_cast<std::streamsize>(oshot_png_len));
    out.close();
    TrayIcon tray = { png_path.string(), "oshot.ico", "oshot", menu };
#endif

    tray.menu.push_back(new TrayMenu{ "Capture",
                                      true,
                                      false,
                                      false,
                                      [&](TrayMenu*) {
#if OSHOT_TOOL_ON_MAIN_THREAD
                                          run_main_tool(imgui_ini_path);
#else
                                          std::lock_guard lk(mtx);
                                          // only queue if not already queued
                                          if (!do_capture)
                                          {
                                              do_capture = true;
                                              cv.notify_all();
                                          }
#endif
                                      },
                                      {} });

    tray.menu.push_back(new TrayMenu{ "Quit", true, false, false, [&](TrayMenu*) { exit_handler(0); }, {} });

    if (trayMaker.Initialize(&tray))
    {
        while (trayMaker.Loop(1))
        {
        }
    }
    else
    {
        die("Systray initialization failed");
    }

    // Quitted the tray
#if !OSHOT_TOOL_ON_MAIN_THREAD
    worker.join();
#endif

    return EXIT_SUCCESS;
}
