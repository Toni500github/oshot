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

#ifndef _UTIL_PATHS_HPP_
#define _UTIL_PATHS_HPP_

// Directories, path lookup and removal

#include <filesystem>
#include <string>
#include <string_view>

#include "platform.hpp"

namespace fs = std::filesystem;

// ------------------------------
// Macros
// ------------------------------

#if OSHOT_WINDOWS
#  define DIR_SEP_STR "\\"
#  define DIR_SEP     '\\'
#else
#  define DIR_SEP_STR "/"
#  define DIR_SEP     '/'
#endif

/**
 * What get_relative_path() and which() return when nothing is found.
 * Unlikely enough to never be a real path.
 **/
#define UNKNOWN "|<u2n4kn6ow8n>|"

// ------------------------------
// Functions
// ------------------------------

/**
 * Expand a leading '~' (alone or before a directory separator) to the home
 * directory, and $NAME or ${NAME} to the environment variable's value.
 * Unset variables are left as they are.
 * @param ret The string to expand
 **/
std::string expand_var(std::string ret);

/**
 * Find a file in one of the directories of a ':'-separated environment variable (PATH, XDG_DATA_DIRS, ...)
 * @param relative_path The path to look for, relative to each directory
 * @param env The environment variable's name, without $
 * @param mode Mode bits the file must have, from sys/stat.h,
 *             such as S_IXUSR for executables or S_IFDIR for directories
 * @return The first full path found, else UNKNOWN
 **/
std::string get_relative_path(const std::string_view relative_path, const char* env, const long long mode);

/**
 * Find a command in PATH, like `which`
 * @param command The command to look up in $PATH
 * @return The full path of the executable, else UNKNOWN
 **/
std::string which(const std::string_view command);

/**
 * Get the user's home directory. Dies if it can't be found
 * @return $HOME (%USERPROFILE% on Windows), else the passwd entry's
 **/
fs::path get_home_dir();

/**
 * Get the user config directory
 * @return $XDG_CONFIG_HOME or ~/.config/ (%APPDATA% on Windows)
 **/
fs::path get_home_config_dir();

/**
 * Get the user cache directory
 * @return $XDG_CACHE_HOME or ~/.cache/ (%LOCALAPPDATA% on Windows)
 **/
fs::path get_home_cache_dir();

/**
 * Get the user pictures directory, where the save dialog opens first
 * @return $XDG_PICTURES_DIR, or the one in user-dirs.dirs, else ~/Pictures (the Pictures known folder on Windows)
 **/
fs::path get_home_pictures_dir();

/**
 * Get oshot's config directory, where "config.toml" is
 * @return get_home_config_dir() / "oshot"
 **/
fs::path get_config_dir();

/**
 * Get oshot's cache directory, where "cache.toml" is
 * @return get_home_cache_dir() / "oshot"
 **/
fs::path get_cache_dir();

#if !OSHOT_WINDOWS
/**
 * Get the directory for runtime files, like the ProcessLock files
 * @return $XDG_RUNTIME_DIR, else the temp directory
 **/
fs::path get_runtime_dir();
#endif

/**
 * Get the path of the running executable
 * @return The absolute path, or empty if it can't be found (e.g. on OpenBSD)
 **/
fs::path get_exe_path();

/**
 * Get the directory of the running executable
 * @return get_exe_path()'s parent, or empty
 **/
fs::path get_exe_dir();

/**
 * Get the OCR models shipped next to the executable (Windows zip/installer, macOS .app)
 * @return The directory, or empty if there's none
 **/
fs::path get_bundled_models_dir();

/**
 * Get where the system's tesseract models are, which differs per distro.
 * Only the default of the ocr-path option
 **/
std::string get_default_ocr_path();

/**
 * fs::remove_all() that also deletes read-only files (on Windows), and only logs on failure
 * @param path The file or directory to remove
 * @return false if something couldn't be removed
 **/
bool remove_tree(const fs::path& path) noexcept;

#endif  // _UTIL_PATHS_HPP_
