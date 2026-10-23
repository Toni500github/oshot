#include "manifest.hh"

#include <string>
#include <vector>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "plugin_index.hh"
#include "state_manager.hh"

using Catch::Matchers::ContainsSubstring;
using strings = std::vector<std::string>;

TEST_CASE("Manifest::IsValidName()", "[plugins][manifest]")
{
    for (const char* name : { "a", "my-plugins", "My_Plugins=2", "0123" })
    {
        INFO("'" << name << "'");
        CHECK(Manifest::IsValidName(name));
    }
    for (const char* name : { "", "my plugins", "a.b", "a/b", "../a", "ä" })
    {
        INFO("'" << name << "'");
        CHECK_FALSE(Manifest::IsValidName(name));
    }
    CHECK(Manifest::IsValidName(std::string(64, 'a')));
    CHECK_FALSE(Manifest::IsValidName(std::string(65, 'a')));
}

TEST_CASE("Manifest::IsValidID()", "[plugins][manifest]")
{
    for (const char* id : { "a.b", "io.github.user.plugin", "io.github.user-name.my_plugin", "0.9" })
    {
        INFO("'" << id << "'");
        CHECK(Manifest::IsValidID(id));
    }
    for (const char* id : { "",
                            "nodot",
                            "Io.github.user",
                            "io.github.User",
                            ".a.b",
                            "a.b.",
                            "a..b",
                            "-a.b",
                            "a.b-",
                            "a.b_",
                            "a.b/c",
                            "a.b c",
                            "a.b\\c",
                            "a.b:c" })
    {
        INFO("'" << id << "'");
        CHECK_FALSE(Manifest::IsValidID(id));
    }
    CHECK(Manifest::IsValidID("a." + std::string(62, 'b')));
    CHECK_FALSE(Manifest::IsValidID("a." + std::string(63, 'b')));
}

TEST_CASE("Manifest::IsSupported()", "[plugins][manifest]")
{
    plugin_t plugin;
    CHECK(Manifest::IsSupported(plugin));
    plugin.platforms = { "all" };
    CHECK(Manifest::IsSupported(plugin));
    plugin.platforms = { OSHOT_PLATFORM };
    CHECK(Manifest::IsSupported(plugin));
    plugin.platforms = { "dragonfly", "netbsd" };
    CHECK(Manifest::IsSupported(plugin) ==
          (std::string_view(OSHOT_PLATFORM) == "dragonfly" || std::string_view(OSHOT_PLATFORM) == "netbsd"));
}

// Parse a manifest from its text
static Result<manifest_t> parse(const std::string& toml)
{
    const fs::path path = test_temp_dir() / "manifest" / MANIFEST_NAME;
    write_file(path, toml);

    Manifest manifest(path);
    TRY(manifest.ParseManifest());
    return Ok(manifest.GetRepo());
}

TEST_CASE("Manifest::ParseManifest()", "[plugins][manifest]")
{
    SECTION("A repository with two plugins")
    {
        const Result<manifest_t> res = parse(fmt::format(R"([repository]
name = "my-plugins"
url = "https://example.com/my-plugins.git"

[dependencies]
all = ["cmake"]
{0} = ["gcc"]
some-other-platform = ["never"]

[upper]
id = "io.github.user.upper"
description = "Uppercase the text"
output-dir = "build"
licenses = ["MIT"]
authors = ["User"]
platforms = ["all"]
build-steps = ["make"]
build-steps-{0} = ["make {0}"]
prebuilt = {{ linux-x86_64 = "linux/libupper.so", windows-x86_64 = "windows/upper.dll" }}

[lower]
id = "io.github.user.lower"
build-steps = ["make lower"]
)",
                                                         OSHOT_PLATFORM));
        if (!res.ok())
            FAIL(res.error_v());
        const manifest_t& repo = res.get();

        CHECK(repo.name == "my-plugins");
        CHECK(repo.url == "https://example.com/my-plugins.git");
        CHECK(repo.dependencies == strings{ "cmake", "gcc" });
        REQUIRE(repo.plugins.size() == 2);

        const auto upper = std::ranges::find(repo.plugins, "io.github.user.upper", &plugin_t::id);
        REQUIRE(upper != repo.plugins.end());
        CHECK(upper->name == "upper");
        CHECK(upper->description == "Uppercase the text");
        CHECK(upper->output_dir == "build");
        CHECK(upper->licenses == strings{ "MIT" });
        CHECK(upper->authors == strings{ "User" });
        CHECK(upper->platforms == strings{ "all" });
        // build-steps-<platform> wins over build-steps
        CHECK(upper->build_steps == strings{ std::string("make ") + OSHOT_PLATFORM });
        CHECK(upper->prebuilt.size() == 2);
        CHECK(upper->prebuilt.at("linux-x86_64") == "linux/libupper.so");

        const auto lower = std::ranges::find(repo.plugins, "io.github.user.lower", &plugin_t::id);
        REQUIRE(lower != repo.plugins.end());
        CHECK(lower->build_steps == strings{ "make lower" });
        CHECK(lower->platforms.empty());
    }

    SECTION("Invalid manifests")
    {
        const std::string                                      plugin = "\n[p]\nid = \"io.github.user.p\"\n";
        const std::vector<std::pair<std::string, std::string>> bad    = {
            { plugin, "no [repository] name" },
            { "[repository]\nname = \"a b\"\n" + plugin, "Repository name 'a b' is invalid" },
            { "[repository]\nname = \"index\"\n" + plugin, "reserved" },
            { "[repository]\nname = \"r\"\n", "doesn't declare any plugin" },
            { "[repository]\nname = \"r\"\n[p]\nid = \"NoDot\"\n", "invalid ID 'NoDot'" },
            { "[repository]\nname = \"r\"\n[p]\n", "invalid ID ''" },
            { "[repository]\nname = \"r\"\n[p]\nid = \"a.b\"\n[q]\nid = \"a.b\"\n", "'a.b' is used twice" },
            { "[repository]\nname = \"r\"\n[p]\nid = \"a.b\"\nplatforms = [\"amiga\"]\n", "unknown platform 'amiga'" },
            { "foo = 1\n[repository]\nname = \"r\"\n" + plugin, "'foo' is not a table" },
            { "[repository]\nname = \"r\"\n[\"bad name\"]\nid = \"a.b\"\n", "plugin name 'bad name' is invalid" },
            { "[repository\nname = \"r\"\n", "" },
        };
        for (const auto& [toml, error] : bad)
        {
            INFO(toml);
            const Result<manifest_t> res = parse(toml);
            REQUIRE_FALSE(res.ok());
            CHECK_THAT(res.error_v(), ContainsSubstring(error));
        }
    }

    SECTION("A missing manifest")
    {
        Manifest manifest(test_temp_dir() / "manifest" / "missing.toml");
        CHECK_FALSE(manifest.ParseManifest().ok());
        CHECK_FALSE(manifest.IsParsed());
    }
}

// StateManager always uses <config dir>/plugins/state.toml. main() points the
// config dir into the test folder, except on Windows, where it can't
static fs::path state_file()
{
    const fs::path& path = get_config_dir() / "plugins" / "state.toml";
    const auto [end, _]  = std::ranges::mismatch(test_temp_dir(), path);
    if (end != test_temp_dir().end())
        SKIP("The config dir " << get_config_dir().string() << " isn't in the test folder");

    fs::remove(path);
    return path;
}

static plugin_t make_plugin(const std::string& id)
{
    plugin_t p;
    p.id          = id;
    p.name        = id.substr(id.rfind('.') + 1);
    p.description = "The " + p.name + " plugin";
    p.authors     = { "User" };
    p.licenses    = { "MIT" };
    p.platforms   = { "all" };
    return p;
}

TEST_CASE("StateManager", "[plugins][state]")
{
    const fs::path path = state_file();

    StateManager state;
    CHECK(state.GetAllRepos().empty());

    manifest_t repo;
    repo.name     = "repo-a";
    repo.url      = "https://example.com/repo-a.git";
    repo.git_hash = "0123abcd";
    state.SetRepo(repo);
    state.SetPlugin("repo-a", make_plugin("io.github.user.zeta"), "libzeta.so");
    state.SetPlugin("repo-a", make_plugin("io.github.user.alpha"), "build/libalpha.so");

    SECTION("Repositories with their plugins, sorted by ID")
    {
        const std::vector<manifest_t>& repos = state.GetAllRepos();
        REQUIRE(repos.size() == 1);
        CHECK(repos[0].name == "repo-a");
        CHECK(repos[0].url == "https://example.com/repo-a.git");
        CHECK(repos[0].git_hash == "0123abcd");
        CHECK(repos[0].source == RepoSource::GitRepository);
        REQUIRE(repos[0].plugins.size() == 2);
        CHECK(repos[0].plugins[0].id == "io.github.user.alpha");
        CHECK(repos[0].plugins[1].id == "io.github.user.zeta");

        const std::optional<plugin_t> alpha = state.FindPlugin("io.github.user.alpha");
        REQUIRE(alpha.has_value());
        CHECK(alpha->name == "alpha");
        CHECK(alpha->description == "The alpha plugin");
        CHECK(alpha->authors == strings{ "User" });
        CHECK(alpha->licenses == strings{ "MIT" });
        CHECK(alpha->platforms == strings{ "all" });
        CHECK(alpha->repository == "repo-a");
        CHECK(alpha->enabled);
        // Only the file name, in the plugin's own folder
        CHECK(alpha->library == get_config_dir() / "plugins" / "io.github.user.alpha" / "libalpha.so");

        CHECK(state.FindRepo("repo-a").has_value());
        CHECK_FALSE(state.FindRepo("repo-b").has_value());
        CHECK_FALSE(state.FindPlugin("io.github.user.missing").has_value());
    }

    SECTION("Enabling and disabling")
    {
        REQUIRE(state.SetEnabled("io.github.user.alpha", false).ok());
        CHECK_FALSE(state.FindPlugin("io.github.user.alpha")->enabled);
        CHECK(state.FindPlugin("io.github.user.zeta")->enabled);

        // Reinstalling keeps it disabled
        state.SetPlugin("repo-a", make_plugin("io.github.user.alpha"), "libalpha.so");
        CHECK_FALSE(state.FindPlugin("io.github.user.alpha")->enabled);

        CHECK_FALSE(state.SetEnabled("io.github.user.missing", true).ok());
    }

    SECTION("Saved and read back")
    {
        REQUIRE(state.SetEnabled("io.github.user.zeta", false).ok());
        REQUIRE(state.SaveState().ok());
        CHECK_THAT(read_file(path), ContainsSubstring("# Installed oshot plugins"));

        const StateManager reread;
        REQUIRE(reread.GetAllRepos().size() == 1);
        CHECK(reread.GetAllRepos()[0].plugins.size() == 2);
        CHECK(reread.FindRepo("repo-a")->git_hash == "0123abcd");
        CHECK_FALSE(reread.FindPlugin("io.github.user.zeta")->enabled);
        CHECK(reread.FindPlugin("io.github.user.alpha")->enabled);

        // Reload() sees what another process saved
        StateManager other;
        other.RemovePlugin("io.github.user.zeta");
        REQUIRE(other.SaveState().ok());
        state.Reload();
        CHECK_FALSE(state.FindPlugin("io.github.user.zeta").has_value());
    }

    SECTION("Removing a repository removes only its plugins")
    {
        manifest_t index;
        index.name   = INDEX_REPO_NAME;
        index.source = RepoSource::Index;
        state.SetRepo(index);

        plugin_t from_index = make_plugin("io.github.user.indexed");
        from_index.version  = "1.2.0";
        state.SetPlugin(INDEX_REPO_NAME, from_index, "libindexed.so");

        CHECK(state.FindRepo(INDEX_REPO_NAME)->source == RepoSource::Index);
        CHECK(state.FindPlugin("io.github.user.indexed")->version == "1.2.0");
        CHECK(state.FindPlugin("io.github.user.alpha")->version.empty());

        state.RemoveRepo("repo-a");
        CHECK_FALSE(state.FindRepo("repo-a").has_value());
        CHECK_FALSE(state.FindPlugin("io.github.user.alpha").has_value());
        CHECK_FALSE(state.FindPlugin("io.github.user.zeta").has_value());
        CHECK(state.FindPlugin("io.github.user.indexed").has_value());
    }

    SECTION("A plugin whose repository is gone isn't listed")
    {
        state.SetPlugin("repo-gone", make_plugin("io.github.user.orphan"), "liborphan.so");
        CHECK_FALSE(state.FindPlugin("io.github.user.orphan").has_value());
    }

    SECTION("A local repository")
    {
        manifest_t local;
        local.name   = "local";
        local.source = RepoSource::LocalPlugin;
        state.SetRepo(local);
        REQUIRE(state.SaveState().ok());
        CHECK(StateManager().FindRepo("local")->source == RepoSource::LocalPlugin);
    }

    fs::remove(path);
}
