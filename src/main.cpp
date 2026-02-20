#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#ifndef _WIN32
#  include <netinet/in.h>
#endif

#include "fmt/base.h"
#include "fmt/compile.h"
#include "getopt_port/getopt.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "switch_fnv1a.hpp"
#include "trayapp/tray.hpp"
#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#  include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h>  // Will drag system OpenGL headers

#include "clipboard.hpp"
#include "config.hpp"
#include "langs.hpp"
#include "oshot_png.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "socket.hpp"
#include "switch_fnv1a.hpp"
#include "util.hpp"

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and
// compatibility with old VS compilers. To link with VS2010-era libraries, VS2015+ requires linking with
// legacy_stdio_definitions.lib, which we do using this pragma. Your own project should not be affected, as you are
// likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#  pragma comment(lib, "legacy_stdio_definitions")
#endif

#if (!__has_include("version.h"))
#  error "version.h not found, please generate it with ./scripts/generateVersion.sh"
#else
#  include "version.h"
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
std::deque<std::string>    g_dropped_paths;
std::unique_ptr<Config>    g_config;
std::unique_ptr<Clipboard> g_clipboard;
bool                       g_is_clipboard_server = false;
int                        g_scr_w{}, g_scr_h{};
FILE*                      g_fp_log;

// Print the version and some other infos, then exit successfully
static void version()
{
    fmt::print(
        "oshot {} built from branch '{}' at {} commit '{}' ({}).\n"
        "Date: {}\n"
        "Tag: {}\n",
        VERSION,
        GIT_BRANCH,
        GIT_DIRTY,
        GIT_COMMIT_HASH,
        GIT_COMMIT_MESSAGE,
        GIT_COMMIT_DATE,
        GIT_TAG);

    // if only everyone would not return error when querying the program version :(
    std::exit(EXIT_SUCCESS);
}

// Print the args help menu, then exit with code depending if it's from invalid or -h arg
static void help(bool invalid_opt = false)
{
    fmt::print(FMT_COMPILE("{}"), oshot_help);
    fmt::print("\n");
    std::exit(invalid_opt);
}

static constexpr void print_languages()
{
    for (const auto& [code, name] : GOOGLE_TRANSLATE_LANGUAGES_ARRAY)
        fmt::print(FMT_COMPILE("{}: {}\n"), name, code);
    std::exit(EXIT_SUCCESS);
}

#ifndef _WIN32
static bool recv_all(int fd, void* dst, size_t n)
{
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n)
    {
        const ssize_t r = ::recv(fd, p, n, 0);
        if (r <= 0)
            return false;
        p += static_cast<size_t>(r);
        n -= static_cast<size_t>(r);
    }
    return true;
}
#endif

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
                    die(_("config file '{}' doesn't exist"), optarg);
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
    const char *optstring = "-Vhlgd:C:f:";
    static const struct option opts[] = {
        {"version", no_argument,       0, 'V'},
        {"help",    no_argument,       0, 'h'},
        {"list",    no_argument,       0, 'l'},
        {"gui",     no_argument,       0, 'g'},
        {"delay",   required_argument, 0, 'd'},
        {"config",  required_argument, 0, 'C'},
        {"source",  required_argument, 0, 'f'},

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
            case 'l':
                print_languages(); break;
            case 'f':
                g_config->Runtime.source_file = optarg; break;
            case 'd':
                g_config->OverrideOption("default.delay", std::atoi(optarg)); break;
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

static void glfw_error_callback(int i_error, const char* description)
{
    error("GLFW Error {}: {}", i_error, description);
}

static void glfw_drop_callback(GLFWwindow*, int count, const char** paths)
{
    for (int i = 0; i < count; ++i)
        g_dropped_paths.emplace_back(paths[i]);
}

int main_tool(const std::string imgui_ini_path);

void capture_worker(const std::string& imgui_ini_path)
{
    while (!quit.load())
    {
        // wait for command
        std::unique_lock lk(mtx);
        cv.wait(lk, [] { return quit.load() || do_capture; });
        if (quit.load())
            break;

        do_capture = false;
        lk.unlock();

        main_tool(imgui_ini_path);
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

    g_fp_log = std::fopen("oshot.log", "w");
    if (!g_fp_log)
        g_fp_log = stdout;
#else
int main(int argc, char* argv[])
{
    g_fp_log = stdout;

#endif

    const std::string& configDir      = get_config_dir().string();
    const std::string& configFile     = parse_config_path(argc, argv, configDir).string();
    const std::string& imgui_ini_path = configDir + "/imgui.ini";

    g_clipboard = std::make_unique<Clipboard>(get_session_type());
    g_config    = std::make_unique<Config>(configFile, configDir);
    if (!parseargs(argc, argv, configFile))
        return EXIT_FAILURE;

    g_config->LoadConfigFile(configFile);

    if (g_config->Runtime.only_launch_gui || !acquire_tray_lock())
        return main_tool(imgui_ini_path);

    g_is_clipboard_server = true;

    auto _ = std::async(std::launch::async, [&] {
        main_tool(imgui_ini_path);
        quit.store(true);
#ifndef _WIN32
        if (g_lock_sock >= 0)
        {
            ::shutdown(g_lock_sock, SHUT_RDWR);
            ::close(g_lock_sock);
            g_lock_sock = -1;
        }
#endif
        cv.notify_all();
    });

    std::thread worker(capture_worker, imgui_ini_path);

#ifndef _WIN32
    std::thread ipc([&] {
        while (!quit.load())
        {
            const int client = ::accept(g_lock_sock, nullptr, nullptr);
            if (client < 0)
            {
                if (quit.load())
                    break;
                continue;
            }

            char     type    = 0;
            uint32_t net_len = 0;

            bool ok = recv_all(client, &type, 1) && recv_all(client, &net_len, sizeof(net_len));

            const uint32_t       len = ntohl(net_len);
            std::vector<uint8_t> payload;
            payload.resize(len);

            if (ok && len > 0)
                ok = recv_all(client, payload.data(), payload.size());

            ::close(client);

            if (!ok)
                continue;

            if (type == 'T')
            {
                std::string text(payload.begin(), payload.end());
                g_clipboard->CopyText(text);
            }
            else if (type == 'I')
            {
                if (payload.size() < 8)
                    continue;

                uint32_t w_be = 0, h_be = 0;
                std::memcpy(&w_be, payload.data() + 0, 4);
                std::memcpy(&h_be, payload.data() + 4, 4);

                const int w = static_cast<int>(ntohl(w_be));
                const int h = static_cast<int>(ntohl(h_be));
                if (w <= 0 || h <= 0)
                    continue;

                const size_t expected = static_cast<size_t>(w) * h * 4;
                if (payload.size() != expected + 8)
                    continue;

                capture_result_t cap{ std::move(payload), w, h };
                g_clipboard->CopyImage(cap);
            }
        }
    });
#endif

#ifdef _WIN32
    Tray::Tray tray("oshot", "oshot.ico");
#else
    std::error_code ec;
    const auto&     path = fs::temp_directory_path() / "oshot.png";
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path.string(), std::ios::binary | std::ios::out | std::ios::trunc);

    out.write(reinterpret_cast<const char*>(oshot_png), static_cast<std::streamsize>(oshot_png_len));
    out.close();
    Tray::Tray tray("oshot", path.string());
#endif

    tray.addEntry(Tray::Button("Capture", [&] {
        std::lock_guard lk(mtx);
        // only queue if not already queued
        if (!do_capture)
        {
            do_capture = true;
            cv.notify_all();
        }
    }));

    tray.addEntry(Tray::Button("Quit", [&] {
        quit.store(true);
#ifndef _WIN32
        if (g_lock_sock >= 0)
        {
            ::shutdown(g_lock_sock, SHUT_RDWR);
            ::close(g_lock_sock);
            g_lock_sock = -1;
        }
#endif
        cv.notify_all();
        tray.exit();
    }));

    tray.run();

    // Quitted the tray
    worker.join();
#ifndef _WIN32
    if (ipc.joinable())
        ipc.join();
#endif

    return EXIT_SUCCESS;
}

int main_tool(const std::string imgui_ini_path)
{
    GLFWwindow* window = nullptr;

    // Setup Screenshot Tool
    // Calling it before starting the window so that
    // we can capture at the exact moment we launch
    ScreenshotTool ss_tool;
    ss_tool.SetOnCancel([&]() {
        fmt::println(stderr, "Cancelled screenshot");
        glfwSwapInterval(0);  // Disable vsync
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });
    ss_tool.SetOnComplete([&](SavingOp op, const Result<capture_result_t>& result) {
        if (!result.ok())
        {
            error("Screenshot failed: {}", result.error());
        }
        else
        {
            const Result<>& res = save_png(op, result.get());
            if (!res.ok())
                error("Failed to save as PNG: {}", result.error());
        }
        glfwSwapInterval(0);  // Disable vsync
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });

    {
        const Result<>& res = ss_tool.Start();
        if (!res.ok())
        {
            error("Failed to start capture: {}", res.error());
            return EXIT_FAILURE;
        }
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return EXIT_FAILURE;

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100 (WebGL 1.0)
    const char* glsl_version = "#version 100";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
    // GL ES 3.0 + GLSL 300 es (WebGL 2.0)
    const char* glsl_version = "#version 300 es";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 330 core";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
#endif

#if !DEBUG
    // Don't make the window actually fullscreen if debug build
    // this because on windows it hanged in gdb and everytime had to restart the VM
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // Borderless
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);    // Always on top
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
#endif

    GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode    = glfwGetVideoMode(monitor);

    window = glfwCreateWindow(mode->width, mode->height, "oshot", monitor, nullptr);
    if (!window)
        return EXIT_FAILURE;
    glfwMakeContextCurrent(window);
    glfwSetDropCallback(window, glfw_drop_callback);
    glfwSwapInterval(1);  // Enable vsync

    g_scr_w = mode->width;
    g_scr_h = mode->height;

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = imgui_ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (!g_config->File.font.empty())
    {
        const auto& path = get_font_path(g_config->File.font);
        if (!path.empty())
            io.FontDefault =
                io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f, nullptr, io.Fonts->GetGlyphRangesDefault());
    }

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    {
        const Result<>& res = ss_tool.StartWindow();
        if (!res.ok())
        {
            error("Failed to start tool window: {}", res.error());
            return EXIT_FAILURE;
        }
    }

    while (!glfwWindowShouldClose(window) && ss_tool.IsActive())
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your
        // inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or
        // clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or
        // clear/overwrite your copy of the keyboard data. Generally you may always pass all inputs to dear imgui, and
        // hide them from your application based on those two flags.
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ss_tool.RenderOverlay();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // Transparent/dark background
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    g_sender->Close();

    if (g_fp_log && g_fp_log != stdout)
        std::fclose(g_fp_log);

    return EXIT_SUCCESS;
}
