#include "plugin_index.hh"

#include <string>
#include <vector>

#include "catch2/catch_amalgamated.hpp"
#include "common.hpp"
#include "manifest.hh"
#include "plugin.hpp"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("compare_versions()", "[plugins][version]")
{
    SECTION("Release versions")
    {
        CHECK(compare_versions("1.2.3", "1.2.3") == 0);
        CHECK(compare_versions("1.10.0", "1.9.0") > 0);
        CHECK(compare_versions("1.9.0", "1.10.0") < 0);
        CHECK(compare_versions("2.0.0", "1.99.99") > 0);
        CHECK(compare_versions("0.5.1", "0.5.0") > 0);
        CHECK(compare_versions("1.2", "1.2.0") == 0);
        CHECK(compare_versions("1", "1.0.0") == 0);
    }

    SECTION("A release is newer than its pre-releases")
    {
        CHECK(compare_versions("0.5.0", "0.5.0-rc3") > 0);
        CHECK(compare_versions("0.5.0-rc3", "0.5.0") < 0);
        CHECK(compare_versions("0.5.0-rc3", "0.5.0-rc2") > 0);
        CHECK(compare_versions("0.5.0-rc2", "0.5.0-rc2") == 0);
        CHECK(compare_versions("0.5.1-rc1", "0.5.0") > 0);
    }

    SECTION("The SemVer 2.0 precedence example")
    {
        const std::vector<std::string> ordered = { "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                                                   "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0" };
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            for (size_t j = 0; j < ordered.size(); ++j)
            {
                INFO(ordered[i] << " vs " << ordered[j]);
                const int cmp = compare_versions(ordered[i], ordered[j]);
                CHECK((i < j ? cmp < 0 : i > j ? cmp > 0 : cmp == 0));
            }
        }
    }
}

TEST_CASE("sha256_file()", "[plugins][sha256]")
{
    const fs::path dir = test_temp_dir() / "sha256";

    SECTION("Known hashes")
    {
        const fs::path abc = dir / "abc";
        write_file(abc, "abc");
        REQUIRE(sha256_file(abc).ok());
        CHECK(sha256_file(abc).get() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        const fs::path million = dir / "million";
        write_file(million, std::string(1000000, 'a'));
        REQUIRE(sha256_file(million).ok());
        CHECK(sha256_file(million).get() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    // Around the 64-byte block, the 55/56 bytes where the length no longer
    // fits in the last block, and the 64 KiB sha256_file() reads at a time.
    // Bytes i % 251, hashed with Python's hashlib
    SECTION("Block and buffer boundaries")
    {
        const std::vector<std::pair<size_t, std::string_view>> hashes = {
            { 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
            { 1, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d" },
            { 55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59" },
            { 56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562" },
            { 63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488" },
            { 64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108" },
            { 65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781" },
            { 119, "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6" },
            { 65535, "dda402a2c028f0cbbdbc5c6ebae965eed9c75f71236e7022b0386d3455d5ae2f" },
            { 65536, "4b640d85ab3ba30fd02c9fc9db4a8928f416322ad27022ea58a65aaee68a4df2" },
            { 65537, "237356e18b503616912abb8ffaed3a72591e397d4ac294c4637917d48a3f529d" },
            { 200000, "e24bc62381f1224fbbb74688663f8f9743b9680b193edd666835e97b06e730eb" },
        };
        for (const auto& [size, hash] : hashes)
        {
            std::string data(size, '\0');
            for (size_t i = 0; i < size; ++i)
                data[i] = char(i % 251);

            const fs::path path = dir / fmt::format("bytes_{}", size);
            write_file(path, data);

            INFO(size << " bytes");
            REQUIRE(sha256_file(path).ok());
            CHECK(sha256_file(path).get() == hash);
        }
    }

    SECTION("A missing file")
    {
        CHECK_FALSE(sha256_file(dir / "missing").ok());
    }
}

TEST_CASE("parse_plugin_index()", "[plugins][index]")
{
    const fs::path path = test_temp_dir() / "index" / "index.toml";

    SECTION("Plugins, versions, sources and prebuilt files")
    {
        write_file(path, R"(schema = 1
generated = 2026-10-20T12:00:00Z

[plugins."io.github.user.zeta"]
name = "Zeta"
description = "The last one by ID"
homepage = "https://example.com/zeta"
license = "MIT"
owners = ["user"]
authors = ["User <user@example.com>"]
tags = ["text", "ocr"]
network = true

[[plugins."io.github.user.zeta".versions]]
version = "1.2.0"
date = 2026-10-01
abi = 1
imgui = 0
min-oshot = "0.5.0"
changes = "Fixes"
source = { git = "https://example.com/zeta.git", tag = "v1.2.0", commit = "0123abcd", subdir = "plugin" }
artifacts = { linux-x86_64 = { url = "https://example.com/zeta-1.2.0.so", sha256 = "aabb" } }

[[plugins."io.github.user.zeta".versions]]
version = "1.10.0"
abi = 1
imgui = 0
yanked = true
yanked-reason = "Crashes on start"

[[plugins."io.github.user.zeta".versions]]
version = "1.2.0-rc1"
abi = 1
imgui = 0

[[plugins."io.github.user.zeta".versions]]
abi = 1

[plugins."io.github.user.alpha"]

[[plugins."io.github.user.alpha".versions]]
version = "0.1.0"

[plugins."Not A Valid ID"]
name = "Skipped"

[plugins."io.github.user.noversions"]
name = "No versions"
)");

        Result<plugin_index_t> res = parse_plugin_index(path.string());
        REQUIRE(res.ok());
        const plugin_index_t& index = res.get();

        CHECK(index.generated == "2026-10-20T12:00:00Z");

        // Sorted by ID, without the invalid ID. A plugin without versions is kept
        REQUIRE(index.plugins.size() == 3);
        CHECK(index.plugins[0].id == "io.github.user.alpha");
        CHECK(index.plugins[1].id == "io.github.user.noversions");
        CHECK(index.plugins[1].versions.empty());
        CHECK(index.plugins[2].id == "io.github.user.zeta");
        CHECK(index.Find("Not A Valid ID") == nullptr);

        // No name: the ID
        const index_plugin_t* alpha = index.Find("io.github.user.alpha");
        REQUIRE(alpha != nullptr);
        CHECK(alpha->name == "io.github.user.alpha");
        REQUIRE(alpha->versions.size() == 1);
        CHECK(alpha->versions[0].imgui == -1);  // missing, so never installable
        CHECK_FALSE(alpha->versions[0].source.has_value());

        const index_plugin_t* zeta = index.Find("io.github.user.zeta");
        REQUIRE(zeta != nullptr);
        CHECK(zeta->name == "Zeta");
        CHECK(zeta->description == "The last one by ID");
        CHECK(zeta->homepage == "https://example.com/zeta");
        CHECK(zeta->license == "MIT");
        CHECK(zeta->owners == std::vector<std::string>{ "user" });
        CHECK(zeta->authors == std::vector<std::string>{ "User <user@example.com>" });
        CHECK(zeta->tags == std::vector<std::string>{ "text", "ocr" });
        CHECK(zeta->network);

        // Newest first, without the version that has no version string
        REQUIRE(zeta->versions.size() == 3);
        CHECK(zeta->versions[0].version == "1.10.0");
        CHECK(zeta->versions[1].version == "1.2.0");
        CHECK(zeta->versions[2].version == "1.2.0-rc1");

        const index_version_t& yanked = zeta->versions[0];
        CHECK(yanked.yanked);
        CHECK(yanked.yanked_reason == "Crashes on start");

        const index_version_t* v = zeta->FindVersion("1.2.0");
        REQUIRE(v != nullptr);
        CHECK(v->date == "2026-10-01");
        CHECK(v->abi == 1);
        CHECK(v->imgui == 0);
        CHECK(v->min_oshot == "0.5.0");
        CHECK(v->changes == "Fixes");
        CHECK_FALSE(v->yanked);
        REQUIRE(v->source.has_value());
        CHECK(v->source->git == "https://example.com/zeta.git");
        CHECK(v->source->tag == "v1.2.0");
        CHECK(v->source->commit == "0123abcd");
        CHECK(v->source->subdir == "plugin");
        REQUIRE(v->artifacts.contains("linux-x86_64"));
        CHECK(v->artifacts.at("linux-x86_64").url == "https://example.com/zeta-1.2.0.so");
        CHECK(v->artifacts.at("linux-x86_64").sha256 == "aabb");

        CHECK(zeta->FindVersion("9.9.9") == nullptr);
    }

    SECTION("No plugins")
    {
        write_file(path, "schema = 1\n");
        REQUIRE(parse_plugin_index(path.string()).ok());
        CHECK(parse_plugin_index(path.string()).get().plugins.empty());
    }

    SECTION("Another schema")
    {
        write_file(path, "schema = 2\n");
        REQUIRE_FALSE(parse_plugin_index(path.string()).ok());
        CHECK_THAT(parse_plugin_index(path.string()).error_v(), ContainsSubstring("format 2"));

        write_file(path, "[plugins]\n");
        CHECK_FALSE(parse_plugin_index(path.string()).ok());
    }

    SECTION("Broken TOML")
    {
        write_file(path, "schema = 1\n[plugins\n");
        CHECK_FALSE(parse_plugin_index(path.string()).ok());
    }
}

// A version this oshot can install from source
static index_version_t installable(const std::string& version)
{
    index_version_t v;
    v.version = version;
    v.abi     = OSHOT_API_VERSION;
    v.imgui   = 0;
    v.source =
        index_source_t{ .git = "https://example.com/p.git", .tag = "v" + version, .commit = "abc", .subdir = "" };
    return v;
}

TEST_CASE("is_installable()", "[plugins][index]")
{
    std::string why;

    index_version_t v = installable("1.0.0");
    CHECK(is_installable(v, &why));
    CHECK(why.empty());

    SECTION("The plugin ABI must match")
    {
        v.abi = OSHOT_API_VERSION + 1;
        CHECK_FALSE(is_installable(v, &why));
        CHECK_THAT(why, ContainsSubstring("ABI"));
    }

    SECTION("The ImGui version must match, unless it's 0 (no cimgui)")
    {
        v.imgui = OSHOT_IMGUI_VERSION;
        CHECK(is_installable(v));
        v.imgui = OSHOT_IMGUI_VERSION + 1;
        CHECK_FALSE(is_installable(v, &why));
        CHECK_THAT(why, ContainsSubstring("ImGui"));
    }

    SECTION("min-oshot")
    {
        v.min_oshot = "0.0.1";
        CHECK(is_installable(v));
        v.min_oshot = "999.0.0";
        CHECK_FALSE(is_installable(v, &why));
        CHECK_THAT(why, ContainsSubstring("999.0.0"));
    }

    SECTION("A prebuilt file for this target, or a source")
    {
        v.source.reset();
        CHECK_FALSE(is_installable(v, &why));
        CHECK_THAT(why, ContainsSubstring(OSHOT_TARGET));

        v.artifacts.emplace("some-other-target", index_artifact_t{ .url = "x", .sha256 = "y" });
        CHECK_FALSE(is_installable(v));

        v.artifacts.emplace(OSHOT_TARGET, index_artifact_t{ .url = "x", .sha256 = "y" });
        CHECK(is_installable(v));
    }
}

TEST_CASE("pick_version()", "[plugins][index]")
{
    index_plugin_t plugin;
    plugin.id = "io.github.user.plugin";
    std::string why;

    SECTION("The newest installable version that isn't yanked")
    {
        plugin.versions           = { installable("3.0.0"), installable("2.0.0"), installable("1.0.0") };
        plugin.versions[0].yanked = true;
        plugin.versions[1].abi    = OSHOT_API_VERSION + 1;

        const index_version_t* v = pick_version(plugin, &why);
        REQUIRE(v != nullptr);
        CHECK(v->version == "1.0.0");
    }

    SECTION("None installable: why the newest one that isn't yanked can't be")
    {
        plugin.versions           = { installable("3.0.0"), installable("2.0.0") };
        plugin.versions[0].yanked = true;
        plugin.versions[1].abi    = OSHOT_API_VERSION + 1;

        CHECK(pick_version(plugin, &why) == nullptr);
        CHECK_THAT(why, ContainsSubstring("2.0.0"));
        CHECK_THAT(why, ContainsSubstring("ABI"));
    }

    SECTION("Every version yanked")
    {
        plugin.versions           = { installable("1.0.0") };
        plugin.versions[0].yanked = true;
        CHECK(pick_version(plugin, &why) == nullptr);
        CHECK(why == "every version was withdrawn");
    }

    SECTION("No versions")
    {
        CHECK(pick_version(plugin, &why) == nullptr);
    }
}
