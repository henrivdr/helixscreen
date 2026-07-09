// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix::printer {

/// Fully-resolved filament: product deltas merged over the base material type.
struct EffectiveFilament {
    std::string id, brand, name, type;
    int nozzle_min = 0;
    int nozzle_max = 0;
    int nozzle_recommended = 0;
    int bed_temp = 0;
    int chamber_temp_c = 0;
    int dry_temp_c = 0;
    int dry_time_min = 0;
    float density_g_cm3 = 0.0f;
    std::string compat_group;
    std::map<std::string, std::string> codes;  ///< scheme -> code
};

/// Transient, on-demand filament catalog. NO resident singleton: construct via a
/// static loader, query, then let it fall out of scope (frees all parsed data).
class FilamentCatalog {
  public:
    FilamentCatalog() = default;
    FilamentCatalog(const FilamentCatalog&) = delete;
    FilamentCatalog& operator=(const FilamentCatalog&) = delete;
    FilamentCatalog(FilamentCatalog&&) = default;
    FilamentCatalog& operator=(FilamentCatalog&&) = default;

    /// Whole catalog (built-in + user overlay). For the picker.
    static FilamentCatalog load_full();
    /// Only products carrying a code in `scheme`. For CFS decode (small slice).
    static FilamentCatalog load_codes(const std::string& scheme);
    /// Explicit path (tests / non-default locations).
    static FilamentCatalog load_from_file(const std::string& path, bool codes_only,
                                          const std::string& scheme);
    /// Explicit built-in + overlay paths; overlay overrides existing ids and adds new ones.
    static FilamentCatalog load_with_overlay(const std::string& builtin_path,
                                             const std::string& overlay_path);

    const EffectiveFilament* resolve_code(const std::string& scheme,
                                          const std::string& code) const;
    const EffectiveFilament* resolve_id(const std::string& id) const;
    std::vector<const EffectiveFilament*> products_for_type(const std::string& type) const;
    std::vector<const EffectiveFilament*> products_for_brand(const std::string& brand) const;
    std::vector<std::string> all_brands() const;
    std::vector<const EffectiveFilament*> all_products() const;
    std::vector<std::string> types_for_brand(const std::string& brand) const;
    std::vector<std::string> brands_for_type(const std::string& type) const;
    std::vector<const EffectiveFilament*> products_for(const std::string& brand,
                                                       const std::string& type) const;

  private:
    std::vector<EffectiveFilament> products_;
    std::unordered_map<std::string, size_t> by_id_;
    // scheme -> (code -> product index)
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> by_code_;
    void index();
};

}  // namespace helix::printer
