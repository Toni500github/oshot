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

#include "util/paths.hpp"

#include <sys/stat.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "dotenv.hpp"
#include "fmt/format.h"
#include "platform.hpp"
#include "util/dialogs.hpp"
#include "util/strings.hpp"

// clang-format off
#if OSHOT_WINDOWS
#  ifdef __MINGW64__
#    undef NTDDI_VERSION
#    undef _WIN32_WINNT
#    define NTDDI_VERSION NTDDI_WINBLUE
#    define _WIN32_WINNT  _WIN32_WINNT_WINBLUE
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <combaseapi.h>
#  include <io.h>
#  include <knownfolders.h>
#  include <shellapi.h>
#  include <shellscalingapi.h>  // GetDpiForMonitor
#  include <shlobj.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <optional>
#  pragma comment(lib, "Shcore.lib")
#  pragma comment(lib, "ws2_32.lib")
#else
#  if OSHOT_MACOS
// AssertMacros.h (pulled in by CoreServices) defines check()/verify()/require() otherwise
#    define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#    include <CoreFoundation/CoreFoundation.h>
#    include <CoreServices/CoreServices.h>
#    include <CoreGraphics/CoreGraphics.h>
#    include <mach-o/dyld.h>
#  elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#    include <sys/types.h>
#    include <sys/sysctl.h>
#    include <climits>
#  endif
#  include <pwd.h>
#  include <unistd.h>
#endif
// clang-format on

static const std::unordered_map<std::string, std::string>& get_xdg_user_dirs()
{
    static std::unordered_map<std::string, std::string> cache;
    if (!cache.empty())
        return cache;

#if !OSHOT_WINDOWS
    fs::path file = get_home_config_dir() / "user-dirs.dirs";

    if (!fs::exists(file))
        return cache;

    Dotenv env;
    if (!env.load(file.string()))
        return cache;

    cache = env.data();
#endif

    return cache;
}

#if !OSHOT_WINDOWS
fs::path get_runtime_dir()
{
    const char* xdg = ::getenv("XDG_RUNTIME_DIR");
    return xdg ? fs::path(xdg) : fs::temp_directory_path();
}
#endif

std::string get_relative_path(const std::string_view relative_path, const char* env, const long long mode)
{
    const char* c_env = std::getenv(env);
    if (!c_env)
        return UNKNOWN;

    struct stat sb;
    std::string fullPath;
    fullPath.reserve(1024);

    for (const std::string& dir : split(c_env, ':'))
    {
        fullPath += dir;
        fullPath += '/';
        fullPath += relative_path;

        if (stat(fullPath.c_str(), &sb) == 0 && (sb.st_mode & mode) != 0)
            return fullPath;

        fullPath.clear();
    }

    return UNKNOWN;
}

std::string which(const std::string_view command)
{
    return get_relative_path(command, "PATH", S_IXUSR);
}

#if OSHOT_WINDOWS
static std::optional<fs::path> get_known_dir(REFKNOWNFOLDERID rfid, const char* backup_env)
{
    PWSTR widePath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &widePath)))
    {
        // Get required buffer size
        int         size = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
        std::string narrowPath(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, widePath, -1, &narrowPath[0], size, nullptr, nullptr);
        CoTaskMemFree(widePath);

        narrowPath.pop_back();  // the NUL
        return narrowPath;
    }

    const char* dir = std::getenv(backup_env);
    if (dir != nullptr && dir[0] != '\0' && fs::exists(dir))
        return fs::path(dir);

    return std::nullopt;
}

fs::path get_exe_path()
{
    std::wstring buf(MAX_PATH, L'\0');
    DWORD        len = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    while (len == buf.size())
    {
        buf.resize(buf.size() * 2);
        len = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    }
    if (len == 0)
        return {};
    buf.resize(len);
    return fs::path(buf);
}

fs::path get_home_dir()
{
    if (const char* h = std::getenv("USERPROFILE"))
        return h;

    const char* d = std::getenv("HOMEDRIVE");
    const char* p = std::getenv("HOMEPATH");
    if (d && p)
        return std::string(d) + p;

    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf)))
        return buf;

    dialog::die("Cannot determine home directory");
}

fs::path get_home_config_dir()
{
    auto p(get_known_dir(FOLDERID_RoamingAppData, "APPDATA"));
    if (p)
        return *p;
    dialog::die("Failed to get %APPDATA% path");
}

fs::path get_home_cache_dir()
{
    auto p(get_known_dir(FOLDERID_LocalAppData, "LOCALAPPDATA"));
    if (p)
        return *p;
    dialog::die("Failed to get %LOCALAPPDATA% path");
}

fs::path get_home_pictures_dir()
{
    auto p(get_known_dir(FOLDERID_Pictures, "_1"));
    if (p)
        return *p;

    const char* dir = std::getenv("USERPROFILE");
    if (dir != nullptr && dir[0] != '\0')
    {
        fs::path pictures = fs::path(dir) / "Pictures";
        if (fs::exists(pictures))
            return pictures;
    }

    dialog::die("Failed to get Pictures directory path");
}
#else
// `is_user_dir`: a user dir like XDG_PICTURES_DIR, also looked up in user-dirs.dirs, which
// must exist. Otherwise a base dir like XDG_CONFIG_HOME, which oshot creates on first use
static fs::path get_known_dir(const char* xdg, const char* backup, bool is_user_dir)
{
    const char* dir = std::getenv(xdg);
    if (dir != nullptr && fs::path(dir).is_absolute() && (!is_user_dir || fs::exists(dir)))
        return dir;

    if (is_user_dir)
    {
        const auto& dirs = get_xdg_user_dirs();
        if (dirs.find(xdg) != dirs.end())
            return dirs.at(xdg);
    }

    return get_home_dir() / backup;
}

fs::path get_exe_path()
{
    std::error_code ec;

#  if OSHOT_MACOS
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    return fs::canonical(buf.c_str(), ec);

#  elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#    if defined(__NetBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME };
#    else
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
#    endif
    char   buf[PATH_MAX];
    size_t len = sizeof(buf);
    if (sysctl(mib, 4, buf, &len, nullptr, 0) != 0)
        return {};
    return fs::canonical(buf, ec);

#  else
    // Linux. OpenBSD has no way to get it, so this fails there and callers
    // fall back to what they'd do without the path
    return fs::canonical("/proc/self/exe", ec);
#  endif
}

fs::path get_home_dir()
{
    if (const char* h = std::getenv("HOME"))
        return h;

    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return pw->pw_dir;

    dialog::die("Cannot determine home directory");
}

fs::path get_home_config_dir()
{
    return get_known_dir("XDG_CONFIG_HOME", ".config", false);
}

fs::path get_home_cache_dir()
{
    return get_known_dir("XDG_CACHE_HOME", ".cache", false);
}

fs::path get_home_pictures_dir()
{
    return get_known_dir("XDG_PICTURES_DIR", "Pictures", true);
}
#endif

std::string expand_var(std::string ret)
{
    if (ret.empty())
        return ret;

    if (ret.front() == '~' && (ret.size() == 1 || ret[1] == '/' || ret[1] == DIR_SEP))
        ret.replace(0, 1, get_home_dir().string());

    auto is_name_char = [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    std::string out;
    out.reserve(ret.size());
    Parser p(ret);
    while (!p.IsEof())
    {
        const char c = p.ReadChar();
        if (c != '$' || p.IsEof())
        {
            out += c;
            continue;
        }

        // ${NAME} or $NAME, where NAME doesn't start with a digit
        const size_t dollar = p.pos - 1;
        std::string  name;
        if (p.TryRead('{'))
        {
            while (!p.IsEof() && p.Peek() != '}')
                name += p.ReadChar();
            if (!p.TryRead('}'))
            {
                // No closing brace, so the '$' is literal and the rest is read again
                out += c;
                p.Rewind(p.pos - dollar - 1);
                continue;
            }
        }
        else
        {
            while (is_name_char(p.Peek()))
                name += p.ReadChar();
        }

        const char* value =
            (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) ? nullptr : std::getenv(name.c_str());

        // Unset variables stay as they are, so a literal '$' survives
        if (value)
            out += value;
        else
            out.append(ret, dollar, p.pos - dollar);
    }

    return out;
}

fs::path get_config_dir()
{
    return get_home_config_dir() / "oshot";
}

fs::path get_cache_dir()
{
    return get_home_cache_dir() / "oshot";
}

fs::path get_exe_dir()
{
    return get_exe_path().parent_path();
}

bool remove_tree(const fs::path& path) noexcept
{
    std::error_code ec;
#if OSHOT_WINDOWS
    // git marks its object files read-only, and Windows won't delete those
    for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
    ec.clear();
#endif
    fs::remove_all(path, ec);
    if (ec)
        spdlog::warn("Failed to remove '{}': {}", path.string(), ec.message());
    return !ec;
}

fs::path get_bundled_models_dir()
{
    const fs::path& exe_dir = get_exe_dir();
    if (exe_dir.empty())
        return {};

    std::error_code ec;
#if OSHOT_MACOS
    // oshot.app/Contents/MacOS/oshot -> oshot.app/Contents/Resources/tessdata
    const fs::path resources = exe_dir.parent_path() / "Resources" / "tessdata";
    if (fs::is_directory(resources, ec))
        return resources;
#endif

    const fs::path& models = exe_dir / "models";
    if (fs::is_directory(models, ec))
        return models;
    return {};
}

std::string get_default_ocr_path()
{
#if OSHOT_FREEDESKTOP
    static constexpr std::array candidates = {
#  ifdef OSHOT_TESSERACT_DATADIR
        OSHOT_TESSERACT_DATADIR "/tessdata",  // tesseract.pc's datadir, the prefix tesseract was built for
#  endif
        "/usr/share/tessdata",                     // Arch, openSUSE, Alpine, Gentoo, Void
        "/usr/share/tesseract-ocr/5/tessdata",     // Debian 12+, Ubuntu 23.04+
        "/usr/share/tesseract-ocr/4.00/tessdata",  // Debian 11, Ubuntu 22.04
        "/usr/share/tesseract/tessdata",           // Fedora, RHEL
        "/usr/local/share/tessdata",               // FreeBSD, OpenBSD, source installs
        "/usr/pkg/share/tessdata",                 // NetBSD (pkgsrc)
    };

    std::error_code ec;
    for (const char* dir : candidates)
    {
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
            if (entry.path().extension() == ".traineddata")
                return std::string(dir) + '/';
    }
    // No models installed yet, so keep the path most distros use
    return "/usr/share/tessdata/";
#else
    // Windows and macOS ship eng.traineddata, which Config seeds into the config dir
    return "./models";
#endif
}
