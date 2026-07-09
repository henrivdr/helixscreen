// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"

#include "filament_database.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <set>

#include "hv/json.hpp"

namespace helix::printer {

namespace {

// Search paths for the built-in catalog (mirrors the old CFS loader).
const char* kBuiltinPaths[] = {"assets/filaments.json", "../assets/filaments.json",
                               "/opt/helixscreen/assets/filaments.json"};
const char* kUserPaths[] = {"config/user_filaments.json",
                            "../config/user_filaments.json"};

int get_int(const nlohmann::json& j, const char* key, int def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int>() : def;
}

/// Resolve one product JSON into an EffectiveFilament, inheriting from its type.
EffectiveFilament to_effective(const nlohmann::json& p) {
    EffectiveFilament e;
    e.id = p.value("id", "");
    e.brand = p.value("brand", "");
    e.name = p.value("name", "");
    e.type = p.value("type", "");

    auto base = filament::find_material(e.type);  // std::optional<MaterialInfo>
    const int type_min = base ? base->nozzle_min : 0;
    const int type_max = base ? base->nozzle_max : 0;

    e.nozzle_min = get_int(p, "nozzle_min", type_min);
    e.nozzle_max = get_int(p, "nozzle_max", type_max);
    e.nozzle_recommended = get_int(p, "nozzle", base ? base->nozzle_recommended() : 0);
    e.bed_temp = get_int(p, "bed", base ? base->bed_temp : 0);
    e.chamber_temp_c = base ? base->chamber_temp_c : 0;
    e.dry_temp_c = base ? base->dry_temp_c : 0;
    e.dry_time_min = base ? base->dry_time_min : 0;
    e.density_g_cm3 = p.contains("density") && p["density"].is_number()
                          ? p["density"].get<float>()
                          : (base ? base->density_g_cm3 : 0.0f);
    e.compat_group = base ? base->compat_group : "";

    if (auto it = p.find("codes"); it != p.end() && it->is_object()) {
        for (auto& [scheme, code] : it->items()) {
            if (code.is_string())
                e.codes[scheme] = code.get<std::string>();
        }
    }
    return e;
}

std::vector<nlohmann::json> read_products(const char* const* paths, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        std::ifstream f(paths[i]);
        if (!f.is_open())
            continue;
        try {
            auto doc = nlohmann::json::parse(f);
            if (doc.is_object() && doc.contains("filaments") && doc["filaments"].is_array())
                return doc["filaments"].get<std::vector<nlohmann::json>>();
            if (doc.is_array())  // user overlay is a bare array
                return doc.get<std::vector<nlohmann::json>>();
        } catch (const std::exception& e) {
            spdlog::warn("[filament] parse failed {}: {}", paths[i], e.what());
        }
    }
    return {};
}

}  // namespace

void FilamentCatalog::index() {
    by_id_.clear();
    by_code_.clear();
    for (size_t i = 0; i < products_.size(); ++i) {
        const auto& e = products_[i];
        by_id_[e.id] = i;
        for (const auto& [scheme, code] : e.codes)
            by_code_[scheme][code] = i;
    }
}

FilamentCatalog FilamentCatalog::load_from_file(const std::string& path, bool codes_only,
                                                const std::string& scheme) {
    const char* paths[] = {path.c_str()};
    FilamentCatalog cat;
    for (const auto& jp : read_products(paths, 1)) {
        auto e = to_effective(jp);
        if (codes_only && e.codes.find(scheme) == e.codes.end())
            continue;
        cat.products_.push_back(std::move(e));
    }
    cat.index();
    return cat;
}

FilamentCatalog FilamentCatalog::load_with_overlay(const std::string& builtin_path,
                                                   const std::string& overlay_path) {
    const char* bpaths[] = {builtin_path.c_str()};
    const char* opaths[] = {overlay_path.c_str()};

    // Raw product JSON keyed by id, so overlay can override before resolution.
    std::unordered_map<std::string, nlohmann::json> merged;
    std::vector<std::string> order;
    for (const auto& jp : read_products(bpaths, 1)) {
        std::string id = jp.value("id", "");
        if (merged.find(id) == merged.end())
            order.push_back(id);
        merged[id] = jp;
    }
    for (const auto& jp : read_products(opaths, 1)) {
        std::string id = jp.value("id", "");
        if (merged.find(id) == merged.end()) {
            order.push_back(id);
            merged[id] = jp;
        } else {
            merged[id].merge_patch(jp);  // field-level override
        }
    }
    FilamentCatalog cat;
    for (const auto& id : order)
        cat.products_.push_back(to_effective(merged[id]));
    cat.index();
    return cat;
}

FilamentCatalog FilamentCatalog::load_codes(const std::string& scheme) {
    FilamentCatalog cat;
    for (const auto& jp : read_products(kBuiltinPaths, std::size(kBuiltinPaths))) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    // User overlay may add coded products too.
    for (const auto& jp : read_products(kUserPaths, std::size(kUserPaths))) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    cat.index();
    return cat;
}

namespace {

/// First path in the list that exists on disk, or "" if none do.
std::string first_existing(const char* const* paths, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        std::ifstream f(paths[i]);
        if (f.is_open())
            return paths[i];
    }
    return "";
}

}  // namespace

FilamentCatalog FilamentCatalog::load_full() {
    return load_with_overlay(first_existing(kBuiltinPaths, std::size(kBuiltinPaths)),
                             first_existing(kUserPaths, std::size(kUserPaths)));
}

const EffectiveFilament* FilamentCatalog::resolve_code(const std::string& scheme,
                                                       const std::string& code) const {
    auto s = by_code_.find(scheme);
    if (s == by_code_.end())
        return nullptr;
    auto c = s->second.find(code);
    return c == s->second.end() ? nullptr : &products_[c->second];
}

const EffectiveFilament* FilamentCatalog::resolve_id(const std::string& id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &products_[it->second];
}

std::vector<const EffectiveFilament*>
FilamentCatalog::products_for_type(const std::string& type) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        if (e.type == type)
            out.push_back(&e);
    return out;
}

std::vector<const EffectiveFilament*>
FilamentCatalog::products_for_brand(const std::string& brand) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        if (e.brand == brand)
            out.push_back(&e);
    return out;
}

std::vector<std::string> FilamentCatalog::all_brands() const {
    std::set<std::string> seen;
    for (const auto& p : products_) seen.insert(p.brand);
    return {seen.begin(), seen.end()};  // sorted + deduped
}

std::vector<const EffectiveFilament*> FilamentCatalog::all_products() const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        out.push_back(&e);
    return out;
}

std::vector<std::string> FilamentCatalog::types_for_brand(const std::string& brand) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& p : products_) {
        if (p.brand == brand && seen.insert(p.type).second) {
            out.push_back(p.type);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> FilamentCatalog::brands_for_type(const std::string& type) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& p : products_) {
        if (p.type == type && seen.insert(p.brand).second) {
            out.push_back(p.brand);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<const EffectiveFilament*> FilamentCatalog::products_for(
    const std::string& brand, const std::string& type) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& p : products_) {
        if (p.brand == brand && p.type == type) {
            out.push_back(&p);
        }
    }
    return out;
}

}  // namespace helix::printer
