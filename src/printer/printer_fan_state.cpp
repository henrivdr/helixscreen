// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_fan_state.cpp
 * @brief Fan state management extracted from PrinterState
 *
 * Manages fan subjects including main part-cooling fan speed, multi-fan
 * tracking with per-fan subjects, and fan metadata for UI display.
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_fan_state.h"

#include "config.h"
#include "device_display_name.h"
#include "hardware_role_registry.h"
#include "state/subject_macros.h"
#include "unit_conversions.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

FanRoleConfig FanRoleConfig::from_config(Config* config,
                                         const std::vector<std::string>& discovered_fans) {
    FanRoleConfig roles;
    if (!config) {
        return roles;
    }
    // Resolve all four fan roles without saving (persist=false), then write once if any changed.
    // This avoids up to 4 independent disk writes on first-heal.
    bool any_changed = false;
    auto resolve_one = [&](HardwareRoleId id, std::string& field) {
        const auto* desc = role_descriptor(id);
        if (!desc)
            return;
        const std::string key = config->df() + desc->config_key;
        const std::string dflt =
            desc->canonical_default ? std::string(desc->canonical_default) : std::string();
        const std::string saved = config->get<std::string>(key, dflt);
        std::string resolved = resolve_role_from_config(id, config, discovered_fans, false);
        field = resolved;
        if (!resolved.empty() && resolved != saved) {
            config->set<std::string>(key, resolved);
            any_changed = true;
        }
    };
    resolve_one(HardwareRoleId::PartFan, roles.part_fan);
    resolve_one(HardwareRoleId::HotendFan, roles.hotend_fan);
    resolve_one(HardwareRoleId::ChamberFan, roles.chamber_fan);
    resolve_one(HardwareRoleId::ExhaustFan, roles.exhaust_fan);
    if (any_changed) {
        if (!config->save()) {
            spdlog::warn("[FanRoleConfig] Failed to persist batched fan role heals");
        }
    }
    return roles;
}

void PrinterFanState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterFanState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterFanState] Initializing subjects (register_xml={})", register_xml);

    // Fan subjects
    INIT_SUBJECT_INT(fan_speed, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(fans_version, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterFanState] Subjects initialized successfully");
}

void PrinterFanState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterFanState] Deinitializing subjects");

    // Signal subject death FIRST — sets bool to false so ALL ObserverGuards detect
    // dead subjects, then clear the map to release shared_ptr references. (#816)
    for (auto& [name, lifetime] : fan_speed_lifetimes_) {
        if (lifetime)
            *lifetime = false;
    }
    fan_speed_lifetimes_.clear();

    // Now safe to deinit subjects (lv_subject_deinit frees attached observers)
    for (auto& [name, subject_ptr] : fan_speed_subjects_) {
        if (subject_ptr) {
            lv_subject_deinit(subject_ptr.get());
        }
    }
    fan_speed_subjects_.clear();

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterFanState::update_from_status(const nlohmann::json& status) {
    // Update main part-cooling fan speed
    if (status.contains("fan")) {
        const auto& fan = status["fan"];
        spdlog::trace("[PrinterFanState] Received fan status update: {}", fan.dump());

        if (fan.contains("speed") && fan["speed"].is_number()) {
            int speed_pct = units::json_to_percent(fan, "speed");
            spdlog::trace("[PrinterFanState] Fan speed update: {}%", speed_pct);
            if (lv_subject_get_int(&fan_speed_) != speed_pct) {
                lv_subject_set_int(&fan_speed_, speed_pct);
            }

            // Also update multi-fan tracking
            double speed = fan["speed"].get<double>();
            update_fan_speed("fan", speed);
        }
    }

    // Check for other fan types in the status update
    // Moonraker sends fan objects as top-level keys: "heater_fan hotend_fan", "fan_generic xyz"
    for (const auto& [key, value] : status.items()) {
        // Skip non-fan objects
        if (key.rfind("heater_fan ", 0) == 0 || key.rfind("fan_generic ", 0) == 0 ||
            key.rfind("controller_fan ", 0) == 0 || key.rfind("temperature_fan ", 0) == 0) {
            if (value.is_object() && value.contains("speed") && value["speed"].is_number()) {
                double speed = value["speed"].get<double>();
                update_fan_speed(key, speed);

                // If this is the configured part fan, also update the main fan_speed_ subject
                // so the hero slider tracks the actual part fan speed
                if (!roles_.part_fan.empty() && key == roles_.part_fan) {
                    int speed_pct = units::json_to_percent(value, "speed");
                    if (lv_subject_get_int(&fan_speed_) != speed_pct) {
                        lv_subject_set_int(&fan_speed_, speed_pct);
                    }
                }
            }
        }

        // Handle output_pin fan objects (Creality-style)
        // These report {"value": 0.0-1.0} instead of {"speed": 0.0-1.0}
        if (key.rfind("output_pin ", 0) == 0) {
            if (value.is_object() && value.contains("value") && value["value"].is_number()) {
                double speed = value["value"].get<double>();
                update_fan_speed(key, speed);

                // If this is the configured part fan, also update the main fan_speed_ subject
                if (!roles_.part_fan.empty() && key == roles_.part_fan) {
                    int speed_pct = units::to_percent(speed);
                    if (lv_subject_get_int(&fan_speed_) != speed_pct) {
                        lv_subject_set_int(&fan_speed_, speed_pct);
                    }
                }
            }
        }
    }

    // Parse fan_feedback RPM data (Creality-specific tachometer module)
    if (status.contains("fan_feedback")) {
        const auto& fb = status["fan_feedback"];
        if (fb.is_object()) {
            for (int i = 0; i < 10; i++) {
                std::string key = "fan" + std::to_string(i) + "_speed";
                if (fb.contains(key) && fb[key].is_number()) {
                    int rpm = fb[key].get<int>();
                    update_fan_rpm("output_pin fan" + std::to_string(i), rpm);
                }
            }
        }
    }
}

FanType PrinterFanState::classify_fan_type(const std::string& object_name) const {
    if (object_name == "fan") {
        return FanType::PART_COOLING;
    }
    // Check if this fan is the wizard-configured part cooling fan
    if (!roles_.part_fan.empty() && object_name == roles_.part_fan) {
        return FanType::PART_COOLING;
    }
    if (object_name.rfind("heater_fan ", 0) == 0) {
        return FanType::HEATER_FAN;
    } else if (object_name.rfind("controller_fan ", 0) == 0) {
        return FanType::CONTROLLER_FAN;
    } else if (object_name.rfind("temperature_fan ", 0) == 0) {
        return FanType::TEMPERATURE_FAN;
    } else if (object_name.rfind("output_pin ", 0) == 0) {
        // Check if the short name starts with "fan" (e.g., "output_pin fan0")
        std::string short_name = object_name.substr(11);
        if (short_name.rfind("fan", 0) == 0) {
            return FanType::OUTPUT_PIN_FAN;
        }
        return FanType::GENERIC_FAN;
    } else {
        return FanType::GENERIC_FAN;
    }
}

PrimaryFans PrinterFanState::classify_primary_fans() const {
    PrimaryFans out;
    for (const auto& fan : fans_) {
        switch (fan.type) {
        case FanType::PART_COOLING:
            if (out.part.empty())
                out.part = fan.object_name;
            break;
        case FanType::HEATER_FAN:
            if (out.hotend.empty())
                out.hotend = fan.object_name;
            break;
        case FanType::CONTROLLER_FAN:
        case FanType::TEMPERATURE_FAN:
        case FanType::GENERIC_FAN:
        case FanType::OUTPUT_PIN_FAN:
            if (out.aux.empty())
                out.aux = fan.object_name;
            break;
        }
    }
    return out;
}

std::string PrinterFanState::get_role_display_name(const std::string& object_name) const {
    auto it = role_display_names_.find(object_name);
    if (it != role_display_names_.end()) {
        return it->second;
    }
    return {};
}

std::string PrinterFanState::disambiguate_chamber_fan_name(const std::string& object_name,
                                                           FanType type,
                                                           const std::string& base_name) const {
    // Some printers (e.g. Creality K2 Plus) expose two distinct Klipper objects
    // that share the "chamber_fan" suffix: the PTC heater element's cooling fan
    // (heater_fan chamber_fan) and the chamber cooling fan (temperature_fan
    // chamber_fan). Both otherwise resolve to a flat "Chamber Fan", which is
    // ambiguous. Refine the label by role so each reads distinctly.
    if (extract_device_suffix(object_name) != "chamber_fan") {
        return base_name;
    }
    switch (type) {
    case FanType::HEATER_FAN:
        return "Chamber Heater Fan";
    case FanType::TEMPERATURE_FAN:
        return "Chamber Cooling Fan";
    default:
        return base_name;
    }
}

bool PrinterFanState::is_fan_controllable(FanType type) {
    return type == FanType::PART_COOLING || type == FanType::GENERIC_FAN ||
           type == FanType::OUTPUT_PIN_FAN;
}

void PrinterFanState::init_fans(const std::vector<std::string>& fan_objects,
                                const FanRoleConfig& roles) {
    // Build new subject map, reusing existing subjects for fans that persist
    // across reconnections. Only deinit subjects for fans that disappeared.
    std::unordered_map<std::string, std::unique_ptr<lv_subject_t>> new_subjects;
    std::unordered_map<std::string, SubjectLifetime> new_lifetimes;
    new_subjects.reserve(fan_objects.size());
    new_lifetimes.reserve(fan_objects.size());

    // Store configured fan roles for classification and naming
    roles_ = roles;
    role_display_names_.clear();

    // Build role-based display name overrides for configured fans.
    // Configured fans use their role name; unconfigured fans use auto-generated names.
    if (!roles_.part_fan.empty() && roles_.part_fan != "fan") {
        role_display_names_[roles_.part_fan] = "Part Fan";
    }
    if (!roles_.hotend_fan.empty()) {
        role_display_names_[roles_.hotend_fan] = "Hotend Fan";
    }
    if (!roles_.chamber_fan.empty()) {
        role_display_names_[roles_.chamber_fan] = "Chamber Fan";
    }
    if (!roles_.exhaust_fan.empty()) {
        role_display_names_[roles_.exhaust_fan] = "Exhaust Fan";
    }

    spdlog::trace("[PrinterFanState] Fan role config: part='{}' hotend='{}' chamber='{}' "
                  "exhaust='{}' ({} display overrides)",
                  roles_.part_fan, roles_.hotend_fan, roles_.chamber_fan, roles_.exhaust_fan,
                  role_display_names_.size());

    fans_.clear();
    fans_.reserve(fan_objects.size());

    for (const auto& obj_name : fan_objects) {
        // Skip bare "fan" only when a DIFFERENT, LIVE fan is configured as part
        // cooling — some printers expose an empty "fan" object that never reports
        // speed data. Never skip the real [fan] just because a stale role names an
        // absent object.
        if (obj_name == "fan" && !roles_.part_fan.empty() && roles_.part_fan != "fan" &&
            std::find(fan_objects.begin(), fan_objects.end(), roles_.part_fan) !=
                fan_objects.end()) {
            spdlog::debug("[PrinterFanState] Skipping bare 'fan' — part fan is '{}'",
                          roles_.part_fan);
            continue;
        }

        FanInfo info;
        info.object_name = obj_name;
        info.type = classify_fan_type(obj_name);
        info.is_controllable = is_fan_controllable(info.type);
        info.speed_percent = 0;

        // Name priority: custom name > role name > auto-generated
        auto* config = Config::get_instance();
        std::string custom_name;
        if (config) {
            custom_name = config->get<std::string>(config->df() + "fans/names/" + obj_name, "");
        }
        if (!custom_name.empty()) {
            info.display_name = custom_name;
        } else {
            std::string role_name = get_role_display_name(obj_name);
            info.display_name =
                role_name.empty() ? get_display_name(obj_name, DeviceType::FAN) : role_name;
            info.display_name =
                disambiguate_chamber_fan_name(obj_name, info.type, info.display_name);
        }

        spdlog::trace("[PrinterFanState] Registered fan: {} -> \"{}\" (type={}, controllable={})",
                      obj_name, info.display_name, static_cast<int>(info.type),
                      info.is_controllable);
        fans_.push_back(std::move(info));

        // Reuse existing subject if this fan was already tracked, otherwise create new
        auto existing = fan_speed_subjects_.find(obj_name);
        if (existing != fan_speed_subjects_.end() && existing->second) {
            // Reuse — reset value but keep subject alive (observers remain valid)
            lv_subject_set_int(existing->second.get(), 0);
            new_subjects.emplace(obj_name, std::move(existing->second));
            // Reuse existing lifetime token too (observers still hold valid weak_ptrs)
            auto lifetime_it = fan_speed_lifetimes_.find(obj_name);
            if (lifetime_it != fan_speed_lifetimes_.end()) {
                new_lifetimes.emplace(obj_name, std::move(lifetime_it->second));
            } else {
                new_lifetimes.emplace(obj_name, std::make_shared<bool>(true));
            }
            spdlog::trace("[PrinterFanState] Reused speed subject for fan: {}", obj_name);
        } else {
            auto subject_ptr = std::make_unique<lv_subject_t>();
            lv_subject_init_int(subject_ptr.get(), 0);
            new_subjects.emplace(obj_name, std::move(subject_ptr));
            new_lifetimes.emplace(obj_name, std::make_shared<bool>(true));
            spdlog::trace("[PrinterFanState] Created speed subject for fan: {}", obj_name);
        }
    }

    // Signal subject death for orphaned fans — sets bool to false so ALL
    // ObserverGuards detect the dead subject, even if other services still hold
    // shared_ptr copies. Then reset to release our reference. (#816)
    for (auto& [name, lifetime] : fan_speed_lifetimes_) {
        if (new_lifetimes.find(name) == new_lifetimes.end()) {
            spdlog::trace("[PrinterFanState] Expiring lifetime token for orphaned fan: {}", name);
            if (lifetime)
                *lifetime = false;
            lifetime.reset();
        }
    }

    // Now safe to deinit orphaned subjects (observers already invalidated above)
    for (auto& [name, subject_ptr] : fan_speed_subjects_) {
        if (subject_ptr) {
            spdlog::trace("[PrinterFanState] Deiniting orphaned speed subject for fan: {}", name);
            lv_subject_deinit(subject_ptr.get());
        }
    }
    fan_speed_subjects_ = std::move(new_subjects);
    fan_speed_lifetimes_ = std::move(new_lifetimes);

    // Initialize and bump version to notify UI
    lv_subject_set_int(&fans_version_, lv_subject_get_int(&fans_version_) + 1);
    spdlog::debug("[PrinterFanState] Initialized {} fans with {} speed subjects (version {})",
                  fans_.size(), fan_speed_subjects_.size(), lv_subject_get_int(&fans_version_));
}

void PrinterFanState::update_fan_speed(const std::string& object_name, double speed) {
    int speed_pct = units::to_percent(speed);

    for (auto& fan : fans_) {
        if (fan.object_name == object_name) {
            if (fan.speed_percent != speed_pct) {
                fan.speed_percent = speed_pct;

                // Fire per-fan subject for reactive UI updates
                auto it = fan_speed_subjects_.find(object_name);
                if (it != fan_speed_subjects_.end() && it->second) {
                    lv_subject_set_int(it->second.get(), speed_pct);
                    spdlog::trace("[PrinterFanState] Fan {} speed updated to {}%", object_name,
                                  speed_pct);
                } else {
                    spdlog::debug("[PrinterFanState] Dropping speed update for '{}' — subject "
                                  "not initialized",
                                  object_name);
                }
            }
            return;
        }
    }
    // Fan not in list - this is normal during initial status before discovery
}

void PrinterFanState::update_fan_rpm(const std::string& object_name, int rpm) {
    for (auto& fan : fans_) {
        if (fan.object_name == object_name) {
            fan.rpm = rpm;
            return;
        }
    }
}

lv_subject_t* PrinterFanState::get_fan_speed_subject(const std::string& object_name,
                                                     SubjectLifetime& lifetime) {
    auto it = fan_speed_subjects_.find(object_name);
    if (it != fan_speed_subjects_.end() && it->second) {
        auto lt = fan_speed_lifetimes_.find(object_name);
        if (lt != fan_speed_lifetimes_.end()) {
            lifetime = lt->second;
        }
        return it->second.get();
    }
    lifetime.reset();
    return nullptr;
}

lv_subject_t* PrinterFanState::get_fan_speed_subject(const std::string& object_name) {
    auto it = fan_speed_subjects_.find(object_name);
    if (it != fan_speed_subjects_.end() && it->second) {
        return it->second.get();
    }
    return nullptr;
}

void PrinterFanState::rename_fan(const std::string& object_name, const std::string& new_name) {
    // Save to config
    auto* config = Config::get_instance();
    if (config) {
        std::string key = config->df() + "fans/names/" + object_name;
        config->set<std::string>(key, new_name);

        config->save();
    }

    // Update in-memory display name
    for (auto& fan : fans_) {
        if (fan.object_name == object_name) {
            if (new_name.empty()) {
                // Revert to role name or auto-generated name
                std::string role_name = get_role_display_name(object_name);
                fan.display_name =
                    role_name.empty() ? get_display_name(object_name, DeviceType::FAN) : role_name;
                fan.display_name =
                    disambiguate_chamber_fan_name(object_name, fan.type, fan.display_name);
            } else {
                fan.display_name = new_name;
            }
            spdlog::info("[PrinterFanState] Renamed '{}' -> '{}'", object_name, fan.display_name);
            break;
        }
    }

    // Bump version so UI rebuilds
    lv_subject_set_int(&fans_version_, lv_subject_get_int(&fans_version_) + 1);
}

} // namespace helix
