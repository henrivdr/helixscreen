// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-test isolation guard.
//
// Many flaky failures in the single-process test binary come from a test that
// mutates PROCESS-GLOBAL state and never restores it: the working directory
// (chdir) or the HELIX_DATA_DIR / HELIX_CONFIG_DIR env vars that drive
// find_readable()/get_data_dir(). A later test in the same process then can't
// locate assets/config/* and fails with confusing "file not found" symptoms
// (e.g. MacroManager returns 0 macros, default_layout.json falls back to
// hardcoded anchors). These are invisible in isolation and shard-dependent in
// CI.
//
// This listener captures CWD + the data/config env vars before each TEST_CASE
// and reports any test that leaves them changed — pinpointing the leaking test
// by name, deterministically, in a single run. It does not fail the run (so it
// can also serve as a passive regression tripwire); grep stderr for
// "[ISOLATION-LEAK]".

#include "../catch_amalgamated.hpp"
#include "ui_observer_guard.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

std::string current_cwd() {
    std::array<char, 4096> buf{};
    const char* r = getcwd(buf.data(), buf.size());
    return r ? std::string(r) : std::string("<getcwd-failed>");
}

std::string env_or(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string("<unset>");
}

// Live thread count from /proc/self/status ("Threads:"). A test that spawns a
// background thread (e.g. an hv::EventLoopThread) and does not join it before
// returning leaks it; the loop later fires a callback on freed state and crashes
// a *different* test (UAF in hio_get / a freed observer). This count can't be
// auto-healed (we can't safely kill a thread), but naming the leaking test makes
// the otherwise-nondeterministic crash diagnosable in one run.
int live_thread_count() {
    std::ifstream st("/proc/self/status");
    std::string line;
    while (std::getline(st, line)) {
        if (line.rfind("Threads:", 0) == 0) {
            return std::atoi(line.c_str() + 8);
        }
    }
    return -1;
}

class IsolationListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(Catch::TestRunStats const& /*stats*/) override {
        // Production teardown calls ObserverGuard::invalidate_all() before LVGL is
        // torn down so surviving singletons release their observers instead of
        // calling lv_observer_remove() on freed state. The test binary has no such
        // hook, so global panel singletons (e.g. PrintStatusPanel) held in
        // function-local statics destruct during __run_exit_handlers and crash in
        // lv_ll_remove (SIGSEGV at exit, after every test passed). Bumping the
        // epoch here makes those at-exit ObserverGuard::reset() calls release
        // safely. No tests run after this point, so skipping the removes is free.
        ObserverGuard::invalidate_all();
    }

    void testCaseStarting(Catch::TestCaseInfo const& info) override {
        name_ = info.name;
        cwd_ = current_cwd();
        data_dir_ = env_or("HELIX_DATA_DIR");
        config_dir_ = env_or("HELIX_CONFIG_DIR");
        threads_ = live_thread_count();
    }

    void testCaseEnded(Catch::TestCaseStats const& /*stats*/) override {
        // Report AND auto-heal: a leaked CWD / data-dir env breaks find_readable()
        // for every later test in the process (cascading "file not found"
        // failures). Restoring here makes that failure class structurally
        // impossible even if a future test forgets to clean up, while the warning
        // ensures the offending test still gets fixed at the source.
        if (cwd_ != current_cwd()) {
            warn("cwd", cwd_, current_cwd());
            (void)chdir(cwd_.c_str());
        }
        heal_env("HELIX_DATA_DIR", data_dir_);
        heal_env("HELIX_CONFIG_DIR", config_dir_);

        // Thread leaks can't be healed; settle briefly to avoid flagging a thread
        // that is mid-exit, then report a genuine increase.
        int now = live_thread_count();
        if (threads_ >= 0 && now > threads_) {
            for (int i = 0; i < 20 && live_thread_count() > threads_; ++i) {
                usleep(5000); // up to 100ms for a joining/exiting thread to clear
            }
            now = live_thread_count();
            if (now > threads_) {
                std::fprintf(stderr,
                             "\n[ISOLATION-LEAK] test \"%s\" leaked %d thread(s): %d -> %d "
                             "(likely an unjoined hv::EventLoopThread → later UAF crash)\n",
                             name_.c_str(), now - threads_, threads_, now);
            }
        }
    }

  private:
    void warn(const char* what, const std::string& before, const std::string& after) {
        std::fprintf(stderr,
                     "\n[ISOLATION-LEAK] test \"%s\" did not restore %s: \"%s\" -> \"%s\" "
                     "(auto-healed)\n",
                     name_.c_str(), what, before.c_str(), after.c_str());
    }

    void heal_env(const char* var, const std::string& before) {
        std::string after = env_or(var);
        if (before == after) {
            return;
        }
        warn(var, before, after);
        if (before == "<unset>") {
            unsetenv(var);
        } else {
            setenv(var, before.c_str(), 1);
        }
    }

    std::string name_;
    std::string cwd_;
    std::string data_dir_;
    std::string config_dir_;
    int threads_ = -1;
};

} // namespace

CATCH_REGISTER_LISTENER(IsolationListener)
