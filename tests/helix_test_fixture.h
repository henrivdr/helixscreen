// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Base fixture for every HelixScreen test. Deterministically resets
// process-wide singletons tests are known to mutate so ordering cannot
// mask bugs. Expand reset_all() reactively as flakiness surfaces.
//
// Derive test-specific fixtures from this (LVGLTestFixture does). Plain
// non-LVGL unit tests can use it directly via TEST_CASE_METHOD.
//
// Note: the first reset_all() call initializes SystemSettingsManager's subjects,
// which self-register with StaticSubjectRegistry for process-lifetime. Once
// initialized, they stay initialized — that's intentional and harmless, but
// worth knowing for future derived fixtures.
class HelixTestFixture {
  public:
    HelixTestFixture();          // calls reset_all() on entry — idempotent
    virtual ~HelixTestFixture(); // calls reset_all() on exit — leaves clean slate

    HelixTestFixture(const HelixTestFixture&) = delete;
    HelixTestFixture& operator=(const HelixTestFixture&) = delete;
    HelixTestFixture(HelixTestFixture&&) = delete;
    HelixTestFixture& operator=(HelixTestFixture&&) = delete;

  protected:
    // List expands reactively. Keep small; don't over-reset.
    static void reset_all();
};
