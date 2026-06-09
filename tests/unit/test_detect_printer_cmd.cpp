// SPDX-License-Identifier: GPL-3.0-or-later
#include "detect_printer_cmd.h"
#include "printer_detector.h"

#include "catch_amalgamated.hpp"

TEST_CASE("format_detect_verdict: confident match with runner-up", "[detect_cmd]") {
    PrinterDetectionResult r{"Qidi Q2", 90, "chamber"};
    r.preset = "qidi_q2";
    r.runner_up_type_name = "Qidi Q1 Pro";
    r.runner_up_confidence = 70;
    std::string json = helix::detect::format_detect_verdict(r, "qidi_q1_pro");
    REQUIRE(json.find("\"model\":\"Qidi Q2\"") != std::string::npos);
    REQUIRE(json.find("\"preset\":\"qidi_q2\"") != std::string::npos);
    REQUIRE(json.find("\"confidence\":90") != std::string::npos);
    REQUIRE(json.find("\"runner_up_preset\":\"qidi_q1_pro\"") != std::string::npos);
    REQUIRE(json.find("\"runner_up_confidence\":70") != std::string::npos);
}

TEST_CASE("format_detect_verdict: no preset and no runner-up emit null", "[detect_cmd]") {
    PrinterDetectionResult r{"Mystery Printer", 40, "corexy"};
    std::string json = helix::detect::format_detect_verdict(r, "");
    REQUIRE(json.find("\"preset\":null") != std::string::npos);
    REQUIRE(json.find("\"runner_up_preset\":null") != std::string::npos);
    REQUIRE(json.find("\"runner_up_confidence\":0") != std::string::npos);
}
