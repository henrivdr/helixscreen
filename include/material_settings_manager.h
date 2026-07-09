// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_database.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>

namespace helix::printer {
struct EffectiveFilament;
}

namespace helix {

inline constexpr std::array<const char*, 4> DEFAULT_PRESET_MATERIALS{"PLA", "PETG", "ABS", "TPU"};

/**
 * @brief Manages user overrides for material temperature settings
 *
 * Loads/saves sparse overrides from settings.json under "material_overrides".
 * The filament::get_material_override() bridge function delegates to this manager,
 * so all callers of filament::find_material() transparently get customized values.
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class MaterialSettingsManager {
  public:
    static MaterialSettingsManager& instance();

    // Non-copyable
    MaterialSettingsManager(const MaterialSettingsManager&) = delete;
    MaterialSettingsManager& operator=(const MaterialSettingsManager&) = delete;

    /** @brief Load overrides from config (call at startup before any find_material) */
    void init();

    /** @brief Get override for a material (nullptr if none) */
    const filament::MaterialOverride* get_override(const std::string& name) const;

    /** @brief Set override for a material (saves to config) */
    void set_override(const std::string& name, const filament::MaterialOverride& override);

    /** @brief Remove override for a material (saves to config) */
    void clear_override(const std::string& name);

    /** @brief Check if a material has any overrides */
    bool has_override(const std::string& name) const;

    /** @brief Get all overrides (for UI list display) */
    const std::unordered_map<std::string, filament::MaterialOverride>& get_all_overrides() const {
        return overrides_;
    }

    /** @brief Get the 4 preset materials assigned to the quick-material buttons */
    std::array<std::string, 4> get_preset_materials() const {
        return preset_materials_;
    }

    /** @brief Assign a material name to a preset slot (0-3); saves to config */
    void set_preset_material(int index, const std::string& material);

    /** @brief Restore all preset slots to DEFAULT_PRESET_MATERIALS; saves to config */
    void reset_preset_materials();

    /** @brief Branded filament info attached to a quick-material preset slot */
    struct PresetFilament {
        std::string filament_id;
        std::string brand;
        std::string name;
        int nozzle = 0;
        int bed = 0;
        bool is_branded() const {
            return !filament_id.empty();
        }
    };

    /** @brief Get the branded filament attached to a preset slot (nullopt if generic) */
    std::optional<PresetFilament> get_preset_filament(int index) const;

    /** @brief Attach a branded filament to a preset slot (0-3); saves to config */
    void set_preset_filament(int index, const helix::printer::EffectiveFilament& ef);

    /** @brief Clear the branded filament from a preset slot (0-3); saves to config */
    void clear_preset_filament(int index);

  private:
    friend class TestAccess;
    MaterialSettingsManager() = default;
    ~MaterialSettingsManager() = default;

    void load_from_config();
    void save_to_config();
    void load_presets_from_config();
    void save_presets_to_config();
    /** @brief Reset all 4 preset slots (in memory only) to DEFAULT_PRESET_MATERIALS */
    void assign_defaults();

    std::unordered_map<std::string, filament::MaterialOverride> overrides_;
    std::array<std::string, 4> preset_materials_ = {"PLA", "PETG", "ABS", "TPU"};
    std::array<std::optional<PresetFilament>, 4> preset_filaments_{};
    bool initialized_ = false;
};

} // namespace helix
