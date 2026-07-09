// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>

namespace mock_internal {

// Stateful mock queue — jobs can be deleted, queue can be paused/started.
//
// Reset inside register_queue_handlers() so each MoonrakerClientMock
// construction starts with a clean slate — otherwise job state leaks
// between tests running in the same process (L053).
static struct MockQueueState {
    struct Job {
        std::string job_id;
        std::string filename;
        double time_added;
    };
    std::vector<Job> jobs;
    std::string queue_state;
    // Monotonic ID counter — never collides with existing or deleted IDs,
    // unlike the old `size()+1` scheme which reused IDs after deletion.
    int next_id = 1;

    void reset() {
        double now = static_cast<double>(time(nullptr));
        jobs = {
            {"0001", "benchy_v2.gcode", now - 3600},
            {"0002", "calibration_cube.gcode", now - 1800},
            {"0003", "phone_stand.gcode", now - 300},
        };
        queue_state = "ready";
        next_id = 4; // Next-available ID; preserves 4-digit zero-padded format.
    }

    std::string allocate_id() {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04d", next_id++);
        return std::string(buf);
    }
} s_mock_queue;

void register_queue_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // Reset state on every handler registration — ensures each MoonrakerClientMock
    // instance sees a fresh queue, preventing leakage between tests (L053).
    s_mock_queue.reset();

    // server.job_queue.status — return current mock queue state
    registry["server.job_queue.status"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        double now = static_cast<double>(time(nullptr));
        json result;
        result["queue_state"] = s_mock_queue.queue_state;
        json jobs_arr = json::array();
        for (const auto& job : s_mock_queue.jobs) {
            jobs_arr.push_back({{"job_id", job.job_id},
                                {"filename", job.filename},
                                {"time_added", job.time_added},
                                {"time_in_queue", now - job.time_added}});
        }
        result["queued_jobs"] = jobs_arr;

        spdlog::debug("[MoonrakerClientMock] Returning mock job queue: {} jobs ({})",
                      s_mock_queue.jobs.size(), s_mock_queue.queue_state);

        if (success_cb) {
            success_cb(json{{"result", result}});
        }
        return true;
    };

    // server.job_queue.start — start processing queue
    registry["server.job_queue.start"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        s_mock_queue.queue_state = "ready";
        spdlog::info("[MoonrakerClientMock] Job queue started");
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // server.job_queue.pause — pause queue processing
    registry["server.job_queue.pause"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        s_mock_queue.queue_state = "paused";
        spdlog::info("[MoonrakerClientMock] Job queue paused");
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // server.job_queue.post_job — add job(s) to queue
    registry["server.job_queue.post_job"] =
        []([[maybe_unused]] MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        // Validate input — malformed params are a client bug, report and bail.
        if (!params.contains("filenames") || !params["filenames"].is_array()) {
            if (error_cb) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::VALIDATION_ERROR;
                err.message = "post_job: 'filenames' must be a JSON array";
                err.method = "server.job_queue.post_job";
                error_cb(err);
            }
            return true;
        }

        double now = static_cast<double>(time(nullptr));
        for (const auto& f : params["filenames"]) {
            if (!f.is_string()) {
                spdlog::warn("[MoonrakerClientMock] post_job: skipping non-string filename");
                continue;
            }
            std::string filename = f.get<std::string>();
            std::string id = s_mock_queue.allocate_id();
            s_mock_queue.jobs.push_back({id, filename, now});
            spdlog::info("[MoonrakerClientMock] Added job {} to queue: {}", id, filename);
        }
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // server.job_queue.delete_job — remove job(s) from queue
    registry["server.job_queue.delete_job"] =
        []([[maybe_unused]] MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        if (!params.contains("job_ids") || !params["job_ids"].is_array()) {
            if (error_cb) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::VALIDATION_ERROR;
                err.message = "delete_job: 'job_ids' must be a JSON array";
                err.method = "server.job_queue.delete_job";
                error_cb(err);
            }
            return true;
        }

        for (const auto& id_val : params["job_ids"]) {
            if (!id_val.is_string()) {
                spdlog::warn("[MoonrakerClientMock] delete_job: skipping non-string job_id");
                continue;
            }
            std::string id = id_val.get<std::string>();
            auto it = std::remove_if(s_mock_queue.jobs.begin(), s_mock_queue.jobs.end(),
                                     [&id](const auto& j) { return j.job_id == id; });
            if (it != s_mock_queue.jobs.end()) {
                spdlog::info("[MoonrakerClientMock] Removed job {} from queue", id);
                s_mock_queue.jobs.erase(it, s_mock_queue.jobs.end());
            }
        }
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };
}

} // namespace mock_internal
