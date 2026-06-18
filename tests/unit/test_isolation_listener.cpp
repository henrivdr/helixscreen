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

#include <array>
#include <cstdio>
#include <cstdlib>
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

class IsolationListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const& info) override {
        name_ = info.name;
        cwd_ = current_cwd();
        data_dir_ = env_or("HELIX_DATA_DIR");
        config_dir_ = env_or("HELIX_CONFIG_DIR");
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
};

} // namespace

CATCH_REGISTER_LISTENER(IsolationListener)
