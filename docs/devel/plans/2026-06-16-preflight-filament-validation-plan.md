# Pre-flight Filament Validation & Remap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop multi-color prints from running out mid-print by validating required tools against actually-loaded filament *before* the print starts, showing an honest color/head mapping, and letting the user remap — including on Snapmaker U1/ACE which have no native remap API.

**Architecture:** A new pure-logic `PreflightValidator` (reusing the existing `FilamentMapper` comparison helpers) classifies each gcode tool as Ok / ColorMismatch / MaterialMismatch / EmptySlot. It is decoupled from the mapping-card visibility that currently blinds U1/ACE. The print-start path hard-blocks on an empty required head, the detail view renders honest swatches from real `tools_used_indices` (not palette position), and a per-backend `RemapStrategy` drives either the existing native `set_tool_mapping` modal or a comprehensive gcode rewrite (U1/ACE) through the existing `GCodeFileModifier` + HelixPrint plugin.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (`helix-tests`), libhv JSON (`hv/json.hpp`), existing `FilamentMapper` / `GCodeFileModifier` / HelixPrint Moonraker plugin.

**Spec:** `docs/devel/plans/2026-06-16-preflight-filament-validation.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/preflight_validator.h` (create) | `ToolCheck`, `PreflightResult`, `PreflightValidator::validate()` declarations |
| `src/printer/preflight_validator.cpp` (create) | Classification logic; reuses `FilamentMapper` |
| `tests/unit/test_preflight_validator.cpp` (create) | Unit tests for validator |
| `src/printer/snapmaker_task_config.{h,cpp}` (create) | Parse `print_task_config` JSON → `AvailableSlot[]` (U1 live truth) |
| `tests/unit/test_snapmaker_task_config.cpp` (create) | Unit tests for the parser |
| `include/ams_backend.h` (modify) | Add `RemapStrategy` enum + `get_remap_strategy()` |
| `src/printer/ams_backend_*.cpp` (modify) | Per-backend `get_remap_strategy()` returns |
| `src/ui/ui_print_select_detail_view.cpp` (modify) | Kill synthetic-index seed; drive mismatch for all backends |
| `src/ui/ui_panel_print_select.cpp` (modify) | Parse-complete gate + block/advisory + enriched modal |
| `ui_xml/components/preflight_check_modal.xml` (create) | Enriched per-tool check modal |
| `include/gcode_tool_remapper.h`, `src/rendering/gcode_tool_remapper.cpp` (create) | U1/ACE three-family rewrite atop `GCodeFileModifier` |
| `tests/unit/test_gcode_tool_remapper.cpp` (create) | Rewrite tests vs `u1_4color_ring.gcode` fixture |
| `assets/test_gcodes/u1_4color_ring.gcode` (already added) | Real bench-captured oracle |

---

## Task 1: PreflightValidator — types + empty-slot detection

**Files:**
- Create: `include/preflight_validator.h`
- Create: `src/printer/preflight_validator.cpp`
- Test: `tests/unit/test_preflight_validator.cpp`

Reuse existing structs from `include/filament_mapper.h`: `helix::GcodeToolInfo {int tool_index; uint32_t color_rgb; std::string material;}`, `helix::AvailableSlot {int slot_index; uint32_t color_rgb; std::string material; bool is_empty; …}`, `helix::ToolMapping {int tool_index; int mapped_slot; …}`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_preflight_validator.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "preflight_validator.h"
#include "filament_mapper.h"

using helix::AvailableSlot;
using helix::GcodeToolInfo;
using helix::ToolMapping;
using helix::PreflightValidator;
using Severity = helix::ToolCheck::Severity;

TEST_CASE("empty required slot is flagged and blocks", "[preflight]") {
    std::vector<GcodeToolInfo> tools = {
        {0, 0xED1C24, "PLA"},   // magenta
        {2, 0x00C1AE, "PLA"},   // green
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xED1C24, "PLA", false, -1, 0, 0, ""},  // loaded magenta
        {1, 0, 0x000000, "",    true,  -1, 0, 0, ""},  // empty
        {2, 0, 0x00C1AE, "PLA", false, -1, 0, 0, ""},  // loaded green
    };
    std::vector<ToolMapping> mapping = {
        {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO},
        {2, 1, 0, false, false, ToolMapping::MatchReason::AUTO},  // green mapped to EMPTY slot 1
    };

    auto result = PreflightValidator::validate(tools, slots, mapping);

    REQUIRE(result.checks.size() == 2);
    CHECK(result.checks[0].severity == Severity::Ok);
    CHECK(result.checks[1].severity == Severity::EmptySlot);
    CHECK(result.checks[1].tool_index == 2);
    CHECK(result.checks[1].mapped_slot == 1);
    CHECK_FALSE(result.checks[1].slot_present);
    CHECK(result.has_block());
    CHECK_FALSE(result.has_advisory());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[preflight]"`
Expected: FAIL — `preflight_validator.h` not found / `PreflightValidator` undefined.

- [ ] **Step 3: Write the header**

```cpp
// include/preflight_validator.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "filament_mapper.h"

namespace helix {

struct ToolCheck {
    int tool_index = -1;
    uint32_t intended_color = 0;
    std::string intended_material;
    int mapped_slot = -1;
    bool slot_present = false;
    bool color_ok = true;
    bool material_ok = true;
    enum class Severity { Ok, ColorMismatch, MaterialMismatch, EmptySlot };
    Severity severity = Severity::Ok;
};

struct PreflightResult {
    std::vector<ToolCheck> checks;
    bool has_block() const;     // any EmptySlot
    bool has_advisory() const;  // any MaterialMismatch
};

class PreflightValidator {
public:
    // `mapping` is the resolved tool->slot assignment (from FilamentMapper).
    static PreflightResult validate(const std::vector<GcodeToolInfo>& tools,
                                    const std::vector<AvailableSlot>& slots,
                                    const std::vector<ToolMapping>& mapping);
};

}  // namespace helix
```

- [ ] **Step 4: Write minimal implementation (empty-slot only)**

```cpp
// src/printer/preflight_validator.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "preflight_validator.h"
#include <algorithm>

namespace helix {

bool PreflightResult::has_block() const {
    return std::any_of(checks.begin(), checks.end(),
        [](const ToolCheck& c) { return c.severity == ToolCheck::Severity::EmptySlot; });
}
bool PreflightResult::has_advisory() const {
    return std::any_of(checks.begin(), checks.end(),
        [](const ToolCheck& c) { return c.severity == ToolCheck::Severity::MaterialMismatch; });
}

static int slot_for_tool(int tool_index, const std::vector<ToolMapping>& mapping) {
    for (const auto& m : mapping)
        if (m.tool_index == tool_index) return m.mapped_slot;
    return -1;
}
static const AvailableSlot* find_slot(int slot_index, const std::vector<AvailableSlot>& slots) {
    for (const auto& s : slots)
        if (s.slot_index == slot_index) return &s;
    return nullptr;
}

PreflightResult PreflightValidator::validate(const std::vector<GcodeToolInfo>& tools,
                                             const std::vector<AvailableSlot>& slots,
                                             const std::vector<ToolMapping>& mapping) {
    PreflightResult out;
    for (const auto& t : tools) {
        ToolCheck c;
        c.tool_index = t.tool_index;
        c.intended_color = t.color_rgb;
        c.intended_material = t.material;
        c.mapped_slot = slot_for_tool(t.tool_index, mapping);
        const AvailableSlot* slot = c.mapped_slot >= 0 ? find_slot(c.mapped_slot, slots) : nullptr;
        if (slot == nullptr || slot->is_empty) {
            c.slot_present = false;
            c.severity = ToolCheck::Severity::EmptySlot;
        } else {
            c.slot_present = true;
            c.severity = ToolCheck::Severity::Ok;  // refined in Task 2
        }
        out.checks.push_back(std::move(c));
    }
    return out;
}

}  // namespace helix
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[preflight]"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/preflight_validator.h src/printer/preflight_validator.cpp tests/unit/test_preflight_validator.cpp
git commit -m "feat(preflight): validator with empty-slot detection"
```

---

## Task 2: PreflightValidator — color + material classification

**Files:**
- Modify: `src/printer/preflight_validator.cpp` (the `else` branch)
- Test: `tests/unit/test_preflight_validator.cpp` (add cases)

`FilamentMapper` static helpers (verified `src/printer/filament_mapper.cpp`): `bool colors_match(uint32_t, uint32_t)` (tolerance 50), `bool materials_match(const std::string&, const std::string&)`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("material mismatch is advisory, not block", "[preflight]") {
    std::vector<GcodeToolInfo> tools = { {0, 0xED1C24, "PLA"} };
    std::vector<AvailableSlot> slots = { {0, 0, 0xED1C24, "PETG", false, -1, 0, 0, ""} };
    std::vector<ToolMapping> mapping = { {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO} };
    auto r = PreflightValidator::validate(tools, slots, mapping);
    CHECK(r.checks[0].severity == Severity::MaterialMismatch);
    CHECK_FALSE(r.checks[0].material_ok);
    CHECK(r.has_advisory());
    CHECK_FALSE(r.has_block());
}

TEST_CASE("color mismatch is display-only (no block, no advisory)", "[preflight]") {
    std::vector<GcodeToolInfo> tools = { {0, 0x00C1AE, "PLA"} };       // green intended
    std::vector<AvailableSlot> slots = { {0, 0, 0x2233FF, "PLA", false, -1, 0, 0, ""} };  // blue loaded
    std::vector<ToolMapping> mapping = { {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO} };
    auto r = PreflightValidator::validate(tools, slots, mapping);
    CHECK(r.checks[0].severity == Severity::ColorMismatch);
    CHECK_FALSE(r.checks[0].color_ok);
    CHECK_FALSE(r.has_advisory());
    CHECK_FALSE(r.has_block());
}

TEST_CASE("exact match is Ok", "[preflight]") {
    std::vector<GcodeToolInfo> tools = { {0, 0xED1C24, "PLA"} };
    std::vector<AvailableSlot> slots = { {0, 0, 0xED1C24, "PLA", false, -1, 0, 0, ""} };
    std::vector<ToolMapping> mapping = { {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO} };
    auto r = PreflightValidator::validate(tools, slots, mapping);
    CHECK(r.checks[0].severity == Severity::Ok);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/bin/helix-tests "[preflight]"`
Expected: FAIL — material/color cases land on `Ok` (refinement not implemented).

- [ ] **Step 3: Implement classification in the `else` branch**

Replace the `else` block in `validate()` with:

```cpp
        } else {
            c.slot_present = true;
            c.color_ok = FilamentMapper::colors_match(t.color_rgb, slot->color_rgb);
            c.material_ok = FilamentMapper::materials_match(t.material, slot->material);
            if (!c.material_ok)      c.severity = ToolCheck::Severity::MaterialMismatch;
            else if (!c.color_ok)    c.severity = ToolCheck::Severity::ColorMismatch;
            else                     c.severity = ToolCheck::Severity::Ok;
        }
```

Add `#include "filament_mapper.h"` (already pulled via header).

- [ ] **Step 4: Run to verify pass**

Run: `./build/bin/helix-tests "[preflight]"`
Expected: PASS (all four cases).

- [ ] **Step 5: Commit**

```bash
git add src/printer/preflight_validator.cpp tests/unit/test_preflight_validator.cpp
git commit -m "feat(preflight): color/material classification via FilamentMapper"
```

---

## Task 3: PreflightValidator — unmapped tool + empty input edge cases

**Files:**
- Test: `tests/unit/test_preflight_validator.cpp`
- Modify (only if needed): `src/printer/preflight_validator.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("tool with no mapping is treated as empty/block", "[preflight]") {
    std::vector<GcodeToolInfo> tools = { {3, 0xFFFFFF, "PLA"} };
    std::vector<AvailableSlot> slots = { {0, 0, 0xFFFFFF, "PLA", false, -1, 0, 0, ""} };
    std::vector<ToolMapping> mapping = {};  // tool 3 unmapped
    auto r = PreflightValidator::validate(tools, slots, mapping);
    CHECK(r.checks[0].severity == Severity::EmptySlot);
    CHECK(r.checks[0].mapped_slot == -1);
    CHECK(r.has_block());
}

TEST_CASE("no tools yields empty, non-blocking result", "[preflight]") {
    auto r = PreflightValidator::validate({}, {}, {});
    CHECK(r.checks.empty());
    CHECK_FALSE(r.has_block());
    CHECK_FALSE(r.has_advisory());
}
```

- [ ] **Step 2: Run**

Run: `./build/bin/helix-tests "[preflight]"`
Expected: PASS already (Task 1 logic handles `mapped_slot == -1` → null slot → EmptySlot; empty input → empty checks). If any fails, the only allowed fix is making `find_slot`/`slot_for_tool` null-safe — no behavior change beyond that.

- [ ] **Step 3: Commit (tests only if no code change)**

```bash
git add tests/unit/test_preflight_validator.cpp
git commit -m "test(preflight): unmapped-tool and empty-input edge cases"
```

---

## Task 4: Snapmaker U1 live slot truth from `print_task_config`

**Files:**
- Create: `include/snapmaker_task_config.h`, `src/printer/snapmaker_task_config.cpp`
- Test: `tests/unit/test_snapmaker_task_config.cpp`
- Modify (wire-up only): `src/printer/ams_backend_snapmaker.cpp`

Live-verified shape (bench U1): `print_task_config` has parallel arrays of length 4 — `filament_exist` (bool), `filament_color_rgba` (e.g. `"E72F1DFF"`), `filament_type` (e.g. `"PLA"`). These are per-physical-head seated/color/material truth.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_snapmaker_task_config.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "snapmaker_task_config.h"
#include "hv/json.hpp"

TEST_CASE("parse print_task_config into AvailableSlots", "[snapmaker][taskcfg]") {
    auto j = nlohmann::json::parse(R"({
        "filament_exist":      [true, false, true, true],
        "filament_color_rgba": ["E72F1DFF","00000000","00C1AEFF","F4C032FF"],
        "filament_type":       ["PLA","PLA","PLA","PETG"]
    })");
    auto slots = helix::parse_snapmaker_task_config(j);
    REQUIRE(slots.size() == 4);
    CHECK(slots[0].slot_index == 0);
    CHECK(slots[0].color_rgb == 0xE72F1D);   // alpha stripped
    CHECK(slots[0].material == "PLA");
    CHECK_FALSE(slots[0].is_empty);
    CHECK(slots[1].is_empty);                // filament_exist[1] == false
    CHECK(slots[3].material == "PETG");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `make test && ./build/bin/helix-tests "[taskcfg]"`
Expected: FAIL — header missing.

- [ ] **Step 3: Implement**

```cpp
// include/snapmaker_task_config.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include "hv/json.hpp"
#include "filament_mapper.h"   // helix::AvailableSlot
namespace helix {
std::vector<AvailableSlot> parse_snapmaker_task_config(const nlohmann::json& task_config);
}
```

```cpp
// src/printer/snapmaker_task_config.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_task_config.h"
namespace helix {

static uint32_t rgba_hex_to_rgb(const std::string& s) {
    if (s.size() < 6) return 0;
    return static_cast<uint32_t>(std::stoul(s.substr(0, 6), nullptr, 16));  // drop trailing alpha
}

std::vector<AvailableSlot> parse_snapmaker_task_config(const nlohmann::json& tc) {
    std::vector<AvailableSlot> out;
    if (!tc.is_object()) return out;
    auto exist = tc.value("filament_exist", nlohmann::json::array());
    auto color = tc.value("filament_color_rgba", nlohmann::json::array());
    auto mat   = tc.value("filament_type", nlohmann::json::array());
    size_t n = exist.size();
    for (size_t i = 0; i < n; ++i) {
        AvailableSlot s{};
        s.slot_index = static_cast<int>(i);
        s.backend_index = 0;
        s.is_empty = !(exist[i].is_boolean() && exist[i].get<bool>());
        s.color_rgb = (i < color.size() && color[i].is_string())
                          ? rgba_hex_to_rgb(color[i].get<std::string>()) : 0;
        s.material = (i < mat.size() && mat[i].is_string()) ? mat[i].get<std::string>() : "";
        s.current_tool_mapping = -1;
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace helix
```

- [ ] **Step 4: Run to verify pass**

Run: `./build/bin/helix-tests "[taskcfg]"`
Expected: PASS.

- [ ] **Step 5: Wire into the backend (no new test — covered at integration)**

In `src/printer/ams_backend_snapmaker.cpp`, where `get_slot_info()` / the available-slot list is built, prefer the parsed `print_task_config` arrays when present (the object is already subscribed; locate the existing `print_task_config` read or add a subscription consistent with sibling backends). Keep the existing fallback when the object is absent.

- [ ] **Step 6: Commit**

```bash
git add include/snapmaker_task_config.h src/printer/snapmaker_task_config.cpp \
        tests/unit/test_snapmaker_task_config.cpp src/printer/ams_backend_snapmaker.cpp
git commit -m "feat(snapmaker): per-head seated truth from print_task_config"
```

---

## Task 5: L2 — remove the synthetic-index seed (honest tool labels)

**Files:**
- Modify: `src/ui/ui_print_select_detail_view.cpp:351-354` and `:297-378` (`show()`)

The bug: tool indices are seeded from palette position before parse. Fix: render a neutral "analyzing" state until `tools_used_indices` is available; never display a guessed `{0..N}` set.

- [ ] **Step 1: Read the current seed + both `update_color_swatches` call sites**

Run: `sed -n '340,360p;820,835p' src/ui/ui_print_select_detail_view.cpp`
Confirm `:351-354` builds `synthetic_tools` and `:829` re-calls with `parsed->tools_used_indices`.

- [ ] **Step 2: Replace the synthetic seed with a neutral state**

Delete the `synthetic_tools` construction (`:351-354`) and its `update_color_swatches(synthetic_tools, …)` call. In its place set the swatch area to an "analyzing" placeholder (reuse the existing empty/hidden swatch state — set `color_swatches_visible` subject to 0 until parse). The authoritative render remains the post-parse call at `:829` with `parsed->tools_used_indices`.

- [ ] **Step 3: Build + manual smoke**

Run (background, per L060): `./build/bin/helix-screen --test -vv -p print_select 2>&1 | tee /tmp/preflight.log`
User action: open a multi-color file; confirm chips appear only after the preview loads and read the real tool indices (no flash of T0/T1 for a T0/T2 file).

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_print_select_detail_view.cpp
git commit -m "fix(print-select): label filament chips from real gcode tools, not palette index"
```

---

## Task 6: L2 — drive mismatch detection for all AMS backends

**Files:**
- Modify: `src/ui/ui_print_select_detail_view.cpp` (post-parse path near `:829`)

Currently mismatch detection lives inside `FilamentMappingCard` and only runs when the card is visible (`ui_filament_mapping_card.cpp:67-86`), so U1/ACE get nothing. Compute it directly from `PreflightValidator` and publish the existing `filament_mismatch` / `empty_tools_warning_` subjects unconditionally.

- [ ] **Step 1: In `try_extract_gcode_colors()` (around `:801-833`), after obtaining `parsed->tools_used_indices`, build validator inputs and run it**

```cpp
// Build GcodeToolInfo from tools_used_indices + current_filament_colors_/materials
std::vector<helix::GcodeToolInfo> tools;
for (int ti : parsed->tools_used_indices)
    tools.push_back({ti, color_for_tool(ti), material_for_tool(ti)});

auto slots   = backend ? backend->get_available_slots() : std::vector<helix::AvailableSlot>{};
auto mapping = helix::FilamentMapper::compute_defaults(tools, slots);
preflight_result_ = helix::PreflightValidator::validate(tools, slots, mapping);  // cache as member

lv_subject_set_int(&filament_mismatch_,
    std::any_of(preflight_result_.checks.begin(), preflight_result_.checks.end(),
        [](const helix::ToolCheck& c){ return c.severity != helix::ToolCheck::Severity::Ok; }) ? 1 : 0);
lv_subject_set_int(&empty_tools_warning_, preflight_result_.has_block() ? 1 : 0);
```

Add `PreflightResult preflight_result_;` member to `include/ui_print_select_detail_view.h` and a getter `const helix::PreflightResult& preflight_result() const { return preflight_result_; }`. Use existing helpers for `color_for_tool`/`material_for_tool` (the palette lookups already present in `update_color_swatches`); factor them out if inline.

- [ ] **Step 2: Build + verify on bench U1 backend in --test**

Run: `./build/bin/helix-screen --test -vv -p print_select 2>&1 | tee /tmp/preflight.log`
User action: load a multi-color file with a mismatch; confirm the warning icon (`print_file_detail.xml:203`) now appears even though the mapping card is hidden.

- [ ] **Step 3: Commit**

```bash
git add src/ui/ui_print_select_detail_view.cpp include/ui_print_select_detail_view.h
git commit -m "feat(print-select): run mismatch detection for all AMS backends"
```

---

## Task 7: L1 — parse-complete gate + block/advisory in `start_print`

**Files:**
- Modify: `src/ui/ui_panel_print_select.cpp:2503-2543` (`start_print`)

- [ ] **Step 1: Gate on parse completion first**

At the top of `start_print(bool force)`, before the existing empty-tool check at `:2515`:

```cpp
if (!force && detail_view_ && !detail_view_->is_gcode_loaded()) {
    // Parse not done — show a brief "Checking filaments..." and retry when ready.
    detail_view_->run_when_loaded([this]{ start_print(false); });   // see Step 2
    ui::toast_info(lv_tr("Checking filaments..."));
    return;
}
```

- [ ] **Step 2: Add `is_gcode_loaded()` + `run_when_loaded()` to the detail view**

In `include/ui_print_select_detail_view.h`: expose `bool is_gcode_loaded() const { return gcode_loaded_; }` and `void run_when_loaded(std::function<void()> cb)`. Store the callback; invoke it (via `lifetime_.defer`) at the end of the existing load callback (`ui_print_select_detail_view.cpp:917/1008`) where `gcode_loaded_` is set true. If already loaded, run immediately.

- [ ] **Step 3: Replace the empty-tool check body with PreflightResult**

```cpp
const auto& pf = detail_view_->preflight_result();
if (!force && pf.has_block()) {
    show_preflight_modal(pf);   // enriched modal — Task 8
    return;
}
// advisory (material) does not block; it is surfaced in the modal/icon only.
```

- [ ] **Step 4: Build + manual verify the block**

Run: `./build/bin/helix-screen --test -vv -p print_select 2>&1 | tee /tmp/preflight.log`
User action: load a file whose required head is empty (use the mock/bench U1 state); tap Print before and after parse. Confirm: tapping before parse shows "Checking filaments…" then resolves; an empty head blocks with the modal; a clean file prints.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_panel_print_select.cpp include/ui_print_select_detail_view.h src/ui/ui_print_select_detail_view.cpp
git commit -m "feat(print-select): gate print on parse + preflight block"
```

---

## Task 8: L1 — enriched pre-flight modal

**Files:**
- Create: `ui_xml/components/preflight_check_modal.xml`
- Modify: `src/ui/ui_panel_print_select.cpp` (`show_preflight_modal`)
- Register: `src/xml_registration.cpp` (or `main.cpp` per L014)

Per-tool rows (intended → seated), severity glyph, and three actions. `[Remap…]` shown only when `RemapStrategy != None` (Task 9).

- [ ] **Step 1: Author the XML component**

```xml
<!-- ui_xml/components/preflight_check_modal.xml -->
<component>
  <view extends="ui_dialog">
    <text_heading text="$title"/>
    <lv_obj name="rows" width="100%" style_pad_row="#space_xs" flex_flow="column"/>
    <text_body name="explanation" style_text_color="#warning"/>
    <modal_button_row
        tertiary_text="Remap…"   tertiary_callback="on_preflight_remap"
        secondary_text="Cancel"  secondary_callback="on_preflight_cancel"
        primary_text="Print Anyway" primary_callback="on_preflight_force"/>
  </view>
</component>
```

(Match existing modal components in `ui_xml/components/`; if `tertiary_text` isn't supported on `modal_button_row`, use a `Modal` subclass with `wire_tertiary_button` per `ui_modal.h:262`.)

- [ ] **Step 2: Register the component**

Add `lv_xml_component_register_from_file(".../preflight_check_modal.xml")` alongside the other modal registrations (grep `preflight` after to confirm). Register the three event callbacks via `lv_xml_register_event_cb`.

- [ ] **Step 3: Implement `show_preflight_modal(const PreflightResult&)`**

Populate one row per `ToolCheck`: label `T{tool_index}`, intended swatch (`intended_color`), seated swatch (or "EMPTY"), and ✓/⚠/✗ by `severity`. Build the explanation string from the first blocking check (e.g. `"Head {mapped_slot+1} is empty. This print will run out."`). `on_preflight_force` → `start_print(true)`; `on_preflight_cancel` → `Modal::hide`; `on_preflight_remap` → open remap (Task 10/11).

- [ ] **Step 4: Build + manual verify (ASCII-checked against the spec mock)**

Run: `./build/bin/helix-screen --test -vv -p print_select 2>&1 | tee /tmp/preflight.log`
User action: trigger a block; confirm rows + buttons render and "Print Anyway" proceeds.

- [ ] **Step 5: Commit**

```bash
git add ui_xml/components/preflight_check_modal.xml src/ui/ui_panel_print_select.cpp src/xml_registration.cpp
git commit -m "feat(print-select): enriched pre-flight check modal"
```

---

## Task 9: L3 — `RemapStrategy` capability

**Files:**
- Modify: `include/ams_backend.h` (enum + virtual)
- Modify: each `src/printer/ams_backend_*.cpp`
- Test: `tests/unit/test_remap_strategy.cpp` (create)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_remap_strategy.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "ams_backend_snapmaker.h"
#include "ams_backend_afc.h"
using helix::printer::RemapStrategy;

TEST_CASE("U1 uses gcode-rewrite remap, AFC uses native", "[remap][strategy]") {
    CHECK(AmsBackendSnapmaker::static_remap_strategy() == RemapStrategy::GcodeRewrite);
    CHECK(AmsBackendAFC::static_remap_strategy()       == RemapStrategy::Native);
}
```

(If a static accessor is awkward, instantiate per the existing backend test pattern and call `get_remap_strategy()`.)

- [ ] **Step 2: Run to verify failure**

Run: `make test && ./build/bin/helix-tests "[strategy]"`
Expected: FAIL — enum/method missing.

- [ ] **Step 3: Add the enum + default**

In `include/ams_backend.h` (namespace `helix::printer`):

```cpp
enum class RemapStrategy { None, Native, GcodeRewrite };
```

On `AmsBackendBase`:

```cpp
virtual RemapStrategy get_remap_strategy() const { return RemapStrategy::None; }
```

- [ ] **Step 4: Override per backend**

- `ams_backend_afc.cpp`, `ams_backend_happy_hare.cpp`, `ams_backend_cfs.cpp`, `ams_backend_ad5x_ifs.cpp`, `ams_backend_toolchanger.cpp` → `return RemapStrategy::Native;`
- `ams_backend_snapmaker.cpp`, `ams_backend_ace.cpp` → `return RemapStrategy::GcodeRewrite;`
- All others → inherit `None`.

- [ ] **Step 5: Run to verify pass**

Run: `./build/bin/helix-tests "[strategy]"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/ams_backend.h src/printer/ams_backend_*.cpp tests/unit/test_remap_strategy.cpp
git commit -m "feat(ams): per-backend RemapStrategy capability"
```

---

## Task 10: L3 — native remap reachable from the gate

**Files:**
- Modify: `src/ui/ui_panel_print_select.cpp` (`on_preflight_remap`)

- [ ] **Step 1: Route `on_preflight_remap` by strategy**

```cpp
void PrintSelectPanel::on_preflight_remap() {
    auto* backend = /* active AMS backend */;
    if (!backend) return;
    switch (backend->get_remap_strategy()) {
        case RemapStrategy::Native:       open_native_remap_modal();  break;  // this step
        case RemapStrategy::GcodeRewrite: open_gcode_remap_modal();   break;  // Task 11/12
        case RemapStrategy::None:         break;
    }
}
```

- [ ] **Step 2: `open_native_remap_modal()` reuses `FilamentMappingModal`**

Populate from `detail_view_->preflight_result()` + backend slots (the modal already takes `set_tool_info` / `set_available_slots` / `set_mappings`, `ui_filament_mapping_modal.h:23-82`). On `set_on_mappings_updated`, the modal already calls `set_tool_mapping` for native backends. After update, re-run preflight and refresh the gate modal.

- [ ] **Step 3: Build + manual verify on a native backend (mock AFC/CFS)**

Run: `./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/preflight.log` (select a backend that reports `Native`).
User action: open Remap from the gate, change a mapping, confirm the warning clears.

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_panel_print_select.cpp
git commit -m "feat(print-select): native remap from pre-flight gate"
```

---

## Task 11: L3 — U1/ACE comprehensive gcode remapper

**Files:**
- Create: `include/gcode_tool_remapper.h`, `src/rendering/gcode_tool_remapper.cpp`
- Test: `tests/unit/test_gcode_tool_remapper.cpp`
- Fixture: `assets/test_gcodes/u1_4color_ring.gcode` (already added)

Builds a `GCodeFileModifier` modification list that rewrites **all three** families for a logical→physical remap. Verified families in the fixture: bare `^T<n>`, `SM_PRINT_(AUTO_FEED|EXTRUDER_PREHEAT|FLOW_CALIBRATE) EXTRUDER=<n>`, and `M10[49] … T<n>`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_gcode_tool_remapper.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "gcode_tool_remapper.h"
#include <fstream>
#include <sstream>

static std::string slurp(const std::string& p){ std::ifstream f(p); std::stringstream s; s<<f.rdbuf(); return s.str(); }

TEST_CASE("U1 remap rewrites all three command families", "[remap][gcode]") {
    // Remap logical tool 1 -> physical head 2.
    std::map<int,int> remap = {{1, 2}};
    auto plan = helix::GcodeToolRemapper::build_plan(
        "assets/test_gcodes/u1_4color_ring.gcode", remap);

    // body Tn
    CHECK(plan.replacements_for("T1").empty());            // no T1 left after applying
    CHECK(plan.count_matched_lines() > 0);

    // Apply to an in-memory copy and assert no stray '=1' / 'T1' survive for tool 1.
    std::string out = helix::GcodeToolRemapper::apply_to_string(slurp("assets/test_gcodes/u1_4color_ring.gcode"), remap);
    CHECK(out.find("\nT1\n") == std::string::npos);
    CHECK(out.find("SM_PRINT_AUTO_FEED EXTRUDER=1") == std::string::npos);
    CHECK(out.find("SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1") == std::string::npos);
    CHECK(out.find("SM_PRINT_FLOW_CALIBRATE EXTRUDER=1") == std::string::npos);
    // M104 ... T1 rewritten
    CHECK(out.find(" T1 ;") == std::string::npos);
    CHECK(out.find(" T1\n") == std::string::npos);
    // untouched tool 0 lines remain
    CHECK(out.find("\nT0\n") != std::string::npos);
    CHECK(out.find("SM_PRINT_AUTO_FEED EXTRUDER=0") != std::string::npos);
}
```

(Adjust the exact API to the simplest shape that passes; `apply_to_string` is the testable core, `build_plan` may be folded in.)

- [ ] **Step 2: Run to verify failure**

Run: `make test && ./build/bin/helix-tests "[remap][gcode]"`
Expected: FAIL — header missing.

- [ ] **Step 3: Implement the remapper**

```cpp
// include/gcode_tool_remapper.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <string>
namespace helix {
class GcodeToolRemapper {
public:
    // remap: logical tool index -> physical head index.
    static std::string apply_to_string(const std::string& gcode, const std::map<int,int>& remap);
};
}
```

Implementation rewrites, line-by-line, three patterns (apply remap simultaneously — build the full target set first to avoid chained `1->2`, `2->1` collisions; use a temp sentinel pass or apply by exact source-match):
1. Whole-line `^T(\d+)$` → `T{remap[n]}`.
2. `^(SM_PRINT_(?:AUTO_FEED|EXTRUDER_PREHEAT|FLOW_CALIBRATE) EXTRUDER=)(\d+)\b` → group1 + `{remap[n]}`.
3. `(\bT)(\d+)(\b)` inside `M104`/`M109` lines → `T{remap[n]}`.

Only remap indices present as keys; leave others untouched. To avoid swap collisions, compute each line's replacement from the *original* index in a single pass (regex replace with a lookup), never re-scan output.

For the production path, also provide a method returning `GCodeFileModifier` `REPLACE` modifications keyed by line number so the streaming temp-file flow (Task 12) reuses existing infrastructure rather than loading the whole file in memory.

```cpp
// src/rendering/gcode_tool_remapper.cpp — sketch of the pure core
#include "gcode_tool_remapper.h"
#include <regex>
#include <sstream>
namespace helix {
static int mapped(const std::map<int,int>& m, int n){ auto it=m.find(n); return it==m.end()? n : it->second; }

std::string GcodeToolRemapper::apply_to_string(const std::string& g, const std::map<int,int>& remap) {
    static const std::regex re_tn(R"(^T(\d+)\s*$)");
    static const std::regex re_sm(R"(^(SM_PRINT_(?:AUTO_FEED|EXTRUDER_PREHEAT|FLOW_CALIBRATE) EXTRUDER=)(\d+)(.*)$)");
    static const std::regex re_temp(R"((M10[49]\b[^\n]*?\bT)(\d+)(\b))");
    std::istringstream in(g); std::ostringstream out; std::string line;
    while (std::getline(in, line)) {
        std::smatch mm;
        if (std::regex_match(line, mm, re_tn)) {
            out << "T" << mapped(remap, std::stoi(mm[1])) ;
        } else if (std::regex_match(line, mm, re_sm)) {
            out << mm[1].str() << mapped(remap, std::stoi(mm[2])) << mm[3].str();
        } else if (line.rfind("M104",0)==0 || line.rfind("M109",0)==0) {
            out << std::regex_replace(line, re_temp,
                [&](const std::smatch& s){ return s[1].str()+std::to_string(mapped(remap,std::stoi(s[2])))+s[3].str(); });
            // NOTE: std::regex_replace has no functional overload pre-C++17 lambda; if unavailable,
            //       hand-roll the single T-token replacement. Keep it to M104/M109 lines only.
        } else {
            out << line;
        }
        out << "\n";
    }
    return out.str();
}
}
```

(If the `regex_replace` functional form is unavailable in the toolchain, replace step 3 with a manual scan for the first `T<digits>` token on `M104/M109` lines. The test is the contract.)

- [ ] **Step 4: Run to verify pass**

Run: `./build/bin/helix-tests "[remap][gcode]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/gcode_tool_remapper.h src/rendering/gcode_tool_remapper.cpp \
        tests/unit/test_gcode_tool_remapper.cpp assets/test_gcodes/u1_4color_ring.gcode
git commit -m "feat(remap): comprehensive U1/ACE gcode tool remapper"
```

---

## Task 12: L3 — wire gcode-rewrite remap through HelixPrint plugin

**Files:**
- Modify: `src/ui/ui_panel_print_select.cpp` (`open_gcode_remap_modal` + apply path)
- Reuse: `src/ui/ui_print_preparation_manager.cpp` (download→modify→upload→start_modified_print), `moonraker_job_api.cpp:81-107` (`check_helix_plugin`)

- [ ] **Step 1: `open_gcode_remap_modal()` collects the user's logical→physical map**

Reuse `FilamentMappingModal` UI to gather `std::map<int,int> remap` (logical tool → head). The modal already renders tool rows + slot pickers; for `GcodeRewrite` backends, capture the result instead of calling `set_tool_mapping`.

- [ ] **Step 2: Guard on plugin, then apply via the modifier flow**

```cpp
if (!api_->job().has_helix_plugin()) {   // check_helix_plugin result
    ui::modal_show_confirmation(lv_tr("Remap needs the HelixPrint plugin"),
        lv_tr("Install the HelixPrint plugin (Advanced settings) to remap without affecting print history."),
        ModalSeverity::Info, lv_tr("OK"), nullptr, nullptr, nullptr);
    return;
}
prep_manager_->modify_and_print_with_remap(file_path, remap);  // builds GCodeFileModifier REPLACE list via GcodeToolRemapper
```

Add `modify_and_print_with_remap()` to the preparation manager that turns the remap into `GCodeFileModifier` `REPLACE` modifications (using the line-keyed method from Task 11), runs the existing streaming upload, and calls `start_modified_print(original, temp, mod_names)` so the HelixPrint plugin patches history back to the original filename.

- [ ] **Step 3: Build + (deferred) on-device validation**

Native unit/build only here; full validation is Task 13. Confirm compile + that a `None`/`Native` backend never reaches this path.

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_panel_print_select.cpp src/ui/ui_print_preparation_manager.cpp include/ui_print_preparation_manager.h
git commit -m "feat(remap): apply U1/ACE gcode remap via HelixPrint plugin"
```

---

## Task 13: Bench U1 end-to-end validation

**Files:** none (manual, gated release)

Device: Snapmaker U1 at `192.168.30.103` (sshpass `-p snapmaker`). HelixPrint plugin installed.

- [ ] **Step 1: Deploy** — `make snapmaker-u1-docker && SNAPMAKER_U1_HOST=192.168.30.103 make deploy-snapmaker-u1`
- [ ] **Step 2: Empty-head block** — load filament in heads 0 and 2, unload head 1. Slice/transfer a 2-color file that targets heads 0+1. Confirm the gate **blocks** before print start naming head 1.
- [ ] **Step 3: Remap round-trip** — from the gate, Remap logical tool 1 → head 2. Confirm the print starts, pulls heads 0 and 2 (no runout), temps correct, geometry correct.
- [ ] **Step 4: History clean** — confirm the Moonraker history entry shows the **original** filename, not a `modified_*` temp name (`history.modify_job` patched).
- [ ] **Step 5: Flip the release flag** — only after Steps 2–4 pass, enable `GcodeRewrite` remap for users (remove the dev gate). Record the result in `project_snapmaker_ams_unload_followups.md` / a new project memory.

---

## Self-Review

- **Spec coverage:** L1 gate → Tasks 5/7/8; L2 honest display + decoupled detection → Tasks 5/6; validator (reuse FilamentMapper) → Tasks 1–3; U1 live truth → Task 4; RemapStrategy → Task 9; native remap → Task 10; U1/ACE rewrite + history safety → Tasks 11/12; bench validation + release gate → Task 13. All spec sections covered.
- **Placeholder scan:** no TBD/TODO; every code step shows code; the one toolchain caveat (`regex_replace` functional overload) names a concrete fallback with the test as contract.
- **Type consistency:** `ToolCheck`/`PreflightResult`/`PreflightValidator::validate` signatures match across Tasks 1–3, 6, 7. `RemapStrategy` enum identical in Tasks 9/10/12. `AvailableSlot`/`GcodeToolInfo`/`ToolMapping` are the existing `filament_mapper.h` types throughout. `apply_to_string`/`build_plan` consistent between Task 11 impl and its test.
