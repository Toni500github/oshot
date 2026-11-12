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

#ifndef _CLI_HPP_
#define _CLI_HPP_

#include <filesystem>

/**
 * First pass over the options, before Config is constructed. Finds the config path, and turns off dialogs for a
 * script run (--extract-*, --selection), which reports errors on stderr. Dies if a -C file doesn't exist
 * @param argc The argument count from main()
 * @param argv The arguments from main()
 * @param configDir The config folder, where the default config.toml is
 * @return The config file to load
 **/
std::filesystem::path parse_config_path(int argc, char* argv[], const std::filesystem::path& configDir);  // NOLINT

/**
 * Second pass over the options, applying them to g_config. Exits for --help, --version, --gen-config and
 * invalid options
 * @param argc The argument count from main()
 * @param argv The arguments from main()
 * @param configFile The config file in use, where a bare --gen-config writes
 * @return false on an option it doesn't handle
 **/
bool parseargs(int argc, char* argv[], const std::filesystem::path& configFile);  // NOLINT

/**
 * Script mode: --extract-text/--extract-barcode, or --instant-copy/--instant-save with --selection.
 * Takes the screenshot (or -f image), crops it to the selection and extracts/copies/saves it
 * with no window, dialog or notification
 * @return The process exit code
 **/
int run_without_overlay();

#endif  // _CLI_HPP_
