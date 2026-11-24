#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2/catch_amalgamated.hpp"
#include "plugin.hpp"
#include "state_manager.hh"

using namespace std::chrono_literals;

using chain = std::vector<std::pair<std::string, bool>>;

TEST_CASE("arrange_text_transformers()", "[plugins][transformers]")
{
    const std::vector<std::string> available = { "io.github.a.one", "io.github.b.two", "io.github.c.three" };

    SECTION("Nothing saved: sorted by ID, all on")
    {
        const std::vector<std::string> unsorted = { "io.github.c.three", "io.github.a.one", "io.github.b.two" };
        CHECK(arrange_text_transformers(unsorted, {}) ==
              chain{ { "io.github.a.one", true }, { "io.github.b.two", true }, { "io.github.c.three", true } });
    }

    SECTION("The saved order first, then the new ones by ID")
    {
        const text_transformers_t saved{ .order = { "io.github.c.three", "io.github.a.one" }, .disabled = {} };
        CHECK(arrange_text_transformers(available, saved) ==
              chain{ { "io.github.c.three", true }, { "io.github.a.one", true }, { "io.github.b.two", true } });
    }

    SECTION("Disabled ones stay in their place, switched off")
    {
        const text_transformers_t saved{ .order    = { "io.github.b.two", "io.github.a.one", "io.github.c.three" },
                                         .disabled = { "io.github.a.one", "io.github.c.three" } };
        CHECK(arrange_text_transformers(available, saved) ==
              chain{ { "io.github.b.two", true }, { "io.github.a.one", false }, { "io.github.c.three", false } });
    }

    SECTION("Saved IDs of plugins that aren't loaded, and duplicates, are skipped")
    {
        const text_transformers_t saved{ .order    = { "io.github.x.gone", "io.github.b.two", "io.github.b.two" },
                                         .disabled = { "io.github.x.gone" } };
        CHECK(arrange_text_transformers(available, saved) ==
              chain{ { "io.github.b.two", true }, { "io.github.a.one", true }, { "io.github.c.three", true } });
    }
}

// A step that appends `suffix`, allocating the result like a plugin does
static text_step_t appender(const std::string& id, const std::string& suffix)
{
    return { .id = id, .fn = [suffix](oshot_str_t in, oshot_str_t* out) {
                const std::string s = std::string(in.p, in.len) + suffix;
                *out                = g_oshot_host.str_new(s.data(), s.size());
                return true;
            } };
}

TEST_CASE("run_text_steps()", "[plugins][transformers]")
{
    std::vector<std::string> slow, bad;
    auto on_slow = [&](const text_step_t& step, std::chrono::milliseconds) { slow.push_back(step.id); };
    auto on_bad  = [&](const text_step_t& step) { bad.push_back(step.id); };
    auto run     = [&](const std::vector<text_step_t>& steps, std::chrono::milliseconds limit = 1000ms) {
        return run_text_steps("text", steps, limit, on_slow, on_bad);
    };

    SECTION("In order, each one getting the one before's text")
    {
        CHECK(run({ appender("a", "-a"), appender("b", "-b") }) == "text-a-b");
        CHECK(run({ appender("b", "-b"), appender("a", "-a") }) == "text-b-a");
        CHECK(run({}) == "text");
    }

    SECTION("Returning false passes the text through, and what it allocated is freed")
    {
        const text_step_t pass{ .id = "pass", .fn = [](oshot_str_t in, oshot_str_t* out) {
                                   CHECK(std::string(in.p, in.len) == "text-a");
                                   *out = g_oshot_host.str_new("ignored", 7);
                                   return false;
                               } };
        CHECK(run({ appender("a", "-a"), pass, appender("b", "-b") }) == "text-a-b");
    }

    SECTION("Empty text is a valid result")
    {
        const text_step_t clear{ .id = "clear", .fn = [](oshot_str_t, oshot_str_t* out) {
                                    *out = g_oshot_host.str_new(nullptr, 0);
                                    return true;
                                } };
        const text_step_t unset{ .id = "unset", .fn = [](oshot_str_t, oshot_str_t*) { return true; } };
        CHECK(run({ clear }).empty());
        // true with { NULL, 0 } still means the empty string
        CHECK(run({ unset }).empty());
        CHECK(bad.empty());
    }

    SECTION("true with a NULL pointer and a length is skipped and reported")
    {
        const text_step_t broken{ .id = "broken", .fn = [](oshot_str_t, oshot_str_t* out) {
                                     out->len = 5;
                                     return true;
                                 } };
        CHECK(run({ broken, appender("a", "-a") }) == "text-a");
        CHECK(bad == std::vector<std::string>{ "broken" });
    }

    SECTION("A slow step is reported, and its text still used")
    {
        const text_step_t sleepy{ .id = "sleepy", .fn = [](oshot_str_t in, oshot_str_t* out) {
                                     std::this_thread::sleep_for(30ms);
                                     *out = g_oshot_host.str_new("slow", 4);
                                     return true;
                                 } };
        CHECK(run({ appender("a", "-a"), sleepy }, 10ms) == "slow");
        CHECK(slow == std::vector<std::string>{ "sleepy" });
    }
}
