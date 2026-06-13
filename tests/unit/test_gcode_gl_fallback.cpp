// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Regression tests for the 3D→2D defensive fallback decision (issue #966).
//
// The 3D GLES gcode renderer can fault inside the GPU driver on constrained
// boards (Mali-G31 on Allwinner CB1) under memory pressure. The defensive
// guard checks glGetError() once after each draw batch and, on a fatal error,
// abandons GPU rendering for the rest of the session — the viewer degrades to
// the pure-CPU 2D renderer instead of letting the driver crash the process.
//
// A unit test cannot drive a real GL context, so the testable seam is the pure
// decision predicate `gl_draw_error_is_fatal()` that gates the fallback. These
// tests FAIL if that predicate is weakened or removed.

#include "gcode_gl_fallback.h"

#include "../catch_amalgamated.hpp"

using helix::gcode::gl_draw_error_is_fatal;
using helix::gcode::kGLInvalidOperation;
using helix::gcode::kGLOutOfMemory;

TEST_CASE("gl_draw_error_is_fatal triggers fallback on out-of-memory", "[gcode][gl_fallback]") {
    // GL_OUT_OF_MEMORY is the primary CB1/Mali fault signature — must fall back.
    REQUIRE(gl_draw_error_is_fatal(kGLOutOfMemory));
    REQUIRE(gl_draw_error_is_fatal(0x0505)); // literal value guards against renumbering
}

TEST_CASE("gl_draw_error_is_fatal triggers fallback on invalid-operation",
          "[gcode][gl_fallback]") {
    // GL_INVALID_OPERATION means the command stream is in a driver-rejected
    // state; continuing to draw is unsafe.
    REQUIRE(gl_draw_error_is_fatal(kGLInvalidOperation));
    REQUIRE(gl_draw_error_is_fatal(0x0502));
}

TEST_CASE("gl_draw_error_is_fatal does NOT fall back on GL_NO_ERROR", "[gcode][gl_fallback]") {
    // The common case — a healthy frame — must keep rendering in 3D. If this
    // returned true the viewer would thrash to 2D on every frame.
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0000)); // GL_NO_ERROR
}

TEST_CASE("gl_draw_error_is_fatal ignores non-fatal recoverable errors",
          "[gcode][gl_fallback]") {
    // These errors indicate a programming mistake in the call, but the driver
    // stays in a defined state and the next frame can recover — they must NOT
    // permanently disable GPU rendering.
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0500)); // GL_INVALID_ENUM
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0501)); // GL_INVALID_VALUE
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0506)); // GL_INVALID_FRAMEBUFFER_OPERATION
}

TEST_CASE("gl_draw_error_is_fatal constants match OpenGL ES values",
          "[gcode][gl_fallback]") {
    // The predicate is header-only (no GL dependency) so it is testable on
    // builds without ENABLE_GLES_3D. Pin the constants to the real GL ES
    // numbers so a typo can never silently disable the guard.
    REQUIRE(kGLOutOfMemory == 0x0505u);
    REQUIRE(kGLInvalidOperation == 0x0502u);
}
