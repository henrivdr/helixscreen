// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_config_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::MoonrakerConfigManager;

// ============================================================================
// Task 1: has_section
// ============================================================================

TEST_CASE("has_section detects existing section", "[config_manager]") {
    std::string content = "[spoolman]\nserver: http://localhost:7912\n";
    CHECK(MoonrakerConfigManager::has_section(content, "spoolman"));
}

TEST_CASE("has_section returns false when missing", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n";
    CHECK_FALSE(MoonrakerConfigManager::has_section(content, "spoolman"));
}

TEST_CASE("has_section returns false for empty content", "[config_manager]") {
    CHECK_FALSE(MoonrakerConfigManager::has_section("", "spoolman"));
}

TEST_CASE("has_section ignores commented-out sections", "[config_manager]") {
    std::string content = "# [spoolman]\n# server: http://localhost:7912\n";
    CHECK_FALSE(MoonrakerConfigManager::has_section(content, "spoolman"));
}

TEST_CASE("has_section handles section with spaces", "[config_manager]") {
    std::string content = "[update_manager timelapse]\ntype: git_repo\n";
    CHECK(MoonrakerConfigManager::has_section(content, "update_manager timelapse"));
}

TEST_CASE("has_section does not match partial names", "[config_manager]") {
    std::string content = "[spoolman_extra]\nkey: value\n";
    CHECK_FALSE(MoonrakerConfigManager::has_section(content, "spoolman"));
}

TEST_CASE("has_section handles trailing whitespace", "[config_manager]") {
    std::string content = "[spoolman]   \nserver: http://localhost:7912\n";
    CHECK(MoonrakerConfigManager::has_section(content, "spoolman"));
}

TEST_CASE("has_section handles Windows line endings", "[config_manager]") {
    std::string content = "[spoolman]\r\nserver: http://localhost:7912\r\n";
    CHECK(MoonrakerConfigManager::has_section(content, "spoolman"));
}

TEST_CASE("has_section handles leading whitespace on section line", "[config_manager]") {
    std::string content = "  [spoolman]\nserver: http://localhost:7912\n";
    CHECK(MoonrakerConfigManager::has_section(content, "spoolman"));
}

// ============================================================================
// Task 2: add_section
// ============================================================================

TEST_CASE("add_section appends section with entries and comment", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n";
    auto result = MoonrakerConfigManager::add_section(
        content, "spoolman", {{"server", "http://localhost:7912"}, {"sync_rate", "5"}},
        "Added by HelixScreen");
    CHECK(MoonrakerConfigManager::has_section(result, "spoolman"));
    CHECK(result.find("# Added by HelixScreen") != std::string::npos);
    CHECK(result.find("server: http://localhost:7912") != std::string::npos);
    CHECK(result.find("sync_rate: 5") != std::string::npos);
}

TEST_CASE("add_section is idempotent", "[config_manager]") {
    std::string content = "[spoolman]\nserver: http://localhost:7912\n";
    auto result = MoonrakerConfigManager::add_section(content, "spoolman",
                                                      {{"server", "http://other:7912"}}, "");
    // Should not have added a second spoolman section
    size_t first = result.find("[spoolman]");
    size_t second = result.find("[spoolman]", first + 1);
    CHECK(second == std::string::npos);
}

TEST_CASE("add_section handles empty content", "[config_manager]") {
    auto result = MoonrakerConfigManager::add_section("", "spoolman",
                                                      {{"server", "http://localhost:7912"}}, "");
    CHECK(MoonrakerConfigManager::has_section(result, "spoolman"));
    CHECK(result.find("server: http://localhost:7912") != std::string::npos);
}

TEST_CASE("add_section preserves multiple entries in order", "[config_manager]") {
    auto result = MoonrakerConfigManager::add_section(
        "", "spoolman",
        {{"server", "http://localhost:7912"}, {"sync_rate", "5"}, {"connection_timeout", "30"}},
        "");
    size_t server_pos = result.find("server: http://localhost:7912");
    size_t sync_pos = result.find("sync_rate: 5");
    size_t timeout_pos = result.find("connection_timeout: 30");
    CHECK(server_pos != std::string::npos);
    CHECK(sync_pos != std::string::npos);
    CHECK(timeout_pos != std::string::npos);
    CHECK(server_pos < sync_pos);
    CHECK(sync_pos < timeout_pos);
}

TEST_CASE("add_section no comment line when comment is empty", "[config_manager]") {
    auto result = MoonrakerConfigManager::add_section("", "spoolman",
                                                      {{"server", "http://localhost:7912"}}, "");
    CHECK(result.find('#') == std::string::npos);
}

TEST_CASE("add_section handles section with no entries", "[config_manager]") {
    auto result = MoonrakerConfigManager::add_section("", "spoolman", {}, "");
    CHECK(MoonrakerConfigManager::has_section(result, "spoolman"));
}

TEST_CASE("add_section result passes has_section check", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n";
    auto result = MoonrakerConfigManager::add_section(content, "spoolman",
                                                      {{"server", "http://localhost:7912"}}, "");
    CHECK(MoonrakerConfigManager::has_section(result, "spoolman"));
}

// ============================================================================
// Task 3: remove_section
// ============================================================================

TEST_CASE("remove_section removes section and its entries", "[config_manager]") {
    std::string content =
        "[server]\nhost: localhost\n\n[spoolman]\nserver: http://localhost:7912\n";
    auto result = MoonrakerConfigManager::remove_section(content, "spoolman");
    CHECK_FALSE(MoonrakerConfigManager::has_section(result, "spoolman"));
    CHECK(result.find("server: http://localhost:7912") == std::string::npos);
    CHECK(MoonrakerConfigManager::has_section(result, "server"));
}

TEST_CASE("remove_section removes preceding comment block", "[config_manager]") {
    std::string content =
        "[server]\nhost: localhost\n\n# Added by HelixScreen\n[spoolman]\nserver: "
        "http://localhost:7912\n";
    auto result = MoonrakerConfigManager::remove_section(content, "spoolman");
    CHECK(result.find("Added by HelixScreen") == std::string::npos);
    CHECK(MoonrakerConfigManager::has_section(result, "server"));
}

TEST_CASE("remove_section removes section between other sections", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n\n[spoolman]\nserver: "
                          "http://localhost:7912\n\n[authorization]\nenabled: true\n";
    auto result = MoonrakerConfigManager::remove_section(content, "spoolman");
    CHECK_FALSE(MoonrakerConfigManager::has_section(result, "spoolman"));
    CHECK(MoonrakerConfigManager::has_section(result, "server"));
    CHECK(MoonrakerConfigManager::has_section(result, "authorization"));
}

TEST_CASE("remove_section is no-op when section does not exist", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n";
    auto result = MoonrakerConfigManager::remove_section(content, "spoolman");
    CHECK(result == content);
}

TEST_CASE("remove_section handles section at end of file", "[config_manager]") {
    std::string content =
        "[server]\nhost: localhost\n\n[spoolman]\nserver: http://localhost:7912\n";
    auto result = MoonrakerConfigManager::remove_section(content, "spoolman");
    CHECK_FALSE(MoonrakerConfigManager::has_section(result, "spoolman"));
    CHECK(MoonrakerConfigManager::has_section(result, "server"));
}

TEST_CASE("remove_section handles section with spaces in name", "[config_manager]") {
    std::string content =
        "[update_manager timelapse]\ntype: git_repo\npath: ~/timelapse\n\n[server]\nhost: "
        "localhost\n";
    auto result = MoonrakerConfigManager::remove_section(content, "update_manager timelapse");
    CHECK_FALSE(MoonrakerConfigManager::has_section(result, "update_manager timelapse"));
    CHECK(MoonrakerConfigManager::has_section(result, "server"));
}

TEST_CASE("remove_section after add returns to original-like state", "[config_manager]") {
    std::string original = "[server]\nhost: localhost\n";
    auto added = MoonrakerConfigManager::add_section(
        original, "spoolman", {{"server", "http://localhost:7912"}}, "Added by HelixScreen");
    auto removed = MoonrakerConfigManager::remove_section(added, "spoolman");
    CHECK_FALSE(MoonrakerConfigManager::has_section(removed, "spoolman"));
    CHECK(MoonrakerConfigManager::has_section(removed, "server"));
}

// ============================================================================
// Task 4: has_include_line, add_include_line, get_section_value
// ============================================================================

TEST_CASE("has_include_line detects existing include", "[config_manager]") {
    std::string content = "[include helixscreen.conf]\n[server]\nhost: localhost\n";
    CHECK(MoonrakerConfigManager::has_include_line(content));
}

TEST_CASE("has_include_line returns false when missing", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n";
    CHECK_FALSE(MoonrakerConfigManager::has_include_line(content));
}

TEST_CASE("has_include_line ignores commented include", "[config_manager]") {
    std::string content = "# [include helixscreen.conf]\n[server]\nhost: localhost\n";
    CHECK_FALSE(MoonrakerConfigManager::has_include_line(content));
}

TEST_CASE("add_include_line adds before first section", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n";
    auto result = MoonrakerConfigManager::add_include_line(content);
    CHECK(MoonrakerConfigManager::has_include_line(result));
    size_t include_pos = result.find("[include helixscreen.conf]");
    size_t server_pos = result.find("[server]");
    CHECK(include_pos < server_pos);
}

TEST_CASE("add_include_line is idempotent", "[config_manager]") {
    std::string content = "[include helixscreen.conf]\n[server]\nhost: localhost\n";
    auto result = MoonrakerConfigManager::add_include_line(content);
    size_t first = result.find("[include helixscreen.conf]");
    size_t second = result.find("[include helixscreen.conf]", first + 1);
    CHECK(second == std::string::npos);
}

TEST_CASE("add_include_line handles empty content", "[config_manager]") {
    auto result = MoonrakerConfigManager::add_include_line("");
    CHECK(MoonrakerConfigManager::has_include_line(result));
}

TEST_CASE("add_include_line inserts after leading comments but before first section",
          "[config_manager]") {
    std::string content =
        "# Moonraker configuration\n# Generated by setup\n\n[server]\nhost: localhost\n";
    auto result = MoonrakerConfigManager::add_include_line(content);
    CHECK(MoonrakerConfigManager::has_include_line(result));
    size_t comment_pos = result.find("# Moonraker configuration");
    size_t include_pos = result.find("[include helixscreen.conf]");
    size_t server_pos = result.find("[server]");
    CHECK(comment_pos < include_pos);
    CHECK(include_pos < server_pos);
}

TEST_CASE("get_section_value extracts value from section", "[config_manager]") {
    std::string content = "[spoolman]\nserver: http://localhost:7912\nsync_rate: 5\n";
    CHECK(MoonrakerConfigManager::get_section_value(content, "spoolman", "server") ==
          "http://localhost:7912");
}

TEST_CASE("get_section_value returns empty for missing key", "[config_manager]") {
    std::string content = "[spoolman]\nserver: http://localhost:7912\n";
    CHECK(MoonrakerConfigManager::get_section_value(content, "spoolman", "missing_key").empty());
}

TEST_CASE("get_section_value returns empty for missing section", "[config_manager]") {
    std::string content = "[server]\nhost: localhost\n";
    CHECK(MoonrakerConfigManager::get_section_value(content, "spoolman", "server").empty());
}

TEST_CASE("get_section_value does not cross section boundaries", "[config_manager]") {
    std::string content =
        "[server]\nhost: localhost\n\n[spoolman]\nserver: http://localhost:7912\n";
    // 'host' key is in [server], not [spoolman]
    CHECK(MoonrakerConfigManager::get_section_value(content, "spoolman", "host").empty());
}

TEST_CASE("get_section_value handles whitespace around colon", "[config_manager]") {
    std::string content = "[spoolman]\nserver  :  http://localhost:7912\n";
    CHECK(MoonrakerConfigManager::get_section_value(content, "spoolman", "server") ==
          "http://localhost:7912");
}
