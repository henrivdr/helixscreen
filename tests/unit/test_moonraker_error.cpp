// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_error.h"

#include "../catch_amalgamated.hpp"

#include "hv/json.hpp"

using nlohmann::json;

// ============================================================================
// extract_friendly_message() tests
// ============================================================================

TEST_CASE("extract_friendly_message parses Klipper error strings",
          "[moonraker][error]") {
    SECTION("extracts message from Python dict repr with single quotes") {
        auto result = MoonrakerError::extract_friendly_message(
            "{'error': 'WebRequestError', 'message': 'Must home axis first'}");
        REQUIRE(result == "Must home axis first");
    }

    SECTION("extracts message from JSON with double quotes and space") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"error": "WebRequestError", "message": "Must home axis first"})");
        REQUIRE(result == "Must home axis first");
    }

    SECTION("extracts message from JSON without space after colon") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"error":"WebRequestError","message":"Must home axis first"})");
        REQUIRE(result == "Must home axis first");
    }

    SECTION("returns raw string when no message key found") {
        auto result = MoonrakerError::extract_friendly_message("Some plain error text");
        REQUIRE(result == "Some plain error text");
    }

    SECTION("returns raw string for empty input") {
        auto result = MoonrakerError::extract_friendly_message("");
        REQUIRE(result == "");
    }

    SECTION("handles message with spaces and punctuation") {
        auto result = MoonrakerError::extract_friendly_message(
            "{'error': 'CommandError', 'message': 'Probe triggered prior to movement'}");
        REQUIRE(result == "Probe triggered prior to movement");
    }

    SECTION("handles message-only dict") {
        auto result = MoonrakerError::extract_friendly_message(
            "{'message': 'Timer too close'}");
        REQUIRE(result == "Timer too close");
    }

    // Creality/Klipper key-error envelope uses "msg" (not "message"), e.g. the
    // K2's SET_HEATER_TEMPERATURE rejection. Without this, toasts show raw JSON.
    SECTION("extracts msg from Creality key-error envelope") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"code":"key69", "msg": "The value 'chamber' is not valid for HEATER", "values": ["chamber", "HEATER"]})");
        REQUIRE(result == "The value 'chamber' is not valid for HEATER");
    }

    SECTION("extracts msg without space after colon") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"code":"key12","msg":"Heater extruder not heating at expected rate"})");
        REQUIRE(result == "Heater extruder not heating at expected rate");
    }

    SECTION("prefers message over msg when both present") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"msg":"raw detail","message":"friendly summary"})");
        REQUIRE(result == "friendly summary");
    }
}

// ============================================================================
// from_json_rpc() integration — error message gets cleaned up
// ============================================================================

TEST_CASE("from_json_rpc extracts friendly message from Klipper errors",
          "[moonraker][error]") {
    SECTION("Klipper homing error gets cleaned up") {
        json error_obj = {
            {"code", -32603},
            {"message",
             "{'error': 'WebRequestError', 'message': 'Must home axis first'}"}};

        auto err = MoonrakerError::from_json_rpc(error_obj, "printer.gcode.script");
        REQUIRE(err.message == "Must home axis first");
        REQUIRE(err.method == "printer.gcode.script");
        REQUIRE(err.code == -32603);
    }

    SECTION("plain message passes through unchanged") {
        json error_obj = {{"code", -32601}, {"message", "Method not found"}};

        auto err = MoonrakerError::from_json_rpc(error_obj, "some.method");
        REQUIRE(err.message == "Method not found");
    }
}
