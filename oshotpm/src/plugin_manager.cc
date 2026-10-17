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

#include "plugin_manager.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <random>

#include "dylib.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "plugin.hpp"
#include "tiny-process-library/process.hpp"

namespace
{
// Deletes a work dir on every early return, unless dismissed
struct WorkDirGuard
{
    fs::path path;
    bool     dismissed = false;

    WorkDirGuard(fs::path p) : path(std::move(p)) {}
    ~WorkDirGuard()
    {
        if (!dismissed)
            remove_tree(path);  // noexcept: a failure only gets logged
    }
    WorkDirGuard(const WorkDirGuard&)            = delete;
    WorkDirGuard& operator=(const WorkDirGuard&) = delete;

    void Dismiss() { dismissed = true; }
};

struct proc_result_t
{
    int         status = -1;
    std::string output;  // stdout and stderr, interleaved
};

template <typename Cmd>
proc_result_t run_process(const Cmd& cmd, const fs::path& cwd, const std::function<void(std::string_view)>& on_output)
{
    proc_result_t r;
    std::mutex    mtx;
    // stdout and stderr are read on two threads
    auto collect = [&](const char* buf, size_t n) {
        std::scoped_lock lock(mtx);
        r.output.append(buf, n);
        if (r.output.size() > 256 * 1024)
            r.output.erase(0, r.output.size() - 128 * 1024);
        if (on_output)
            on_output(std::string_view(buf, n));
    };

    TinyProcessLib::Process proc(cmd, cwd.string(), collect, collect);
    r.status = proc.get_exit_status();
    return r;
}

// The last `n` lines of a command's output, for error messages
std::string tail(std::string_view out, size_t n = 30)
{
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.remove_suffix(1);

    size_t pos = out.size();
    for (size_t i = 0; i < n && pos != out.npos && pos > 0; ++i)
        pos = out.rfind('\n', pos - 1);
    return std::string(pos == out.npos ? out : out.substr(pos + 1));
}

std::string trim(std::string s)
{
    const auto is_space = [](unsigned char c) { return std::isspace(c); };
    s.erase(s.begin(), std::ranges::find_if_not(s, is_space));
    while (!s.empty() && is_space(s.back()))
        s.pop_back();
    return s;
}

bool looks_like_git_url(const std::string& source)
{
    return source.find("://") != source.npos || source.starts_with("git@") || source.ends_with(".git");
}

bool is_archive(const fs::path& path)
{
    std::string name = path.filename().string();
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const std::string_view ext : { ".zip", ".tgz", ".tar.gz", ".txz", ".tar.xz" })
        if (name.ends_with(ext))
            return true;
    return false;
}

// Whether `path` is `base` or inside it, after resolving "..", symlinks and the like
bool is_inside(const fs::path& base, const fs::path& path)
{
    std::error_code ec;
    const fs::path  rel = fs::weakly_canonical(path, ec).lexically_relative(fs::weakly_canonical(base, ec));
    return !ec && !rel.empty() && *rel.begin() != "..";
}

// File extensions of plugin libraries. On macOS, CMake names MODULE libraries
// .so, and dlopen() loads those as well as .dylib
bool is_library_file(const fs::path& path)
{
    const fs::path ext = path.extension();
#if OSHOT_MACOS
    return ext == ".dylib" || ext == ".so";
#else
    return ext == dylib::decorations::os_default().suffix;
#endif
}

std::string library_suffix()
{
#if OSHOT_MACOS
    return ".dylib or .so";
#else
    return dylib::decorations::os_default().suffix;
#endif
}

// The one library in `dir`
Result<fs::path> find_library_in(const fs::path& dir)
{
    std::error_code       ec;
    std::vector<fs::path> libs;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file(ec) && is_library_file(entry.path()))
            libs.push_back(entry.path());

    if (ec)
        return Err("Can't read '{}': {}", dir.string(), ec.message());
    if (libs.empty())
        return Err("No {} library in '{}'", library_suffix(), dir.string());
    if (libs.size() > 1)
    {
        std::vector<std::string> names;
        for (const fs::path& lib : libs)
            names.push_back(lib.filename().string());
        return Err("'{}' has more than one library ({}), it must only have the plugin's",
                   dir.string(),
                   fmt::join(names, ", "));
    }
    return Ok(libs.front());
}

Result<> extract_archive(const fs::path& archive, const fs::path& dest_dir)
{
#if OSHOT_WINDOWS || OSHOT_MACOS
    // bsdtar, which also reads zip files
    std::vector<std::string> cmd{ "tar", "-xf", archive.string(), "-C", dest_dir.string() };
#else
    std::vector<std::string> cmd;
    if (archive.extension() == ".zip")
        cmd = { "unzip", "-q", archive.string(), "-d", dest_dir.string() };
    else
        cmd = { "tar", "-xf", archive.string(), "-C", dest_dir.string() };
#endif

    const proc_result_t r = run_process(cmd, dest_dir, nullptr);
    if (r.status != 0)
        return Err("Failed to extract '{}' with {} (exit code {}): {}",
                   archive.filename().string(),
                   cmd.front(),
                   r.status,
                   tail(r.output));
    return Ok();
}

// Sets the variables build steps can use, once
void export_build_env()
{
    static std::once_flag once;
    std::call_once(once, [] {
        auto set = [](const char* name, const std::string& value) {
#if OSHOT_WINDOWS
            _putenv_s(name, value.c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        };
        set("OSHOT_PLATFORM", OSHOT_PLATFORM);
        set("OSHOT_ARCH", OSHOT_ARCH);
        set("OSHOT_TARGET", OSHOT_TARGET);
        set("OSHOT_VERSION", VERSION);
        const fs::path sdk = PluginManager::FindSdk();
        if (!sdk.empty())
            set("OSHOT_SDK", sdk.string());
    });
}
}  // namespace

fs::path PluginManager::FindSdk()
{
    const auto valid = [](const fs::path& dir) {
        std::error_code ec;
        return !dir.empty() && fs::exists(dir / "include" / "oshot" / "oshot_plugin.h", ec);
    };

    if (const char* env = std::getenv("OSHOT_SDK"); env && valid(env))
        return env;

    const fs::path exe_dir = get_exe_dir();
    if (exe_dir.empty())
        return {};

    // Windows zip/installer and the build tree; macOS .app; Linux prefix (zip, .deb, AppImage's usr/)
    for (const fs::path& dir : { exe_dir / "sdk", exe_dir / ".." / "Resources" / "sdk", exe_dir / ".." })
        if (valid(dir))
            return fs::weakly_canonical(dir);
    return {};
}

// ----- git -----
Result<> PluginManager::CheckGit()
{
    if (run_process(std::vector<std::string>{ "git", "--version" }, m_cache_path, nullptr).status != 0)
        return Err(
            "git isn't installed or isn't in PATH. Install it (https://git-scm.com) to install plugins from "
            "a git URL, or install a prebuilt plugin archive instead");
    return Ok();
}

Result<std::string> PluginManager::Git(const std::vector<std::string>& args, const fs::path& cwd)
{
    std::vector<std::string> cmd{ "git" };
    cmd.insert(cmd.end(), args.begin(), args.end());

    const proc_result_t r = run_process(cmd, cwd, nullptr);
    if (r.status != 0)
        return Err("'git {}' failed (exit code {}): {}", fmt::join(args, " "), r.status, tail(r.output));
    return Ok(trim(r.output));
}

// ----- work dirs -----
fs::path PluginManager::NewWorkDir(std::string_view prefix)
{
    static std::mt19937                    gen(std::random_device{}());
    static std::uniform_int_distribution<> dist(0, 999999);

    fs::path        dir = m_cache_path / fmt::format("tmp-{}-{}", prefix, dist(gen));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// Work dirs of an oshotpm that got killed or interrupted
void PluginManager::RemoveStaleWorkDirs()
{
    std::error_code ec;
    const auto      now = fs::file_time_type::clock::now();
    for (const fs::directory_entry& entry : fs::directory_iterator(m_cache_path, ec))
    {
        if (!entry.path().filename().string().starts_with("tmp-"))
            continue;
        const auto mtime = entry.last_write_time(ec);
        if (!ec && now - mtime > std::chrono::hours(1))
            remove_tree(entry.path());
    }
}

void PluginManager::RemovePluginDir(const std::string& id)
{
    if (!Manifest::IsValidID(id))
        return;

    const fs::path  dir = m_plugins_path / id;
    std::error_code ec;
    if (fs::exists(dir, ec) && !remove_tree(dir))
        m_callbacks.on_warning(fmt::format(
            "Couldn't delete everything in '{}' (is oshot running?). oshot removes it the next time it starts",
            dir.string()));
}

// ----- confirmations -----
Result<> PluginManager::ConfirmTrust()
{
    if (options.install_shut_up)
        return Ok();

    static constexpr std::string_view disclaimer =
        "Plugins are native code: they run with the same permissions as oshot, and can read and change your "
        "files. Only install plugins from people you trust, and check the repository first if you can.";

    // The GUI shows it in the confirmation itself
    if (m_is_cli)
        m_callbacks.on_warning(disclaimer);
    const std::string prompt =
        m_is_cli ? "Continue installing?" : fmt::format("{}\n\nContinue installing?", disclaimer);
    if (!m_callbacks.confirm(prompt, true))
        return Err("Cancelled");
    return Ok();
}

Result<> PluginManager::ConfirmDependencies(const manifest_t& repo)
{
    if (repo.dependencies.empty() || options.install_shut_up)
        return Ok();

    const std::string deps = fmt::format("Building '{}' needs: {}", repo.name, fmt::join(repo.dependencies, ", "));
    m_callbacks.on_info(deps);

    const std::string prompt = m_is_cli ? std::string("Are they installed?") : deps + "\n\nAre they installed?";
    if (!m_callbacks.confirm(prompt, true))
        return Err("Cancelled: install the dependencies, then try again");
    return Ok();
}

// ----- building and installing -----
Result<fs::path> PluginManager::BuildPlugin(const plugin_t& plugin, const fs::path& dir, const manifest_t& repo)
{
    if (plugin.output_dir.empty())
        return Err("'{}' has no output-dir in {}", plugin.name, MANIFEST_NAME);

    const fs::path output_dir = dir / plugin.output_dir;
    if (!is_inside(dir, output_dir))
        return Err("'{}' has an output-dir outside the repository: '{}'", plugin.name, plugin.output_dir);

    export_build_env();
    if (const char* sdk = std::getenv("OSHOT_SDK"); !sdk || !*sdk)
        m_callbacks.on_warning(
            "oshot's plugin SDK (headers) wasn't found next to oshot. Set OSHOT_SDK if the "
            "build can't find oshot/oshot_plugin.h");

    for (const std::string& step : plugin.build_steps)
    {
        m_callbacks.on_status(fmt::format("Building '{}': {}", plugin.name, step));
#if OSHOT_WINDOWS
        // /s: cmd strips the outer quotes and runs the rest as is
        const proc_result_t r = run_process(fmt::format("cmd.exe /d /s /c \"{}\"", step), dir, m_callbacks.on_output);
        const bool          not_found = r.status == 9009;
#else
        const proc_result_t r = run_process(std::vector<std::string>{ "sh", "-c", step }, dir, m_callbacks.on_output);
        const bool          not_found = r.status == 127;
#endif
        if (r.status != 0)
        {
            std::string hint;
            if (not_found)
                hint = repo.dependencies.empty()
                           ? "\nA program it runs isn't installed or isn't in PATH."
                           : fmt::format("\nA program it runs isn't installed or isn't in PATH. '{}' needs: {}",
                                         repo.name,
                                         fmt::join(repo.dependencies, ", "));
            return Err("Build step '{}' failed (exit code {}){}\n{}", step, r.status, hint, tail(r.output));
        }
    }

    return find_library_in(output_dir);
}

Result<fs::path> PluginManager::FindPrebuilt(const plugin_t& plugin, const fs::path& dir)
{
    if (!plugin.prebuilt.empty())
    {
        const auto it = plugin.prebuilt.find(OSHOT_TARGET);
        if (it == plugin.prebuilt.end())
        {
            std::vector<std::string> targets;
            for (const auto& [target, _] : plugin.prebuilt)
                targets.push_back(target);
            return Err("'{}' has no library for {} (only for {})", plugin.name, OSHOT_TARGET, fmt::join(targets, ", "));
        }

        const fs::path  lib = dir / it->second;
        std::error_code ec;
        if (!is_inside(dir, lib) || !fs::is_regular_file(lib, ec))
            return Err("'{}': prebuilt library '{}' isn't in the archive", plugin.name, it->second);
        return Ok(lib);
    }

    // No [prebuilt] table: the library sits in output-dir, or next to the manifest
    const fs::path lib_dir = dir / plugin.output_dir;
    if (!is_inside(dir, lib_dir))
        return Err("'{}' has an output-dir outside the archive: '{}'", plugin.name, plugin.output_dir);
    return find_library_in(lib_dir);
}

// Copies the library into the plugin's directory and returns its file name.
// Copied, not moved: the source may be the person's own folder or an
// archive, and the plugins dir can be on another filesystem than the cache.
Result<std::string> PluginManager::CopyLibrary(const std::string& id, const fs::path& library)
{
    const fs::path  dir  = m_plugins_path / id;
    const fs::path  dest = dir / library.filename();
    const fs::path  tmp  = fs::path(dest).concat(".new");
    const fs::path  old  = fs::path(dest).concat(".old");
    std::error_code ec;

    fs::create_directories(dir, ec);
    if (!fs::copy_file(library, tmp, fs::copy_options::overwrite_existing, ec))
        return Err("Failed to copy '{}' to '{}': {}", library.string(), tmp.string(), ec.message());

    // A loaded library can't be overwritten or deleted on Windows, but it can
    // be renamed. The .old file goes away the next time oshot starts.
    if (fs::exists(dest, ec))
    {
        fs::remove(old, ec);
        fs::rename(dest, old, ec);
        if (ec)
            return Err("Failed to replace '{}': {}", dest.string(), ec.message());
    }
    fs::rename(tmp, dest, ec);
    if (ec)
        return Err("Failed to rename '{}' to '{}': {}", tmp.string(), dest.string(), ec.message());
    fs::remove(old, ec);

    // Libraries of an older version with another file name
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
        if (is_library_file(entry.path()) && entry.path() != dest)
            fs::remove(entry.path(), ec);

    return Ok(dest.filename().string());
}

Result<> PluginManager::InstallFromDir(const fs::path&      dir,
                                       const repo_source_t& src,
                                       bool                 is_update,
                                       std::string&         out_name)
{
    Manifest manifest(dir / MANIFEST_NAME);
    TRY_MSG(manifest.ParseManifest(), "{}");

    manifest_t repo = manifest.GetRepo();
    out_name        = repo.name;
    if (src.origin == Origin::GitClone)
    {
        repo.source   = RepoSource::GitRepository;
        repo.url      = src.git_url;  // where updates come from, whatever the manifest says
        repo.git_hash = src.git_hash;
    }
    else
    {
        repo.source = RepoSource::LocalPlugin;
        repo.git_hash.clear();
    }

    const std::optional<manifest_t> installed = m_state_manager.FindRepo(repo.name);
    if (installed && !is_update && !options.install_force &&
        !m_callbacks.confirm(fmt::format("'{}' is already installed. Reinstall it?", repo.name), false))
        return Err(
            "'{}' is already installed. Use `oshotpm update` to update it, or `oshotpm install --force` to "
            "reinstall it",
            repo.name);

    std::vector<const plugin_t*> plugins;
    for (const plugin_t& plugin : repo.plugins)
    {
        if (!Manifest::IsSupported(plugin))
        {
            m_callbacks.on_warning(fmt::format("'{}' doesn't support {}, skipping it", plugin.name, OSHOT_PLATFORM));
            continue;
        }

        if (const auto owner = m_state_manager.FindPlugin(plugin.id); owner && owner->repository != repo.name)
            return Err("The plugin ID '{}' is already installed from '{}'. Uninstall it first: oshotpm uninstall {}",
                       plugin.id,
                       owner->repository,
                       plugin.id);
        plugins.push_back(&plugin);
    }
    if (plugins.empty())
        return Err("None of the plugins in '{}' support {}", repo.name, OSHOT_PLATFORM);

    TRY(ConfirmTrust());
    if (src.origin != Origin::Archive)
        TRY(ConfirmDependencies(repo));

    std::vector<std::string> failed;
    size_t                   installed_count = 0;
    for (const plugin_t* plugin : plugins)
    {
        Result<fs::path> lib =
            src.origin == Origin::Archive ? FindPrebuilt(*plugin, dir) : BuildPlugin(*plugin, dir, repo);

        Result<plugin_info_t> info = Err("");
        if (lib.ok())
            info = inspect_plugin_library(lib.get());

        std::string error;
        if (!lib.ok())
            error = lib.error_v();
        else if (!info.ok())
            error = fmt::format("{} can't be used: {}", lib.get().filename().string(), info.error_v());
        else if (info.get().id != plugin->id)
            error = fmt::format("the library's ID is '{}', but {} says '{}'", info.get().id, MANIFEST_NAME, plugin->id);

        Result<std::string> file = Err("");
        if (error.empty())
        {
            file = CopyLibrary(plugin->id, lib.get());
            if (!file.ok())
                error = file.error_v();
        }

        if (!error.empty())
        {
            m_callbacks.on_error(fmt::format("'{}': {}", plugin->name, error));
            failed.push_back(plugin->name);
            continue;
        }

        m_state_manager.SetPlugin(repo.name, *plugin, file.get());
        m_callbacks.on_success(fmt::format("Installed '{}' ({})", plugin->name, plugin->id));
        ++installed_count;
    }

    // Keeps the old state (and git hash) when nothing could be built, so the
    // next update tries again
    if (installed_count == 0)
        return Err("Nothing from '{}' was installed", repo.name);

    // Plugins this repository no longer has
    if (installed)
    {
        for (const plugin_t& old : installed->plugins)
        {
            if (std::ranges::find(repo.plugins, old.id, &plugin_t::id) != repo.plugins.end())
                continue;
            m_callbacks.on_info(fmt::format("'{}' isn't in '{}' anymore, removing it", old.id, repo.name));
            RemovePluginDir(old.id);
            m_state_manager.RemovePlugin(old.id);
        }
    }

    m_state_manager.SetRepo(repo);
    TRY(m_state_manager.SaveState());

    if (!failed.empty())
        return Err("Failed to install: {}", fmt::join(failed, ", "));
    return Ok();
}

Result<> PluginManager::InstallGit(const std::string& url)
{
    TRY(CheckGit());

    WorkDirGuard work(NewWorkDir("git"));
    m_callbacks.on_status(fmt::format("Cloning {}", url));
    TRY(Git({ "clone", "--recursive", url, work.path.string() }, m_cache_path));
    m_callbacks.on_success("Cloned");

    Result<std::string> hash = Git({ "rev-parse", "HEAD" }, work.path);
    TRY(hash);

    std::string name;
    TRY(InstallFromDir(work.path, { .origin = Origin::GitClone, .git_url = url, .git_hash = hash.get() }, false, name));

    // Keep the clone, so `update` can fetch into it
    const fs::path  cache_dir = m_cache_path / name;
    std::error_code ec;
    if (fs::exists(cache_dir, ec))
        remove_tree(cache_dir);
    fs::rename(work.path, cache_dir, ec);
    if (!ec)
        work.Dismiss();
    else
        spdlog::warn("Failed to move '{}' to '{}': {}", work.path.string(), cache_dir.string(), ec.message());
    return Ok();
}

Result<> PluginManager::InstallArchive(const fs::path& archive)
{
    WorkDirGuard work(NewWorkDir("archive"));
    m_callbacks.on_status(fmt::format("Extracting {}", archive.filename().string()));
    TRY(extract_archive(archive, work.path));

    // The manifest may be in a single top-level folder
    fs::path        root = work.path;
    std::error_code ec;
    if (!fs::exists(root / MANIFEST_NAME, ec))
    {
        std::vector<fs::path> entries;
        for (const fs::directory_entry& entry : fs::directory_iterator(root, ec))
            entries.push_back(entry.path());
        if (entries.size() == 1 && fs::exists(entries.front() / MANIFEST_NAME, ec))
            root = entries.front();
    }

    std::string name;
    return InstallFromDir(root, { .origin = Origin::Archive, .git_url = {}, .git_hash = {} }, false, name);
}

Result<> PluginManager::Install(const std::string& source)
{
    std::error_code ec;
    fs::create_directories(m_cache_path, ec);
    RemoveStaleWorkDirs();

    const fs::path path = fs::absolute(source, ec);
    if (fs::is_directory(path, ec) && fs::exists(path / MANIFEST_NAME, ec))
    {
        std::string name;
        return InstallFromDir(path, { .origin = Origin::UserFolder, .git_url = {}, .git_hash = {} }, false, name);
    }

    // A URL, or a local bare repository
    const bool bare_repo = fs::exists(path / "HEAD", ec) && fs::is_directory(path / "objects", ec);
    if (looks_like_git_url(source) || bare_repo)
        return InstallGit(bare_repo ? path.string() : source);

    if (fs::is_regular_file(path, ec) && is_archive(path))
        return InstallArchive(path);

    return Err("'{}' is not a git URL, a folder with an {}, or a .zip/.tar.gz/.tar.xz archive", source, MANIFEST_NAME);
}

Result<> PluginManager::UpdateRepo(const manifest_t& repo)
{
    const fs::path  dir = m_cache_path / repo.name;
    std::error_code ec;
    std::string     name;

    // The clone is gone (cache cleared): clone it again
    if (!fs::exists(dir / ".git", ec))
    {
        std::string         remote;
        Result<std::string> ls = Git({ "ls-remote", repo.url, "HEAD" }, m_cache_path);
        TRY(ls);
        remote = ls.get().substr(0, ls.get().find_first_of(" \t"));
        if (!remote.empty() && remote == repo.git_hash)
        {
            m_callbacks.on_info(fmt::format("{} is up to date", repo.name));
            return Ok();
        }

        WorkDirGuard work(NewWorkDir("git"));
        m_callbacks.on_status(fmt::format("Cloning {}", repo.url));
        TRY(Git({ "clone", "--recursive", repo.url, work.path.string() }, m_cache_path));
        Result<std::string> hash = Git({ "rev-parse", "HEAD" }, work.path);
        TRY(hash);
        TRY(InstallFromDir(
            work.path, { .origin = Origin::GitClone, .git_url = repo.url, .git_hash = hash.get() }, true, name));

        fs::rename(work.path, m_cache_path / name, ec);
        if (!ec)
            work.Dismiss();
        return Ok();
    }

    m_callbacks.on_status(fmt::format("Checking {} for updates", repo.name));
    TRY(Git({ "fetch", "--quiet" }, dir));
    Result<std::string> upstream = Git({ "rev-parse", "@{u}" }, dir);
    TRY(upstream);
    if (upstream.get() == repo.git_hash)
    {
        m_callbacks.on_info(fmt::format("{} is up to date", repo.name));
        return Ok();
    }

    // oshotpm owns this clone: drop anything a previous build changed or left
    // in it. Build folders can't be reused anyway: the first build ran in a
    // temporary folder, and CMake caches refuse to move.
    TRY(Git({ "reset", "--hard", "--quiet", "@{u}" }, dir));
    TRY(Git({ "clean", "-ffdxq" }, dir));
    TRY(Git({ "submodule", "update", "--init", "--recursive" }, dir));
    TRY(Git({ "submodule", "foreach", "--quiet", "--recursive", "git clean -ffdxq" }, dir));

    m_callbacks.on_status(fmt::format("Updating {} to {:.7}", repo.name, upstream.get()));
    return InstallFromDir(
        dir, { .origin = Origin::GitClone, .git_url = repo.url, .git_hash = upstream.get() }, true, name);
}

Result<> PluginManager::UpdateAll()
{
    std::error_code ec;
    fs::create_directories(m_cache_path, ec);
    RemoveStaleWorkDirs();

    std::vector<std::string> failed;
    bool                     checked_git = false;
    // A copy: updating changes the state
    const std::vector<manifest_t> repos = m_state_manager.GetAllRepos();
    for (const manifest_t& repo : repos)
    {
        if (repo.source == RepoSource::LocalPlugin)
        {
            m_callbacks.on_info(
                fmt::format("{} was installed from a folder or an archive: install it again to update it", repo.name));
            continue;
        }

        if (!checked_git)
        {
            TRY(CheckGit());
            checked_git = true;
        }

        Result<> r = UpdateRepo(repo);
        if (!r.ok())
        {
            m_callbacks.on_error(fmt::format("{}: {}", repo.name, r.error_v()));
            failed.push_back(repo.name);
        }
    }

    if (!failed.empty())
        return Err("Failed to update: {}", fmt::join(failed, ", "));
    return Ok();
}

Result<> PluginManager::Uninstall(const std::string& name)
{
    // Plugin IDs always have a dot, repository names never do
    if (name.find('.') != name.npos)
    {
        const std::optional<plugin_t> plugin = m_state_manager.FindPlugin(name);
        if (!plugin)
            return Err("No plugin with the ID '{}' is installed (see `oshotpm list`)", name);

        RemovePluginDir(plugin->id);
        m_state_manager.RemovePlugin(plugin->id);

        // The last plugin of its repository takes the repository with it
        const std::optional<manifest_t> repo = m_state_manager.FindRepo(plugin->repository);
        if (repo && repo->plugins.empty())
        {
            if (repo->source == RepoSource::GitRepository && Manifest::IsValidName(repo->name))
                remove_tree(m_cache_path / repo->name);
            m_state_manager.RemoveRepo(repo->name);
        }

        TRY(m_state_manager.SaveState());
        m_callbacks.on_success(fmt::format("Uninstalled '{}'", plugin->id));
        return Ok();
    }

    const std::optional<manifest_t> repo =
        Manifest::IsValidName(name) ? m_state_manager.FindRepo(name) : std::optional<manifest_t>();
    if (!repo)
        return Err("No repository named '{}' is installed (see `oshotpm list`)", name);

    for (const plugin_t& plugin : repo->plugins)
        RemovePluginDir(plugin.id);
    if (repo->source == RepoSource::GitRepository)
        remove_tree(m_cache_path / repo->name);

    m_state_manager.RemoveRepo(repo->name);
    TRY(m_state_manager.SaveState());
    m_callbacks.on_success(fmt::format("Uninstalled '{}' and its {} plugin(s)", repo->name, repo->plugins.size()));
    return Ok();
}

Result<> PluginManager::SetEnabled(const std::string& name, bool enabled)
{
    std::string id = name;
    if (const size_t slash = name.find('/'); slash != name.npos)
    {
        const std::optional<manifest_t> repo = m_state_manager.FindRepo(name.substr(0, slash));
        if (!repo)
            return Err("No repository named '{}' is installed", name.substr(0, slash));

        const std::string plugin_name = name.substr(slash + 1);
        auto              it          = std::ranges::find(repo->plugins, plugin_name, &plugin_t::name);
        if (it == repo->plugins.end())
            return Err("'{}' has no plugin named '{}'", repo->name, plugin_name);
        id = it->id;
    }

    const std::optional<plugin_t> plugin = m_state_manager.FindPlugin(id);
    if (!plugin)
        return Err("No plugin with the ID '{}' is installed (see `oshotpm list`)", id);
    if (plugin->enabled == enabled)
    {
        m_callbacks.on_info(fmt::format("'{}' is already {}", id, enabled ? "enabled" : "disabled"));
        return Ok();
    }

    TRY(m_state_manager.SetEnabled(id, enabled));
    TRY(m_state_manager.SaveState());
    m_callbacks.on_success(fmt::format("{} '{}'", enabled ? "Enabled" : "Disabled", id));
    return Ok();
}
