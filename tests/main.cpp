#include <chrono>
#include <filesystem>
#include <memory>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "config.hpp"
#include "fmt/format.h"
#include "spdlog/spdlog.h"

namespace fs = std::filesystem;

OcrAPI& test_ocr()
{
    static OcrAPI ocr;
    return ocr;
}

int main(int argc, char* argv[])
{
    // No warnings about creating the config below
    spdlog::set_level(spdlog::level::err);

    // text_extraction.cpp reads g_config->Runtime. A config in a throwaway
    // folder leaves the user's own untouched
    const fs::path config_dir =
        fs::temp_directory_path() /
        fmt::format("oshot_tests_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    g_config = std::make_unique<Config>(config_dir / "config.toml", config_dir);

    const int ret = Catch::Session().run(argc, argv);

    // Before Tesseract's static caches are destroyed (see OcrAPI::End())
    test_ocr().End();
    g_config.reset();

    std::error_code ec;
    fs::remove_all(config_dir, ec);
    return ret;
}
