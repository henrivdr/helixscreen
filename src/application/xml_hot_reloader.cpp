// SPDX-License-Identifier: GPL-3.0-or-later

#include "xml_hot_reloader.h"

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <lvgl.h>

extern "C" {
#include "helix-xml/src/xml/lv_xml_component_private.h"
}

namespace fs = std::filesystem;

static size_t count_xml_subjects(const char* component_name) {
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(component_name);
    if (!scope)
        return 0;
    size_t n = 0;
    // LV_LL_READ is a C macro that relies on implicit void* conversion — expand it manually
    // for C++ so we can cast explicitly.
    for (void* s = lv_ll_get_head(&scope->subjects_ll); s != nullptr;
         s = lv_ll_get_next(&scope->subjects_ll, s)) {
        ++n;
    }
    return n;
}

namespace helix {

XmlHotReloader::~XmlHotReloader() {
    stop();
}

void XmlHotReloader::start(const std::vector<std::string>& xml_dirs, int poll_interval_ms) {
    if (running_.load()) {
        return;
    }

    poll_interval_ms_ = poll_interval_ms;
    initial_scan(xml_dirs);

    spdlog::info("[HotReload] Watching {} XML files across {} directories (poll every {}ms)",
                 file_mtimes_.size(), xml_dirs.size(), poll_interval_ms_);

    running_.store(true);
    poll_thread_ = std::thread(&XmlHotReloader::poll_loop, this);
}

void XmlHotReloader::stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    spdlog::debug("[HotReload] Stopped");
}

void XmlHotReloader::initial_scan(const std::vector<std::string>& xml_dirs) {
    file_mtimes_.clear();
    file_to_lvgl_path_.clear();

    static constexpr const char* SKIP_DIRS[] = {"translations", ".claude-recall"};
    auto is_skipped = [](const fs::path& p) {
        auto name = p.filename().string();
        for (const auto* skip : SKIP_DIRS) {
            if (name == skip)
                return true;
        }
        return false;
    };

    std::unordered_map<std::string, size_t> per_dir_counts;

    for (const auto& dir : xml_dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            spdlog::warn("[HotReload] Directory not found: {}", dir);
            continue;
        }

        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end) {
            const auto& entry = *it;
            std::error_code dir_ec;
            if (entry.is_directory(dir_ec) && is_skipped(entry.path())) {
                it.disable_recursion_pending();
                std::error_code inc_ec;
                it.increment(inc_ec);
                if (inc_ec) {
                    spdlog::warn("[HotReload] Iterator error in {}: {}", dir, inc_ec.message());
                    break;
                }
                continue;
            }
            std::error_code file_ec;
            if (entry.is_regular_file(file_ec) && entry.path().extension() == ".xml") {
                std::error_code mtime_ec;
                auto mtime = fs::last_write_time(entry.path(), mtime_ec);
                if (mtime_ec) {
                    spdlog::warn("[HotReload] Could not stat {}: {}", entry.path().string(),
                                 mtime_ec.message());
                    std::error_code inc_ec;
                    it.increment(inc_ec);
                    if (inc_ec) {
                        spdlog::warn("[HotReload] Iterator error in {}: {}", dir, inc_ec.message());
                        break;
                    }
                    continue;
                }

                auto abs_path = fs::absolute(entry.path()).string();
                file_mtimes_[abs_path] = mtime;

                auto rel_path = entry.path().string();
                file_to_lvgl_path_[abs_path] = "A:" + rel_path;

                auto parent = entry.path().parent_path().filename().string();
                per_dir_counts[parent.empty() ? dir : parent]++;

                spdlog::trace("[HotReload] Tracking: {} ({})", rel_path,
                              component_name_from_path(entry.path()));
            }
            std::error_code inc_ec;
            it.increment(inc_ec);
            if (inc_ec) {
                spdlog::warn("[HotReload] Iterator error in {}: {}", dir, inc_ec.message());
                break;
            }
        }
    }

    std::string breakdown;
    for (const auto& [bucket, n] : per_dir_counts) {
        if (!breakdown.empty())
            breakdown += ", ";
        breakdown += bucket + ": " + std::to_string(n);
    }
    spdlog::info("[HotReload] Scan complete ({} files) [{}]", file_mtimes_.size(), breakdown);
}

void XmlHotReloader::poll_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms_));
        if (!running_.load())
            break;
        scan_and_reload();
    }
}

void XmlHotReloader::scan_and_reload() {
    for (auto& [abs_path, cached_mtime] : file_mtimes_) {
        std::error_code ec;
        auto current_mtime = fs::last_write_time(abs_path, ec);
        if (ec) {
            // File may have been deleted — skip silently
            continue;
        }

        if (current_mtime == cached_mtime) {
            continue;
        }

        // File changed!
        cached_mtime = current_mtime;

        auto comp_name = component_name_from_path(fs::path(abs_path));
        auto lvgl_path = file_to_lvgl_path_[abs_path];

        spdlog::info("[HotReload] Detected change: {} ({})", comp_name, abs_path);

        if (reload_callback_) {
            // Test mode — invoke callback directly instead of LVGL operations
            reload_callback_(comp_name, lvgl_path);
            if (after_reload_callback_)
                after_reload_callback_(comp_name);
        } else {
            // Marshal the reload to the LVGL main thread
            auto reload_name = comp_name;
            auto reload_path = lvgl_path;
            auto after_cb = after_reload_callback_;
            helix::ui::queue_update([reload_name, reload_path, after_cb]() {
                auto start = std::chrono::steady_clock::now();

                size_t subject_count = count_xml_subjects(reload_name.c_str());
                if (subject_count > 0) {
                    spdlog::warn(
                        "[HotReload] '{}' defines {} XML subject(s); any live widget bound "
                        "to these subjects will hold a dangling pointer until rebuilt",
                        reload_name, subject_count);
                }

                // Unregister old component definition
                auto result = lv_xml_component_unregister(reload_name.c_str());
                if (result != LV_RESULT_OK) {
                    spdlog::warn("[HotReload] Failed to unregister '{}' — registering fresh",
                                 reload_name);
                }

                // Re-register from the updated file
                result = lv_xml_register_component_from_file(reload_path.c_str());
                if (result != LV_RESULT_OK) {
                    spdlog::error("[HotReload] Failed to re-register '{}' from {}", reload_name,
                                  reload_path);
                    return;
                }

                auto elapsed = std::chrono::steady_clock::now() - start;
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
                spdlog::info("[HotReload] Reloaded: {} ({:.1f}ms)", reload_name, us / 1000.0);

                if (after_cb)
                    after_cb(reload_name);
            });
        }
    }
}

std::string XmlHotReloader::component_name_from_path(const fs::path& path) {
    // "home_panel.xml" -> "home_panel"
    return path.stem().string();
}

} // namespace helix
