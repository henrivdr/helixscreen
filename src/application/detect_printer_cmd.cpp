// SPDX-License-Identifier: GPL-3.0-or-later
#include "detect_printer_cmd.h"

#include "hv/requests.h"
#include "printer_detector.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <cstdio>

#include "hv/json.hpp"

namespace helix::detect {

std::string format_detect_verdict(const PrinterDetectionResult& result,
                                  const std::string& runner_up_preset) {
    nlohmann::json j;
    j["model"] = result.type_name;
    j["preset"] = result.preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.preset);
    j["confidence"] = result.confidence;
    j["runner_up_preset"] =
        runner_up_preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(runner_up_preset);
    j["runner_up_confidence"] = result.runner_up_confidence;
    return j.dump();
}

} // namespace helix::detect

namespace {

nlohmann::json http_get_json(const std::string& url, int timeout_sec) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = timeout_sec;
    auto resp = requests::request(req);
    if (!resp || resp->status_code != 200)
        return nullptr;
    try {
        return nlohmann::json::parse(resp->body);
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace

namespace helix::detect {

int run_detect_printer(const std::string& host, int port) {
    const std::string base = "http://" + host + ":" + std::to_string(port);
    const int t = 3;

    nlohmann::json objs = http_get_json(base + "/printer/objects/list", t);
    if (!objs.is_object() || !objs.contains("result") || !objs["result"].contains("objects") ||
        !objs["result"]["objects"].is_array()) {
        spdlog::warn("[detect] Moonraker object list unavailable at {}", base);
        return 1;
    }

    helix::PrinterDiscovery disc;
    disc.parse_objects(objs["result"]["objects"]);

    if (auto info = http_get_json(base + "/printer/info", t);
        info.is_object() && info.contains("result")) {
        disc.set_hostname(info["result"].value("hostname", ""));
    }

    if (auto th = http_get_json(base + "/printer/objects/query?toolhead=kinematics", t);
        th.is_object() && th.contains("result")) {
        const auto& s = th["result"]["status"];
        if (s.contains("toolhead") && s["toolhead"].contains("kinematics")) {
            disc.set_kinematics(s["toolhead"]["kinematics"].get<std::string>());
        }
    }

    if (auto cfg = http_get_json(base + "/printer/objects/query?configfile=settings", t);
        cfg.is_object() && cfg.contains("result")) {
        const auto& st = cfg["result"]["status"];
        if (st.contains("configfile") && st["configfile"].contains("settings")) {
            const auto& s = st["configfile"]["settings"];
            BuildVolume bv{};
            auto rd = [&](const char* k, const char* f, float& out) {
                if (s.contains(k) && s[k].contains(f) && s[k][f].is_number())
                    out = s[k][f].get<float>();
            };
            rd("stepper_x", "position_min", bv.x_min);
            rd("stepper_x", "position_max", bv.x_max);
            rd("stepper_y", "position_min", bv.y_min);
            rd("stepper_y", "position_max", bv.y_max);
            rd("stepper_z", "position_max", bv.z_max);
            disc.set_build_volume(bv);
        }
    }

    PrinterDetectionResult result = PrinterDetector::auto_detect(disc);
    std::string runner_up_preset = PrinterDetector::get_preset_for_name(result.runner_up_type_name);
    printf("%s\n", format_detect_verdict(result, runner_up_preset).c_str());
    return 0;
}

} // namespace helix::detect
