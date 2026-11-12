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

#include "cli.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "cache.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#include "fmt/compile.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "getopt_port.h"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "switch_fnv1a.hpp"
#include "text_extraction.hpp"
#include "texts.hpp"
#include "util.hpp"
#include "util/dialogs.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"

// clang-format off
// https://cfengine.com/blog/2021/optional-arguments-with-getopt-long/
// because "--opt-arg arg" won't work
// but "--opt-arg=arg" will
#define OPTIONAL_ARGUMENT_IS_PRESENT \
    ((optarg == NULL && optind < argc && argv[optind][0] != '-') \
     ? (bool) (optarg = argv[optind++]) \
     : (optarg != NULL))
// clang-format on

// Show the version and build details, then exit successfully
static void version()
{
#if OSHOT_WINDOWS
    dialog::create("oshot version", NVD_DIALOG_SIMPLE, version_infos);
#else
    fmt::print(FMT_COMPILE("{}"), version_infos);
    fmt::print("\n");
#endif

    std::exit(EXIT_SUCCESS);
}

// Show the help, then exit with 1 after a command line error, else 0
static void help(const std::string_view error = {})
{
#if OSHOT_WINDOWS
    if (!error.empty())
        dialog::create("oshot", NVD_DIALOG_WARNING, fmt::format("{}\n\n{}", error, oshot_help));
    else
        dialog::create("oshot help", NVD_DIALOG_SIMPLE, std::string(oshot_help));
#else
    if (!error.empty())
        fmt::print("{}\n\n", error);
    fmt::print(FMT_COMPILE("{}"), oshot_help);
    fmt::print("\n");
#endif

    std::exit(!error.empty());
}

// What getopt_long() just rejected. After a missing argument optind is past the
// last argument (POSIX), so it can't index argv
static std::string bad_option_error(const int argc, char* argv[])
{
    const std::string opt =
        optopt != 0 ? fmt::format("-{}", char(optopt)) : std::string(argv[std::min(optind, argc) - 1]);
    if (optind > argc)
        return fmt::format("Option {} needs an argument", opt);
    return fmt::format("Invalid option: {}", opt);
}

// clang-format off
// NOLINTBEGIN
// Both passes need the whole table, or the first one would stop at the argument of an option it doesn't know
static const char* const optstring = "-Vhtgd:C:f:O:";
static const struct option opts[] = {
    {"version", no_argument,       0, 'V'},
    {"help",    no_argument,       0, 'h'},
    {"tray",    no_argument,       0, 't'},
    {"gui",     no_argument,       0, 'g'},
    {"delay",   required_argument, 0, 'd'},
    {"config",  required_argument, 0, 'C'},
    {"source",  required_argument, 0, 'f'},
    {"override",required_argument, 0, 'O'},

    {"instant-save",    optional_argument, 0, "instant-save"_fnv1a16},
    {"instant-copy",    no_argument,       0, "instant-copy"_fnv1a16},
    {"extract-text",    no_argument,       0, "extract-text"_fnv1a16},
    {"extract-barcode", no_argument,       0, "extract-barcode"_fnv1a16},
    {"selection",       required_argument, 0, "selection"_fnv1a16},
    {"gen-config",      optional_argument, 0, "gen-config"_fnv1a16},

    {0,0,0,0}
};

// First pass over the options, before Config is constructed: the config path, and whether this
// is a script run (--extract-*, --selection), which reports errors on stderr and never in a dialog
fs::path parse_config_path(int argc, char* argv[], const fs::path& configDir)
{
    int opt = 0;
    int option_index = 0;
    fs::path config = configDir / "config.toml";

    optind = 1;
    while ((opt = getopt_long(argc, argv, optstring, opts, &option_index)) != -1)
    {
        switch (opt)
        {
            case "extract-text"_fnv1a16:
            case "extract-barcode"_fnv1a16:
            case "selection"_fnv1a16:
                dialog::disable(); break;

            case 'C':
                config = optarg; break;

            // everything else, errors included, is for parseargs()
            default:
                break;
        }
    }

    if (!fs::exists(config) && config != configDir / "config.toml")
        dialog::die("config file '{}' doesn't exist", config.string());
    return config;
}

bool parseargs(int argc, char* argv[], const fs::path& configFile)
{
    int opt = 0;
    int option_index = 0;

    optind = 1;
    while ((opt = getopt_long(argc, argv, optstring, opts, &option_index)) != -1)
    {
        switch (opt)
        {
            case 0:
            case 'C':
                break;
            case '?':
            case ':':
                help(bad_option_error(argc, argv)); break;

            case 'V':
                version(); break;
            case 'h':
                help(); break;
            case 'f':
                g_config->Runtime.source_file = optarg; break;
            case 'O':
                g_config->OverrideOption(optarg); break;
            case 'd':
                if (int delay = -1;
                    std::from_chars(optarg, optarg + strlen(optarg), delay).ec == std::errc() && delay >= 0)
                    g_config->OverrideOption("default.delay", delay);
                else
                    help(fmt::format("Invalid delay '{}', expected milliseconds", optarg));
                break;
            case 't':
                g_config->Runtime.only_launch_tray = true; break;
            case 'g':
                g_config->Runtime.only_launch_gui = true; break;

            case "instant-save"_fnv1a16:
                g_config->Runtime.instant_copy_save = SavingOp::File;
                if (OPTIONAL_ARGUMENT_IS_PRESENT)
                    g_config->Runtime.instant_save_dir = expand_var(optarg);
                break;
            case "instant-copy"_fnv1a16:
                g_config->Runtime.instant_copy_save = SavingOp::Clipboard; break;
            case "extract-text"_fnv1a16:
                g_config->Runtime.extract_text = true; break;
            case "extract-barcode"_fnv1a16:
                g_config->Runtime.extract_barcode = true; break;
            case "selection"_fnv1a16:
            {
                Result<region_t> region = parse_region(optarg);
                if (!region.ok())
                {
                    spdlog::error("{}", region.error_v());
                    std::exit(EXIT_FAILURE);
                }
                g_config->Runtime.selection = region.get();
                break;
            }

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
// NOLINTEND

// Script mode: --extract-text/--extract-barcode, or --instant-copy/--instant-save
// with --selection. Takes the screenshot (or -f image), crops it to the
// selection and extracts/copies/saves it with no window, dialog or notification
int run_without_overlay()
{
    const auto& rt      = g_config->Runtime;
    const bool  extract = rt.extract_text || rt.extract_barcode;

    if (extract && rt.instant_copy_save == SavingOp::File)
    {
        spdlog::error("--instant-save can't be used with --extract-text/--extract-barcode");
        return EXIT_FAILURE;
    }

    Result<capture_result_t> loaded{ Err() };
    if (!rt.source_file.empty())
    {
        loaded = load_image_rgba(rt.source_file);
    }
    else
    {
        if (g_config->File.delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(g_config->File.delay));
        loaded = capture_full_screen(get_session_type());
    }

    if (!loaded.ok())
    {
        spdlog::error("Failed to get the image: {}", loaded.error_v());
        return EXIT_FAILURE;
    }

    capture_result_t image = std::move(loaded.get());
    if (rt.selection)
    {
        Result<capture_result_t> cropped = crop_image(image, *rt.selection);
        if (!cropped.ok())
        {
            spdlog::error("--selection: {}", cropped.error_v());
            return EXIT_FAILURE;
        }
        image = std::move(cropped.get());
    }

    if (!extract)
    {
        const ImageExt ext = g_config->File.image_out_type.second;
        if (rt.instant_copy_save == SavingOp::Clipboard)
        {
            const Result<>& res = g_clipboard.CopyImage(image, ext);
            if (!res.ok())
            {
                spdlog::error("Failed to copy the image: {}", res.error_v());
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        // There's no save dialog here. Use the --instant-save folder (or the one the
        // overlay last saved to) and print where the image went
        const fs::path dir = rt.instant_save_dir.empty()
                                 ? g_cache->GetValue(CacheEntry::ImgSavePath, get_home_pictures_dir().string())
                                 : rt.instant_save_dir;

        const Result<std::string>& path = save_image_to_dir(dir, image, ext);
        if (!path.ok())
        {
            spdlog::error("{}", path.error_v());
            return EXIT_FAILURE;
        }
        fmt::println("{}", path.get());
        return EXIT_SUCCESS;
    }

    std::vector<std::string> outputs;
    if (rt.extract_text)
    {
        OcrAPI          ocr;
        const Result<>& configured = ocr.Configure(g_config->File.ocr_path.c_str(), g_config->File.ocr_model.c_str());
        if (!configured.ok())
        {
            spdlog::error("Failed to load the OCR model '{}' from '{}': {}",
                          g_config->File.ocr_model,
                          g_config->File.ocr_path,
                          configured.error_v());
            return EXIT_FAILURE;
        }

        Result<ocr_result_t> text = ocr.ExtractTextCapture(image);
        if (text.ok())
            outputs.push_back(std::move(text.get().data));
        else
            spdlog::warn("OCR: {}", text.error_v());
    }

    if (rt.extract_barcode)
    {
        BarcodeAPI               scanner;
        Result<barcode_result_t> codes = scanner.ExtractTextsCapture(image);
        if (codes.ok())
            std::ranges::move(codes.get().datas, std::back_inserter(outputs));
        else
            spdlog::warn("Barcode: {}", codes.error_v());
    }

    if (outputs.empty())
    {
        spdlog::error("Nothing found in the image");
        return EXIT_FAILURE;
    }

    const std::string& joined = fmt::format("{}", fmt::join(outputs, "\n"));
    if (rt.instant_copy_save == SavingOp::Clipboard)
    {
        const Result<>& res = g_clipboard.CopyText(joined);
        if (!res.ok())
        {
            spdlog::error("{}", res.error_v());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    fmt::println("{}", joined);
    return EXIT_SUCCESS;
}
