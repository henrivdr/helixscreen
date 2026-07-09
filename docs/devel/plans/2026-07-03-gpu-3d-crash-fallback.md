# GPU 3D gcode-preview crash fallback (#966 / #1084 / #1085)

## Problem
On constrained Mali/Panfrost SBCs (BTT CB1 Mali-G31, CB2/RK3566 Mali-G52), the 3D
gcode preview SIGSEGVs **inside** the GPU driver during `glDrawArrays`
(`gcode_gles_renderer.cpp:1318`). Signature across all 3 reports: `SIGSEGV`,
`fault_addr=0x28`, PC inside `rockchip_dri.so`/GLES driver, spine
`gcode_viewer_draw_cb → render → render_to_fbo → draw_layers → glDrawArrays`.

The existing reactive guard `gl_draw_error_is_fatal(glGetError())`
(`gcode_gles_renderer.cpp:1335`) **cannot** catch this: control never returns from
`glDrawArrays`, so `glGetError()` never runs. Only a *proactive* (pre-first-draw)
gate prevents it.

## Fix = two complementary layers

### Layer 1 — GL_RENDERER denylist (proactive, before first draw)
Known-bad GPUs skip the GPU path entirely, so affected users never hit even one crash.

### Layer 2 — crash-loop breaker (catches unknown-bad GPUs)
A guard file written before the first GPU draw and removed after the first successful
frame. If the process dies mid-draw the file survives; next startup promotes it to a
persistent block. Covers any GPU that hard-faults, not just the denylisted ones.

## Changes

1. `include/gcode_gl_fallback.h` — add pure, header-only, unit-testable predicate
   `bool gl_renderer_is_denylisted(const char* renderer)` (case-insensitive substring
   match; seed list = {"panfrost"}). Also logs are added elsewhere so we capture the
   exact GL_RENDERER string in the field to refine the list.

2. `tests/unit/test_gcode_gl_fallback.cpp` — TEST-FIRST. Add `[gl_fallback]` cases for
   the new predicate (matches "Mali-G52 (Panfrost)", case-insensitive; rejects nullptr,
   "", llvmpipe, Mesa Intel, Apple; documents that bare "Mali-G52" is intentionally the
   breaker's job, not the denylist's).

3. `include/gcode_gles_renderer.h` — members `bool gpu_checked_=false;`
   `bool gpu_guard_armed_=false;` `bool gpu_guard_cleared_=false;`; private
   `void arm_gpu_guard(); void clear_gpu_guard();`.

4. `src/rendering/gcode_gles_renderer.cpp` `render()`:
   - after `init_gl()`: `if (gl_render_failed_) return;` (sticky bail once failed)
   - after context guard `.ok()`: one-time `gpu_checked_` block — read+log GL_RENDERER,
     if `gl_renderer_is_denylisted(r)` → log error, `gl_render_failed_ = true;` return
     (viewer's existing `render_failed()` poll flips to 2D, no draw issued).
   - wrap the real GPU draw: `arm_gpu_guard();` immediately before
     `render_to_fbo(gcode, camera);` and `clear_gpu_guard();` right after
     `blit_to_lvgl(...)`. (Cached-blit early-returns never reach here, correct.)
   - `arm_gpu_guard`: if !armed, write `helix::writable_path("gpu_3d_guard")` via
     ofstream, set armed. `clear_gpu_guard`: if armed && !cleared, remove file, set
     cleared.

5. `src/application/application.cpp`:
   - add `gpu_3d_guard_path()` helper next to `crash_marker_path()`.
   - after the existing crash-loop block: if guard file exists → warn, set
     `Config /display/gpu_3d_blocked = true` + save, remove the guard file.

6. `src/system/config.cpp` `get_default_display_config()` — add `{"gpu_3d_blocked", false}`.

7. `src/ui/ui_gcode_viewer.cpp`:
   - `GCodeViewerState` member `bool gpu_3d_blocked_{false};`
   - constructor: read `Config /display/gpu_3d_blocked` into it (log if set).
   - `is_using_2d_mode()` ENABLE_3D_RENDERER branch: `... || gpu_3d_blocked_`.

8. `src/system/display_settings_manager.cpp` `set_gcode_render_mode()` — after persisting
   the mode, clear `/display/gpu_3d_blocked` (+save): an explicit user mode choice lets
   them retry the GPU path.

## Verify
- `./build/bin/helix-tests "[gl_fallback]"` (new cases fail before impl, pass after).
- `make -j` builds clean.
