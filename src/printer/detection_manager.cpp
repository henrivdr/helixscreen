// SPDX-License-Identifier: GPL-3.0-or-later
#include "detection_manager.h"

#include "hv/json.hpp"
#include "moonraker_client.h"
#include "u1_stock_detection_source.h"

#include <spdlog/spdlog.h>

namespace helix::detection {

DetectionManager& DetectionManager::instance() {
    static DetectionManager s_instance;
    return s_instance;
}

void DetectionManager::init(helix::MoonrakerClient* client, helix::PrinterState* state) {
    client_ = client;
    state_  = state;
    probe_capabilities();
}

void DetectionManager::register_source(std::unique_ptr<DetectionSource> src) {
    if (!src) {
        return;
    }
    const std::string id = src->id();
    src->set_callback([this](const DetectionEvent& e) { on_event(e); });
    if (policies_.find(id) == policies_.end()) {
        policies_[id] = DetectionPolicy::DeferToSource;
    }
    sources_.push_back(std::move(src));
    spdlog::debug("DetectionManager: registered source '{}'", id);
}

void DetectionManager::set_policy(const std::string& source_id, DetectionPolicy p) {
    policies_[source_id] = p;
}

DetectionPolicy DetectionManager::policy(const std::string& source_id) const {
    auto it = policies_.find(source_id);
    if (it == policies_.end()) {
        return DetectionPolicy::DeferToSource;
    }
    return it->second;
}

bool DetectionManager::any_available() const {
    for (const auto& src : sources_) {
        if (src && src->available()) {
            return true;
        }
    }
    return false;
}

void DetectionManager::on_event(const DetectionEvent& e) {
    DetectionPolicy p = policy(e.source_id);
    if (p == DetectionPolicy::Off) {
        spdlog::debug("DetectionManager: event from '{}' suppressed (policy Off)", e.source_id);
        return;
    }
    spdlog::info("DetectionManager: detection from '{}' (kind={}, paused={})", e.source_id,
                 static_cast<int>(e.kind), e.already_paused);
    if (presenter_) {
        presenter_(e, p);
    }
}

void DetectionManager::probe_capabilities() {
    if (!client_) {
        return;
    }
    client_->send_jsonrpc(
        "printer.objects.list", json::object(),
        [this](const json& resp) {
            bool has = false;
            auto result_it = resp.find("result");
            if (result_it != resp.end() && result_it->is_object()) {
                auto objects_it = result_it->find("objects");
                if (objects_it != result_it->end() && objects_it->is_array()) {
                    for (const auto& obj : *objects_it) {
                        if (obj.is_string() && obj.get<std::string>() == "defect_detection") {
                            has = true;
                            break;
                        }
                    }
                }
            }
            spdlog::info("DetectionManager: defect_detection capability = {}", has);
            for (const auto& src : sources_) {
                if (src && src->id() == "u1_stock") {
                    if (auto* u1 = dynamic_cast<U1StockSource*>(src.get())) {
                        u1->set_capable(has);
                    }
                }
            }
        },
        [](const MoonrakerError& err) {
            spdlog::warn("DetectionManager: capability probe failed: {}", err.message);
        });
}

void DetectionManager::reset_for_test() {
    sources_.clear();
    policies_.clear();
    presenter_ = nullptr;
    client_    = nullptr;
    state_     = nullptr;
}

}  // namespace helix::detection
