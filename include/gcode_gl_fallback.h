// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {
namespace gcode {

// GL error codes that warrant a permanent fall-back to the pure-CPU 2D
// renderer. These mirror the OpenGL ES values so this predicate stays
// header-only and free of any GL dependency (so it is unit-testable on
// targets built without ENABLE_GLES_3D).
//
// GL_OUT_OF_MEMORY (0x0505): the driver could not allocate memory for the
//   draw. On constrained Mali/Panfrost boards (e.g. Allwinner CB1) this can
//   fault inside the driver rather than returning cleanly, so the only safe
//   reaction is to stop issuing 3D draws entirely.
// GL_INVALID_OPERATION (0x0502): the command stream is in a state the driver
//   rejects. Continuing to draw risks undefined behaviour in the driver.
constexpr unsigned int kGLOutOfMemory = 0x0505;
constexpr unsigned int kGLInvalidOperation = 0x0502;

/// Decide whether a GL error observed after a draw batch should trigger a
/// permanent, session-sticky fall-back from the 3D GLES renderer to the
/// pure-CPU 2D renderer.
///
/// Pure function (no GL state, no side effects) so the fall-back decision is
/// unit-testable without a live GL context.
///
/// @param gl_error  the value returned by glGetError() (0 == GL_NO_ERROR)
/// @return true if the error is fatal enough to abandon GPU rendering
inline bool gl_draw_error_is_fatal(unsigned int gl_error) {
    return gl_error == kGLOutOfMemory || gl_error == kGLInvalidOperation;
}

} // namespace gcode
} // namespace helix
