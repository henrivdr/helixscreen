// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_usb_source.h"

#include "ui_panel_print_select.h" // For PrintFileData
#include "ui_print_select_card_view.h"

#include "gcode_parser.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "print_file_data.h"
#include "subject_debug_registry.h"
#include "thumbnail_cache.h"
#include "usb_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Subject for source tab state: 0 = Printer (default), 1 = USB
static lv_subject_t s_print_source_is_usb;
static bool s_source_subject_initialized = false;

void PrintSelectUsbSource::init_subjects() {
    if (s_source_subject_initialized)
        return;
    lv_subject_init_int(&s_print_source_is_usb, 0);
    lv_xml_register_subject(nullptr, "print_source_is_usb", &s_print_source_is_usb);
    SubjectDebugRegistry::instance().register_subject(&s_print_source_is_usb, "print_source_is_usb",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);
    s_source_subject_initialized = true;
    spdlog::debug("[UsbSource] Subject print_source_is_usb initialized");
}

// ============================================================================
// Setup
// ============================================================================

bool PrintSelectUsbSource::setup(lv_obj_t* panel) {
    if (!panel) {
        return false;
    }

    // Find the source selector container
    source_selector_ = lv_obj_find_by_name(panel, "source_selector");
    if (!source_selector_) {
        spdlog::warn("[UsbSource] Source selector container not found");
        return false;
    }

    // Hide selector by default - only show when USB drive is present
    lv_obj_add_flag(source_selector_, LV_OBJ_FLAG_HIDDEN);

    // Set initial state - Printer is selected by default
    update_button_states();

    spdlog::debug("[UsbSource] Source selector configured (hidden until USB drive inserted)");
    return true;
}

void PrintSelectUsbSource::set_usb_manager(UsbManager* manager) {
    usb_manager_ = manager;

    // If USB source is currently active, refresh the file list
    if (current_source_ == FileSource::USB && usb_manager_) {
        refresh_files();
    }

    // Check if drive is already inserted (startup race: drive detected before panel exists)
    if (manager && !manager->get_drives().empty() && !moonraker_has_usb_access_ &&
        source_selector_) {
        spdlog::info("[UsbSource] USB drive already present at setup - showing source selector");
        lv_obj_remove_flag(source_selector_, LV_OBJ_FLAG_HIDDEN);
    }

    spdlog::debug("[UsbSource] UsbManager set");
}

// ============================================================================
// Source Selection
// ============================================================================

void PrintSelectUsbSource::select_printer_source() {
    if (current_source_ == FileSource::PRINTER) {
        return; // Already on Printer source
    }

    spdlog::debug("[UsbSource] Switching to Printer source");
    current_source_ = FileSource::PRINTER;
    update_button_states();

    if (on_source_changed_) {
        on_source_changed_(FileSource::PRINTER);
    }
}

void PrintSelectUsbSource::select_usb_source() {
    if (current_source_ == FileSource::USB) {
        return; // Already on USB source
    }

    spdlog::debug("[UsbSource] Switching to USB source");
    current_source_ = FileSource::USB;
    update_button_states();

    if (on_source_changed_) {
        on_source_changed_(FileSource::USB);
    }

    // Refresh USB files
    refresh_files();
}

// ============================================================================
// USB Drive Events
// ============================================================================

void PrintSelectUsbSource::on_drive_inserted() {
    if (!source_selector_) {
        return;
    }

    // If Moonraker has symlink access to USB files, don't show the source selector
    if (moonraker_has_usb_access_) {
        spdlog::debug("[UsbSource] USB drive inserted - but Moonraker has symlink access, keeping "
                      "source selector hidden");
        return;
    }

    spdlog::debug("[UsbSource] USB drive inserted - showing source selector");
    lv_obj_remove_flag(source_selector_, LV_OBJ_FLAG_HIDDEN);
}

void PrintSelectUsbSource::set_moonraker_has_usb_access(bool has_access) {
    moonraker_has_usb_access_ = has_access;

    if (has_access && source_selector_) {
        // Hide source selector permanently - files are accessible via Printer source
        spdlog::debug(
            "[UsbSource] Moonraker has USB symlink access - hiding source selector permanently");
        lv_obj_add_flag(source_selector_, LV_OBJ_FLAG_HIDDEN);

        // If currently viewing USB source, switch to Printer
        if (current_source_ == FileSource::USB) {
            current_source_ = FileSource::PRINTER;
            update_button_states();
            if (on_source_changed_) {
                on_source_changed_(FileSource::PRINTER);
            }
        }
    }
}

void PrintSelectUsbSource::on_drive_removed() {
    spdlog::info("[UsbSource] USB drive removed - hiding source selector");

    // Hide source selector container
    if (source_selector_) {
        lv_obj_add_flag(source_selector_, LV_OBJ_FLAG_HIDDEN);
    }

    // If USB source is currently active, switch to Printer source
    if (current_source_ == FileSource::USB) {
        spdlog::debug("[UsbSource] Was viewing USB source - switching to Printer");

        // Clear USB files
        usb_files_.clear();

        // Switch to Printer source
        current_source_ = FileSource::PRINTER;
        update_button_states();

        if (on_source_changed_) {
            on_source_changed_(FileSource::PRINTER);
        }
    }
}

// ============================================================================
// File Operations
// ============================================================================

void PrintSelectUsbSource::refresh_files() {
    usb_files_.clear();

    if (!usb_manager_) {
        spdlog::warn("[UsbSource] UsbManager not available");
        if (on_files_ready_) {
            on_files_ready_(std::vector<PrintFileData>{});
        }
        return;
    }

    // Get connected USB drives
    auto drives = usb_manager_->get_drives();
    if (drives.empty()) {
        spdlog::debug("[UsbSource] No USB drives detected");
        if (on_files_ready_) {
            on_files_ready_(std::vector<PrintFileData>{});
        }
        return;
    }

    // Scan first drive for G-code files
    // TODO: If multiple drives, show a drive selector
    usb_files_ = usb_manager_->scan_for_gcode(drives[0].mount_path);

    spdlog::info("[UsbSource] Found {} G-code files on USB drive '{}'", usb_files_.size(),
                 drives[0].label);

    if (on_files_ready_) {
        on_files_ready_(convert_to_print_file_data());
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void PrintSelectUsbSource::update_button_states() {
    // Update subject — XML bind_flag_if_not_eq handles button visibility/appearance
    if (s_source_subject_initialized) {
        lv_subject_set_int(&s_print_source_is_usb, current_source_ == FileSource::USB ? 1 : 0);
    }
}

std::vector<PrintFileData> PrintSelectUsbSource::convert_to_print_file_data() const {
    std::vector<PrintFileData> result;
    result.reserve(usb_files_.size());

    const std::string default_thumbnail = PrintSelectCardView::get_default_thumbnail();
    for (const auto& usb_file : usb_files_) {
        auto file_data = PrintFileData::from_usb_file(usb_file, default_thumbnail);

        auto best = helix::gcode::get_best_thumbnail(usb_file.path);
        if (!best.png_data.empty()) {
            auto& cache = get_thumbnail_cache();
            std::string cache_path = cache.save_raw_png("usb:" + usb_file.filename, best.png_data);
            if (!cache_path.empty()) {
                file_data.thumbnail_path = cache_path;
            }
        }

        result.push_back(std::move(file_data));
    }

    return result;
}

} // namespace helix::ui
