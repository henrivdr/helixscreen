# L0 — ErrorCenter Core (severity-classified surfacing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Evolve `GcodeErrorRouter` so every backend's error — not just Creality CFS coded errors — is classified by **severity** and surfaced correctly: an uncoded `!!` that pauses the print becomes a CRITICAL **modal with the full message**, instead of today's swallowable transient toast.

**Architecture:** Split the **decision** from the **presentation**. A new pure function `error_classify::classify(raw_line, ClassifyContext) → std::optional<ErrorEvent>` produces a severity-tagged `ErrorEvent` (reusing the existing `clean_error_text` translation). `GcodeErrorRouter::process_line` calls it, then routes by `severity` (CRITICAL→modal, WARNING→toast), preserving the existing `key298` recover action, `key8xx` modal, `Error:`, RPC-dedup, and `gcode_store` replay paths. No second `notify_gcode_response` consumer is created.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (amalgamated), `make test-run`. Spec: `docs/devel/plans/2026-06-16-backend-error-recovery-source.md`.

**Scope of THIS plan (L0 slice):** the `ErrorEvent` model, the pure classifier with severity, pause-state input, the per-backend `classify_error` hook (default only; AFC impl is L1), and the router routing-by-severity. **Out of scope (later tasks / L1):** persistent header badge, multi-button recovery rendering via `ActionPromptModal`, `print_stats.message` ingestion, AFC-specific classification & recovery actions.

**Acceptance (spec callouts closed by L0):** E1 (jam → modal), E2 (full untruncated detail in the modal), E4 (real message, never bare "Error" for uncoded pausing errors).

---

## File Structure

| File | Responsibility | Action |
|------|----------------|--------|
| `include/error_event.h` | `ErrorSeverity`, `ErrorSource`, `RecoveryAction`, `ErrorEvent`, `ClassifyContext` structs | Create |
| `include/error_classify.h` | `classify()` pure function decl | Create |
| `src/application/error_classify.cpp` | `classify()` impl (severity rules + reuse of `clean_error_text`) | Create |
| `include/gcode_error_router.h` | expose `clean_error_text` already public; no struct change | Modify (minor) |
| `src/application/gcode_error_router.cpp` | route `process_line` by `ErrorEvent.severity` | Modify |
| `include/printer_state.h` / `src/printer/printer_state.cpp` | track `pause_resume.is_paused` → `is_paused()` | Modify |
| `include/ams_backend.h` | virtual `classify_error()` hook, default `{}` | Modify |
| `tests/unit/test_error_classify.cpp` | classifier unit tests | Create |
| `tests/unit/test_gcode_error_routing.cpp` | routing-by-severity tests | Create |

---

## Task 1: ErrorEvent model

**Files:**
- Create: `include/error_event.h`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_error_classify.cpp` with just the model test for now:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_amalgamated.hpp"
#include "error_event.h"

using helix::ErrorEvent;
using helix::ErrorSeverity;
using helix::ErrorSource;

TEST_CASE("ErrorEvent defaults are safe", "[error-center][model]") {
    ErrorEvent e;
    REQUIRE(e.severity == ErrorSeverity::WARNING);   // conservative default
    REQUIRE(e.source == ErrorSource::GENERIC);
    REQUIRE(e.detail.empty());
    REQUIRE(e.recovery_actions.empty());
    REQUIRE_FALSE(e.sticky);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test-run 2>/dev/null; ./build/bin/helix-tests "[error-center][model]" -v` (or build tests: `make test`)
Expected: FAIL to compile — `error_event.h` not found.

- [ ] **Step 3: Write minimal implementation**

Create `include/error_event.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace helix {

enum class ErrorSeverity { INFO, WARNING, CRITICAL };

enum class ErrorSource {
    GENERIC, KLIPPER, HEATER,
    AFC, CFS, IFS, QIDI, HAPPY_HARE, SNAPMAKER, ACE, TOOLCHANGER
};

/// A recovery action offered alongside a CRITICAL error. In L0 these are
/// populated only by classifiers that already know a one-tap fix (the
/// migrated CFS key840 case); per-backend smart actions arrive in L1.
struct RecoveryAction {
    std::string label;   ///< Button label, e.g. "Reset CFS"
    std::string gcode;   ///< G-code to run on tap
    std::string log_tag; ///< spdlog tag on tap
};

/// Result of classifying one gcode-response line. Produced by the pure
/// `error_classify::classify()`; consumed by the router's presenter.
struct ErrorEvent {
    ErrorSource   source   = ErrorSource::GENERIC;
    ErrorSeverity severity = ErrorSeverity::WARNING;
    std::string   title;   ///< short, already-translated; "" → presenter default
    std::string   detail;  ///< FULL, untruncated, translated message text
    std::string   code;    ///< Klipper error code if any ("key840"), else ""
    std::vector<RecoveryAction> recovery_actions;
    bool          sticky = false;
};

/// Inputs the classifier needs beyond the raw line. Kept as a plain struct
/// so the classifier stays pure and unit-testable without globals.
struct ClassifyContext {
    bool is_paused  = false;  ///< print currently paused (pause_resume.is_paused)
    bool is_printing = false; ///< print active (print_stats.state == "printing")
};

}  // namespace helix
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[error-center][model]" -v`
Expected: PASS (1 assertion section).

- [ ] **Step 5: Commit**

```bash
git add include/error_event.h tests/unit/test_error_classify.cpp
git commit -m "feat(error-center): add ErrorEvent model"
```

---

## Task 2: Pure classifier — severity rules + reuse of clean_error_text

**Files:**
- Create: `include/error_classify.h`, `src/application/error_classify.cpp`
- Test: `tests/unit/test_error_classify.cpp` (extend)

The classifier reuses `GcodeErrorRouter::clean_error_text` (already `static` + public, `gcode_error_router.h:55`) for translation, then assigns severity. Rules:

- code starts with `key8` → CRITICAL, source CFS (CFS/motor hardware fault).
- code `key298` → WARNING, source CFS, recovery action "Recover" (kept as today; the router attaches the actual gcode-side recovery — see Task 5).
- code `key840` → CRITICAL, source CFS, recovery action {"Reset CFS","BOX_ERROR_CLEAR"}.
- no code, line is an `!!` error, `ctx.is_paused || ctx.is_printing` → **CRITICAL**, source GENERIC (this is the AFC-jam case).
- no code, `!!`, idle → WARNING, source GENERIC.
- `Error:` command error → WARNING, source KLIPPER.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_error_classify.cpp`)

```cpp
#include "error_classify.h"
using helix::ClassifyContext;
using helix::error_classify::classify;

TEST_CASE("uncoded jam !! while paused is CRITICAL", "[error-center][classify]") {
    ClassifyContext ctx; ctx.is_paused = true;
    auto e = classify("!! Toolhead runout detected by tool_end sensor, but upstream "
                      "sensors still detect filament. Possible filament break or jam "
                      "at the toolhead. Please clear the jam and reload filament "
                      "manually, then resume the print.", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::GENERIC);
    REQUIRE(e->code.empty());
    // E2: full text preserved, not truncated
    REQUIRE(e->detail.find("reload filament manually") != std::string::npos);
    REQUIRE(e->detail.size() > 80);
}

TEST_CASE("uncoded !! while idle is WARNING", "[error-center][classify]") {
    ClassifyContext ctx;  // idle
    auto e = classify("!! Timer too close", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
}

TEST_CASE("CFS key8xx is CRITICAL", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key849","msg":"retract failed","values":[1]})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->code == "key849");
}

TEST_CASE("key840 carries a recovery action", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key840","msg":"box switch state error"})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->code == "key840");
    REQUIRE(e->recovery_actions.size() == 1);
    REQUIRE(e->recovery_actions[0].gcode == "BOX_ERROR_CLEAR");
}

TEST_CASE("Error: command error is WARNING/KLIPPER", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify("Error: Must home axis first", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->source == helix::ErrorSource::KLIPPER);
}

TEST_CASE("non-error line yields nullopt", "[error-center][classify]") {
    ClassifyContext ctx;
    REQUIRE_FALSE(classify("// AFC_Brush: Clean Nozzle", ctx).has_value());
    REQUIRE_FALSE(classify("ok T:210", ctx).has_value());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test && ./build/bin/helix-tests "[error-center][classify]" -v`
Expected: FAIL — `error_classify.h` not found.

- [ ] **Step 3: Write minimal implementation**

Create `include/error_classify.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "error_event.h"
#include <optional>
#include <string>

namespace helix::error_classify {

/// Classify a single raw gcode-response line into an ErrorEvent.
/// Returns nullopt for non-error lines (`//`, `ok`, status, action:prompt).
/// Pure: all environment comes via `ctx`. Reuses
/// GcodeErrorRouter::clean_error_text for translation.
std::optional<ErrorEvent> classify(const std::string& raw_line, const ClassifyContext& ctx);

}  // namespace helix::error_classify
```

Create `src/application/error_classify.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_classify.h"

#include "gcode_error_router.h"  // clean_error_text

#include <cctype>

namespace helix::error_classify {

namespace {

ErrorSource source_for_code(const std::string& code) {
    if (code.rfind("key", 0) == 0) return ErrorSource::CFS;  // Creality CFS codes
    return ErrorSource::GENERIC;
}

bool is_error_prefix(const std::string& line) {
    if (line.size() >= 2 && line[0] == '!' && line[1] == '!') return true;
    if (line.size() >= 6) {
        std::string p = line.substr(0, 5);
        for (auto& c : p) c = static_cast<char>(std::tolower(c));
        if (p == "error" && line[5] == ':') return true;
    }
    return false;
}

}  // namespace

std::optional<ErrorEvent> classify(const std::string& raw_line, const ClassifyContext& ctx) {
    if (!is_error_prefix(raw_line)) return std::nullopt;

    ErrorEvent e;
    const bool is_bang = raw_line.size() >= 2 && raw_line[0] == '!' && raw_line[1] == '!';

    // Strip the prefix the same way GcodeErrorRouter does.
    std::string text;
    if (is_bang) {
        text = (raw_line.size() > 3 && raw_line[2] == ' ') ? raw_line.substr(3)
                                                           : raw_line.substr(2);
    } else {  // "Error:"
        text = (raw_line.size() > 7 && raw_line[6] == ' ') ? raw_line.substr(7)
                                                           : raw_line.substr(6);
    }

    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);  // translate + extract code
    e.detail = text;
    e.code = code;

    if (!is_bang) {  // "Error:" command error
        e.source = ErrorSource::KLIPPER;
        e.severity = ErrorSeverity::WARNING;
        return e;
    }

    // `!!` emergency error.
    if (!code.empty()) {
        e.source = source_for_code(code);
        if (code.rfind("key8", 0) == 0) {
            e.severity = ErrorSeverity::CRITICAL;
            if (code == "key840") {
                e.recovery_actions.push_back(
                    {"Reset CFS", "BOX_ERROR_CLEAR", "error_classify::key840_reset"});
            }
        } else if (code == "key298") {
            e.severity = ErrorSeverity::WARNING;  // toast + Recover (router wires gcode)
            e.recovery_actions.push_back({"Recover", "", "error_classify::key298_recover"});
        } else {
            e.severity = ErrorSeverity::WARNING;
        }
        return e;
    }

    // Uncoded `!!` (AFC jam, Happy Hare, generic). Pausing/printing → CRITICAL.
    e.source = ErrorSource::GENERIC;
    e.severity = (ctx.is_paused || ctx.is_printing) ? ErrorSeverity::CRITICAL
                                                     : ErrorSeverity::WARNING;
    e.sticky = (e.severity == ErrorSeverity::CRITICAL);
    return e;
}

}  // namespace helix::error_classify
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test && ./build/bin/helix-tests "[error-center][classify]" -v`
Expected: PASS (all sections).

- [ ] **Step 5: Commit**

```bash
git add include/error_classify.h src/application/error_classify.cpp tests/unit/test_error_classify.cpp
git commit -m "feat(error-center): pure severity classifier reusing clean_error_text"
```

---

## Task 3: Pause state in PrinterState

`webhooks.state`/`state_message` already exist (`printer_state.cpp:442-466`). `pause_resume.is_paused` is NOT tracked. Add a plain bool getter (no subject needed for L0 — the router reads it synchronously on the main thread when presenting).

**Files:**
- Modify: `include/printer_state.h`, `src/printer/printer_state.cpp`
- Test: `tests/unit/test_printer_state_pause.cpp` (create)

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_amalgamated.hpp"
#include "helix_test_fixture.h"
#include "printer_state.h"
#include "hv/json.hpp"

TEST_CASE_METHOD(HelixTestFixture, "PrinterState tracks pause_resume.is_paused",
                 "[printer-state][pause]") {
    auto& ps = PrinterState::instance();
    nlohmann::json status = {{"pause_resume", {{"is_paused", true}}}};
    ps.update_from_status(status);
    REQUIRE(ps.is_paused());

    status = {{"pause_resume", {{"is_paused", false}}}};
    ps.update_from_status(status);
    REQUIRE_FALSE(ps.is_paused());
}
```

> Note: confirm `update_from_status` is public or add a test-only accessor; the existing AMS tests call `handle_status_update` directly on backends — mirror whichever is accessible. If `update_from_status` is private, expose a `set_paused_for_test(bool)` guarded by tests, or make the test feed via `update_from_notification` with `{"params":[status]}`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test && ./build/bin/helix-tests "[printer-state][pause]" -v`
Expected: FAIL — `is_paused()` not declared.

- [ ] **Step 3: Write minimal implementation**

In `include/printer_state.h`, add to the public section (near other simple bool getters):

```cpp
/// True when pause_resume.is_paused was last reported true by Klipper.
bool is_paused() const { return is_paused_; }
```

and a private member:

```cpp
bool is_paused_ = false;
```

In `src/printer/printer_state.cpp`, inside `update_from_status(const json& status)` (the main-thread parser, ~line 316), add a parse block mirroring the `webhooks` block at 442:

```cpp
if (status.contains("pause_resume")) {
    const auto& pr = status["pause_resume"];
    if (pr.contains("is_paused") && pr["is_paused"].is_boolean()) {
        is_paused_ = pr["is_paused"].get<bool>();
    }
}
```

Ensure `pause_resume` is in the object subscription list. Find where objects are subscribed (grep `"webhooks"` / `subscribe` in `printer_state.cpp` / `moonraker_api.cpp`) and add `pause_resume` (it reports `is_paused`).

- [ ] **Step 4: Run to verify it passes**

Run: `make test && ./build/bin/helix-tests "[printer-state][pause]" -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/printer_state.h src/printer/printer_state.cpp tests/unit/test_printer_state_pause.cpp
git commit -m "feat(printer-state): track pause_resume.is_paused"
```

---

## Task 4: Per-backend classify hook on AmsBackend (default only)

L0 adds the seam; AFC's implementation is L1. ErrorCenter will consult the active backend before the generic classifier (Task 5).

**Files:**
- Modify: `include/ams_backend.h`
- Test: `tests/unit/test_ams_backend_classify.cpp` (create)

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_amalgamated.hpp"
#include "ams_backend.h"
#include "error_event.h"

// Minimal fake exposing the default + an override.
class FakeClassBackend : public AmsBackend {
  public:
    // ... only the override under test; other pure virtuals stubbed minimally
    std::optional<helix::ErrorEvent> classify_error(
        const std::string& raw, const helix::ClassifyContext&) const override {
        if (raw.find("FAKE-JAM") != std::string::npos) {
            helix::ErrorEvent e;
            e.severity = helix::ErrorSeverity::CRITICAL;
            e.source = helix::ErrorSource::AFC;
            e.detail = "fake jam";
            return e;
        }
        return std::nullopt;
    }
    // NOTE: stub remaining pure virtuals (see test_ams_backend_base_char.cpp helper)
};

TEST_CASE("AmsBackend::classify_error default returns nullopt", "[ams][error-center]") {
    // Use an existing concrete backend or the char-test helper that stubs virtuals.
    // Assert the BASE default: a backend that doesn't override returns nullopt.
}

TEST_CASE("AmsBackend::classify_error override is honored", "[ams][error-center]") {
    FakeClassBackend b;
    helix::ClassifyContext ctx;
    auto e = b.classify_error("!! FAKE-JAM xyz", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
}
```

> Use the existing stub helper pattern from `tests/unit/test_ams_backend_base_char.cpp` (it already subclasses `AmsBackend` and stubs the pure virtuals) to avoid re-stubbing 30+ methods. Add `classify_error` to that helper or derive from it.

- [ ] **Step 2: Run to verify it fails**

Run: `make test && ./build/bin/helix-tests "[ams][error-center]" -v`
Expected: FAIL — `classify_error` not a member of `AmsBackend`.

- [ ] **Step 3: Write minimal implementation**

In `include/ams_backend.h`, add `#include "error_event.h"` and, alongside the other `const` query virtuals, add:

```cpp
/// Backend-specific error classification. Default: no opinion (nullopt),
/// so the generic classifier in ErrorCenter handles the line. Override in
/// a backend (AFC first, L1) to recognize domain errors (toolhead jam) and
/// attach severity + recovery actions.
virtual std::optional<helix::ErrorEvent> classify_error(
    const std::string& /*raw_line*/, const helix::ClassifyContext& /*ctx*/) const {
    return std::nullopt;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test && ./build/bin/helix-tests "[ams][error-center]" -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/ams_backend.h tests/unit/test_ams_backend_classify.cpp
git commit -m "feat(ams): add AmsBackend::classify_error hook (default nullopt)"
```

---

## Task 5: Route GcodeErrorRouter by severity

Rewire `process_line` to: (1) ask the active backend `classify_error`, else generic `classify()`; (2) route by `severity`. Preserve `key298` recover, `key8xx` modal, `Error:` toast, RPC-dedup, deferred-toast late-arrival check, and `gcode_store` replay.

**Files:**
- Modify: `src/application/gcode_error_router.cpp`
- Test: `tests/unit/test_gcode_error_routing.cpp` (create)

Introduce a thin testable seam — a pure function mapping an `ErrorEvent` to a presentation decision — so routing is unit-testable without LVGL:

```cpp
enum class PresentAs { MODAL, TOAST, TOAST_WITH_RECOVER, MODAL_WITH_RECOVER, NONE };
PresentAs decide_presentation(const helix::ErrorEvent& e);
```

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_amalgamated.hpp"
#include "gcode_error_router.h"   // decide_presentation exposed in header (Step 3)
#include "error_event.h"

using helix::ErrorEvent; using helix::ErrorSeverity;

TEST_CASE("CRITICAL with no actions → MODAL", "[error-center][routing]") {
    ErrorEvent e; e.severity = ErrorSeverity::CRITICAL;
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::MODAL);
}
TEST_CASE("CRITICAL with a recovery action → MODAL_WITH_RECOVER", "[error-center][routing]") {
    ErrorEvent e; e.severity = ErrorSeverity::CRITICAL;
    e.recovery_actions.push_back({"Reset CFS","BOX_ERROR_CLEAR","t"});
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::MODAL_WITH_RECOVER);
}
TEST_CASE("WARNING with recover action → TOAST_WITH_RECOVER", "[error-center][routing]") {
    ErrorEvent e; e.severity = ErrorSeverity::WARNING;
    e.recovery_actions.push_back({"Recover","","t"});
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::TOAST_WITH_RECOVER);
}
TEST_CASE("WARNING no actions → TOAST", "[error-center][routing]") {
    ErrorEvent e; e.severity = ErrorSeverity::WARNING;
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::TOAST);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test && ./build/bin/helix-tests "[error-center][routing]" -v`
Expected: FAIL — `decide_presentation`/`PresentAs` undeclared.

- [ ] **Step 3: Write minimal implementation**

In `include/gcode_error_router.h` (namespace `helix`, above the class), add:

```cpp
enum class PresentAs { MODAL, TOAST, TOAST_WITH_RECOVER, MODAL_WITH_RECOVER, NONE };
PresentAs decide_presentation(const ErrorEvent& e);
```

In `gcode_error_router.cpp`, implement and wire it. Add near the top:

```cpp
#include "error_classify.h"
#include "error_event.h"
#include "printer_state.h"
#include "ams_state.h"

namespace helix {
PresentAs decide_presentation(const ErrorEvent& e) {
    const bool has_recover = !e.recovery_actions.empty();
    if (e.severity == ErrorSeverity::CRITICAL)
        return has_recover ? PresentAs::MODAL_WITH_RECOVER : PresentAs::MODAL;
    if (e.severity == ErrorSeverity::WARNING)
        return has_recover ? PresentAs::TOAST_WITH_RECOVER : PresentAs::TOAST;
    return PresentAs::NONE;  // INFO not surfaced in L0
}
}  // namespace helix
```

Then refactor `process_line` so the `!!`/`Error:` branches build an `ErrorEvent` and route. Replace the body of `process_line` (keep RPC-dedup + deferred late-arrival behavior) with:

```cpp
void GcodeErrorRouter::process_line(const std::string& line) {
    if (line.empty()) return;

    // Build classify context from current printer state (main thread).
    ClassifyContext ctx;
    ctx.is_paused = PrinterState::instance().is_paused();
    ctx.is_printing = PrinterState::instance().is_printing();  // existing getter

    // Backend-specific classification first, else generic.
    std::optional<ErrorEvent> ev;
    if (auto* backend = AmsState::instance().get_backend())
        ev = backend->classify_error(line, ctx);
    if (!ev) ev = error_classify::classify(line, ctx);
    if (!ev) return;  // not an error line

    spdlog::error("[GcodeError] sev={} src={} code={}: {}",
                  static_cast<int>(ev->severity), static_cast<int>(ev->source),
                  ev->code.empty() ? "-" : ev->code, ev->detail);

    // Cross-source RPC dedup (unchanged): a caller already toasted this root cause.
    if (rpc_error_correlation::was_recently_handled(ev->detail)) {
        spdlog::info("[GcodeError] Suppressing duplicate (RPC-handled): {}", ev->detail);
        return;
    }

    switch (decide_presentation(*ev)) {
        case PresentAs::MODAL:
            ui_notification_error(ev->title.empty() ? lv_tr("Printer Error")
                                                    : ev->title.c_str(),
                                  ev->detail.c_str(), /*modal=*/true);
            return;
        case PresentAs::MODAL_WITH_RECOVER:
            present_recovery_modal(*ev);   // helper preserving the key840 confirmation-modal flow
            return;
        case PresentAs::TOAST_WITH_RECOVER:
            present_recover_toast(*ev);    // helper preserving the key298 show_with_action flow
            return;
        case PresentAs::TOAST:
            // Deferred 150ms toast preserves the late-arriving-RPC suppression (unchanged).
            present_deferred_toast(ev->detail);
            return;
        case PresentAs::NONE:
            return;
    }
}
```

Extract the existing key840 modal block (lines 311–375) into `present_recovery_modal(const ErrorEvent&)`, the key298 block (281–301) into `present_recover_toast(const ErrorEvent&)` (it wires `PrinterRecoveryService` when `gcode` is empty + log_tag is the key298 tag; otherwise runs `e.recovery_actions[0].gcode`), and the deferred-toast block (384–407) into `present_deferred_toast(std::string)`. These are private methods declared in the header. Keep their internal logic byte-for-byte; only the dispatch changes.

> The replay path (`on_connected`) keeps using `ui_notification_error(..., modal=true)` directly — it is already always-modal by design (#991 age gate stays). No change there beyond optionally reusing `classify()` for the title; out of scope for L0.

- [ ] **Step 4: Run to verify it passes**

Run: `make test && ./build/bin/helix-tests "[error-center][routing]" -v`
Expected: PASS. Then full suite: `make test-run` — expect no regressions in `[error-center]`, AMS, printer-state tags.

- [ ] **Step 5: Commit**

```bash
git add include/gcode_error_router.h src/application/gcode_error_router.cpp tests/unit/test_gcode_error_routing.cpp
git commit -m "feat(error-center): route gcode errors by severity (uncoded pausing !! -> modal)"
```

---

## Task 6: On-device verification (Voron, reproducible jam)

- [ ] **Step 1:** Build for Pi and deploy to the Voron (`PI_HOST=192.168.1.112`-style per the device's deploy path) OR run locally with the mock emitting a synthetic `!!`.
- [ ] **Step 2:** Reproduce the jam (the error recurs readily) OR inject via console: `M118 !! Toolhead runout detected by tool_end sensor, but upstream sensors still detect filament. Possible filament break or jam at the toolhead. Please clear the jam and reload filament manually, then resume the print.` while paused.
- [ ] **Step 3:** Confirm a **modal** appears with the **full** message (not a toast, not "Error", not truncated). Screenshot for the PR.
- [ ] **Step 4:** Confirm CFS coded errors (if testable) and `Error:` lines still behave as before (no regression).

---

## Self-Review

- **Spec coverage:** E1 (Task 5 routes uncoded pausing `!!`→modal), E2 (Task 2 preserves full `detail`; Task 5 passes full text to modal), E4 (real translated message, never bare "Error"). R-/S-/P- callouts are explicitly out of L0 scope (L1–L3). ✓
- **Placeholder scan:** Task 3 notes a conditional (`update_from_status` visibility) — resolved by mirroring existing test access; not a placeholder in the impl. Task 5 names three extracted helpers with explicit "keep logic byte-for-byte" instructions. ✓
- **Type consistency:** `ErrorEvent`/`ErrorSeverity`/`ClassifyContext`/`RecoveryAction` fields are identical across Tasks 1, 2, 4, 5. `classify()` signature matches in header and tests. `decide_presentation`/`PresentAs` consistent between header and test. ✓
- **Threading:** `process_line` runs on the WS thread today via `lifetime_.bg_cb` (router ctor) which **already defers to main**, so `PrinterState::instance().is_paused()` and presentation run on the main thread — safe. No new bg-thread access introduced. ✓
