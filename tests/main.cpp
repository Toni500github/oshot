#include <chrono>
#include <cstdio>
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

// Prints each test case's name before its checks, and its results after:
// Catch2 alone prints nothing for the ones that pass
class ProgressListener : public Catch::EventListenerBase
{
public:
    using EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override
    {
        fmt::println("\n{}", info.name);
        m_start = std::chrono::steady_clock::now();
    }

    void testCaseEnded(const Catch::TestCaseStats& stats) override
    {
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - m_start;
        fmt::println("  -> {} passed, {} failed ({:.1f} s)",
                     stats.totals.assertions.passed,
                     stats.totals.assertions.failed + stats.totals.assertions.failedButOk,
                     took.count());
        std::fflush(stdout);
    }

private:
    std::chrono::steady_clock::time_point m_start;
};
CATCH_REGISTER_LISTENER(ProgressListener)

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
