# Slot Component Designs

**Purpose**: Slot-based components to reduce XML verbosity using LVGL's slot support (available in `lib/helix-xml/`).
**Status**: Ready to implement — slot support included in helix-xml extraction (commit `f38718108`)
**Plan**: `docs/devel/plans/2026-02-18-helix-xml-plan.md` (Phase 3)
**Estimated Savings**: ~170+ lines across 4 component patterns

---

## 1. Z-Offset Button with Kinematics-Aware Icons

### Current Pattern (Verbose)
**File**: `ui_xml/print_tune_panel.xml:77-177`
**Repetitions**: 6 buttons, each with 12+ lines of identical dual-icon logic

```xml
<!-- CURRENT: Repeated 6 times with only button name and text changing -->
<lv_button name="btn_z_closer_01"
           height="content" flex_grow="1" style_radius="#border_radius" style_border_width="0"
           flex_flow="row" style_flex_main_place="center" style_flex_cross_place="center"
           style_pad_gap="#space_xs">
  <event_cb trigger="clicked" callback="on_tune_z_offset"/>
  <!-- Dual icons: show based on printer kinematics -->
  <lv_obj width="content" height="content" style_bg_opa="0" style_border_width="0" style_pad_all="0">
    <icon src="arrow_expand_up" size="sm">
      <bind_flag_if_eq subject="printer_bed_moves" flag="hidden" ref_value="0"/>
    </icon>
    <icon src="arrow_down" size="sm">
      <bind_flag_if_eq subject="printer_bed_moves" flag="hidden" ref_value="1"/>
    </icon>
  </lv_obj>
  <text_small text="0.1" align="center"/>
</lv_button>
```

### Proposed Slot Component

**New File**: `ui_xml/z_offset_button.xml`

```xml
<?xml version="1.0"?>
<component>
  <api>
    <prop name="name" type="string"/>
    <prop name="step" type="string"/>
    <prop name="direction" type="string" default="closer"/>  <!-- "closer" or "farther" -->
  </api>
  <view extends="lv_button"
        name="$name" height="content" flex_grow="1"
        style_radius="#border_radius" style_border_width="0"
        flex_flow="row" style_flex_main_place="center" style_flex_cross_place="center"
        style_pad_gap="#space_xs">
    <event_cb trigger="clicked" callback="on_tune_z_offset"/>
    <!-- SLOT: icon - filled by parent based on direction -->
    <lv_obj name="icon_slot" width="content" height="content"
            style_bg_opa="0" style_border_width="0" style_pad_all="0"/>
    <text_small text="$step" align="center"/>
  </view>
</component>
```

### Usage with Slots (LVGL 9.5)

```xml
<!-- LVGL 9.5: Use slot to inject kinematics-aware icons -->
<z_offset_button name="btn_z_closer_01" step="0.1" direction="closer">
  <z_offset_button-icon_slot>
    <icon src="arrow_expand_up" size="sm">
      <bind_flag_if_eq subject="printer_bed_moves" flag="hidden" ref_value="0"/>
    </icon>
    <icon src="arrow_down" size="sm">
      <bind_flag_if_eq subject="printer_bed_moves" flag="hidden" ref_value="1"/>
    </icon>
  </z_offset_button-icon_slot>
</z_offset_button>
```

### Alternative: Icon Set Component

Even better - extract the dual-icon pattern itself:

**New File**: `ui_xml/kinematics_icon.xml`

```xml
<?xml version="1.0"?>
<component>
  <api>
    <prop name="bed_moves_icon" type="string"/>   <!-- Icon when bed moves (cartesian/corexy) -->
    <prop name="head_moves_icon" type="string"/>  <!-- Icon when head moves (delta) -->
    <prop name="size" type="string" default="sm"/>
  </api>
  <view extends="lv_obj" width="content" height="content"
        style_bg_opa="0" style_border_width="0" style_pad_all="0">
    <icon src="$bed_moves_icon" size="$size">
      <bind_flag_if_eq subject="printer_bed_moves" flag="hidden" ref_value="0"/>
    </icon>
    <icon src="$head_moves_icon" size="$size">
      <bind_flag_if_eq subject="printer_bed_moves" flag="hidden" ref_value="1"/>
    </icon>
  </view>
</component>
```

### Final Usage (Most Concise)

```xml
<z_offset_button name="btn_z_closer_01" step="0.1">
  <z_offset_button-icon_slot>
    <kinematics_icon bed_moves_icon="arrow_expand_up" head_moves_icon="arrow_down"/>
  </z_offset_button-icon_slot>
</z_offset_button>
```

**Lines Saved**: ~50 lines (6 buttons × 8 lines of duplication)

---

## 2. State-Mapped Icon Switcher

### Current Pattern (Verbose)
**File**: `ui_xml/home_panel.xml:178-233`
**Issue**: 6 separate `<icon>` elements, each with its own `bind_flag_if_not_eq`

```xml
<!-- CURRENT: Network state icons - 6 icons for 6 states -->
<lv_button name="network_btn" ...>
  <!-- Disconnected (state 0) -->
  <icon name="net_disconnected" src="wifi_off" size="#icon_size" variant="disabled">
    <lv_obj-bind_flag_if_not_eq subject="home_network_icon_state" flag="hidden" ref_value="0"/>
  </icon>
  <!-- WiFi strength 1 (state 1) -->
  <icon name="net_wifi_1" src="wifi_strength_1_alert" size="#icon_size" variant="warning">
    <lv_obj-bind_flag_if_not_eq subject="home_network_icon_state" flag="hidden" ref_value="1"/>
  </icon>
  <!-- ... 4 more icons for states 2-5 ... -->
</lv_button>
```

### Proposed Slot Component

**New File**: `ui_xml/state_icon.xml`

```xml
<?xml version="1.0"?>
<component>
  <api>
    <prop name="subject" type="string"/>
    <prop name="size" type="string" default="#icon_size"/>
  </api>
  <view extends="lv_obj" width="content" height="content"
        style_bg_opa="0" style_border_width="0" style_pad_all="0">
    <!-- Slots for each state value -->
    <!-- Parent provides state_0, state_1, etc. slots -->
  </view>
</component>
```

### Usage with Slots (LVGL 9.5)

```xml
<state_icon subject="home_network_icon_state" size="#icon_size">
  <state_icon-state_0>
    <icon src="wifi_off" variant="disabled"/>
  </state_icon-state_0>
  <state_icon-state_1>
    <icon src="wifi_strength_1_alert" variant="warning"/>
  </state_icon-state_1>
  <state_icon-state_2>
    <icon src="wifi_strength_2" variant="secondary"/>
  </state_icon-state_2>
  <state_icon-state_3>
    <icon src="wifi_strength_3" variant="secondary"/>
  </state_icon-state_3>
  <state_icon-state_4>
    <icon src="wifi_strength_4" variant="secondary"/>
  </state_icon-state_4>
  <state_icon-state_5>
    <icon src="ethernet" variant="success"/>
  </state_icon-state_5>
</state_icon>
```

### Alternative: Icon Map via Props

If LVGL 9.5 doesn't support dynamic slot generation, use a prop-based approach:

```xml
<!-- Declarative mapping - one line per state -->
<state_icon subject="home_network_icon_state"
            icons="wifi_off,wifi_strength_1_alert,wifi_strength_2,wifi_strength_3,wifi_strength_4,ethernet"
            variants="disabled,warning,secondary,secondary,secondary,success"/>
```

**Lines Saved**: ~30-40 lines per state icon usage

---

## 3. Conditional Row Wrapper

### Current Pattern (Verbose)
**File**: `ui_xml/settings_panel.xml:32-103`
**Issue**: 5 rows with identical wrapper pattern for capability checks

```xml
<!-- CURRENT: Repeated wrapper pattern -->
<lv_obj name="container_filament_sensors"
        width="100%" height="content" style_bg_opa="0" style_border_width="0" style_pad_all="0" scrollable="false">
  <lv_obj-bind_flag_if_eq subject="filament_sensor_count" flag="hidden" ref_value="0"/>
  <setting_action_row name="row_filament_sensors"
                      label="Filament Sensors" icon="filament"
                      description="Configure filament detection sensors"
                      callback="on_filament_sensors_clicked"/>
</lv_obj>

<lv_obj name="container_led_light"
        width="100%" height="content" style_bg_opa="0" style_border_width="0" style_pad_all="0" scrollable="false">
  <lv_obj-bind_flag_if_eq subject="printer_has_led" flag="hidden" ref_value="0"/>
  <setting_toggle_row name="row_led_light" .../>
</lv_obj>

<!-- ... 3 more identical patterns ... -->
```

### Proposed Slot Component

**New File**: `ui_xml/conditional_container.xml`

```xml
<?xml version="1.0"?>
<component>
  <api>
    <prop name="name" type="string"/>
    <prop name="subject" type="string"/>
    <prop name="hide_when" type="string" default="0"/>  <!-- Value that triggers hidden -->
  </api>
  <view extends="lv_obj" name="$name"
        width="100%" height="content"
        style_bg_opa="0" style_border_width="0" style_pad_all="0" scrollable="false">
    <lv_obj-bind_flag_if_eq subject="$subject" flag="hidden" ref_value="$hide_when"/>
    <!-- SLOT: content - filled by parent -->
  </view>
</component>
```

### Usage with Slots (LVGL 9.5)

```xml
<!-- LVGL 9.5: Conditional wrapper with slot for content -->
<conditional_container name="container_filament_sensors" subject="filament_sensor_count" hide_when="0">
  <conditional_container-content>
    <setting_action_row name="row_filament_sensors"
                        label="Filament Sensors" icon="filament"
                        description="Configure filament detection sensors"
                        callback="on_filament_sensors_clicked"/>
  </conditional_container-content>
</conditional_container>

<conditional_container name="container_led_light" subject="printer_has_led" hide_when="0">
  <conditional_container-content>
    <setting_toggle_row name="row_led_light" label="LED Light" icon="light"
                        description="Toggle printer LED strip" callback="on_led_light_changed"/>
  </conditional_container-content>
</conditional_container>
```

**Lines Saved**: ~20 lines (5 rows × 4 lines of wrapper boilerplate)

---

## 4. Tune Card with Slot Body

### Current Pattern (Verbose)
**File**: `ui_xml/print_tune_panel.xml:17-45`
**Issue**: Speed and Flow cards are nearly identical structures

```xml
<!-- CURRENT: Speed card (repeated structure for Flow card) -->
<ui_card name="speed_card"
         height="content" flex_grow="1" style_radius="#border_radius" flex_flow="column"
         style_pad_all="#space_sm" style_pad_gap="#space_xs" flag_overflow_visible="true">
  <text_small text="Print Speed"/>
  <text_heading name="speed_display" bind_text="tune_speed_display"/>
  <lv_obj name="speed_slider_container"
          width="100%" height="content" style_bg_opa="0" style_border_width="0" style_pad_all="0"
          scrollable="false" flex_flow="row" style_flex_cross_place="center" flag_overflow_visible="true">
    <lv_slider name="speed_slider" flex_grow="1" min_value="50" max_value="200" value="100">
      <event_cb trigger="value_changed" callback="on_tune_speed_changed"/>
    </lv_slider>
  </lv_obj>
  <text_small text="Movement speed" style_text_color="#text_muted"/>
</ui_card>
```

### Proposed Slot Component

**New File**: `ui_xml/tune_slider_card.xml`

```xml
<?xml version="1.0"?>
<component>
  <api>
    <prop name="name" type="string"/>
    <prop name="title" type="string"/>
    <prop name="description" type="string"/>
    <prop name="display_subject" type="string"/>
    <prop name="slider_name" type="string"/>
    <prop name="slider_min" type="number" default="0"/>
    <prop name="slider_max" type="number" default="100"/>
    <prop name="slider_value" type="number" default="50"/>
    <prop name="slider_callback" type="string"/>
  </api>
  <view extends="ui_card" name="$name"
        height="content" flex_grow="1" style_radius="#border_radius" flex_flow="column"
        style_pad_all="#space_sm" style_pad_gap="#space_xs" flag_overflow_visible="true">
    <text_small text="$title"/>
    <text_heading name="${name}_display" bind_text="$display_subject"/>
    <lv_obj name="${name}_slider_container"
            width="100%" height="content" style_bg_opa="0" style_border_width="0" style_pad_all="0"
            scrollable="false" flex_flow="row" style_flex_cross_place="center" flag_overflow_visible="true">
      <lv_slider name="$slider_name" flex_grow="1"
                 min_value="$slider_min" max_value="$slider_max" value="$slider_value">
        <event_cb trigger="value_changed" callback="$slider_callback"/>
      </lv_slider>
    </lv_obj>
    <text_small text="$description" style_text_color="#text_muted"/>
  </view>
</component>
```

### Usage (No Slots Needed - Props Sufficient)

```xml
<tune_slider_card name="speed_card" title="Print Speed" description="Movement speed"
                  display_subject="tune_speed_display"
                  slider_name="speed_slider" slider_min="50" slider_max="200" slider_value="100"
                  slider_callback="on_tune_speed_changed"/>

<tune_slider_card name="flow_card" title="Flow Rate" description="Extrusion rate"
                  display_subject="tune_flow_display"
                  slider_name="flow_slider" slider_min="75" slider_max="125" slider_value="100"
                  slider_callback="on_tune_flow_changed"/>
```

**Lines Saved**: ~15 lines per card (10 lines × 2 cards - some complexity moved to component)

---

## Summary: Expected Line Savings

| Pattern | Current Lines | New Lines | Saved | Files Affected |
|---------|--------------|-----------|-------|----------------|
| Z-offset buttons | ~100 | ~50 | ~50 | `print_tune_panel.xml` |
| Network state icons | ~55 | ~20 | ~35 | `home_panel.xml` |
| Printer state icons | ~20 | ~8 | ~12 | `home_panel.xml` |
| Conditional wrappers | ~35 | ~15 | ~20 | `settings_panel.xml` |
| Tune slider cards | ~30 | ~8 | ~22 | `print_tune_panel.xml` |
| **Total** | **~240** | **~101** | **~139** | 3 files |

---

## Implementation Plan

### Phase 1: Create New Components (No Breaking Changes)
1. Create `kinematics_icon.xml` - usable immediately on 9.4
2. Create `tune_slider_card.xml` - usable immediately on 9.4
3. Create `conditional_container.xml` - usable immediately on 9.4

### Phase 2: Adopt Slots (After LVGL 9.5.1)
1. Add slot support to `z_offset_button.xml`
2. Add slot support to `state_icon.xml`
3. Migrate existing XML to use slot syntax

### Phase 3: Cleanup
1. Remove duplicate patterns from panel XML files
2. Update documentation

---

---

## String Subject Audit for Formula Migration

**File**: `src/ui/ui_panel_print_status.cpp:198-247`
**Purpose**: Identify subjects that format values in C++ and could move to XML formulas

### High-Value Candidates (Simple Arithmetic + String Concat)

| Subject Name | Current Format | Formula Requirement | Feasibility |
|--------------|----------------|---------------------|-------------|
| `print_progress_text` | `"%d%%"` | Integer + "%" suffix | ✅ HIGH |
| `print_speed_text` | `"%d%%"` | Integer + "%" suffix | ✅ HIGH |
| `print_flow_text` | `"%d%%"` | Integer + "%" suffix | ✅ HIGH |
| `tune_speed_display` | `"%d%%"` | Integer + "%" suffix | ✅ HIGH |
| `tune_flow_display` | `"%d%%"` | Integer + "%" suffix | ✅ HIGH |
| `tune_z_offset_display` | `"%.3fmm"` | Float + "mm" suffix | ✅ HIGH |

**Example C++ Code** (`ui_panel_print_status.cpp:713`):
```cpp
std::snprintf(progress_text_buf_, sizeof(progress_text_buf_), "%d%%", current_progress_);
```

**Formula Equivalent** (if string concat supported):
```xml
<text_body bind_text="=(print_progress) + '%'"/>
```

### Medium-Value Candidates (Time Formatting)

| Subject Name | Current Format | Formula Requirement | Feasibility |
|--------------|----------------|---------------------|-------------|
| `print_elapsed` | `"%dh %02dm"` | Integer division + zero-padded minutes | ⚠️ MEDIUM |
| `print_remaining` | `"%dh %02dm"` | Integer division + zero-padded minutes | ⚠️ MEDIUM |

**C++ Code** (`ui_panel_print_status.cpp:553-557`):
```cpp
void PrintStatusPanel::format_time(int seconds, char* buf, size_t buf_size) {
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    std::snprintf(buf, buf_size, "%dh %02dm", hours, minutes);
}
```

**Formula Equivalent** (requires zero-padding function):
```xml
<text_body bind_text="=(print_duration / 3600) + 'h ' + pad((print_duration % 3600) / 60, 2) + 'm'"/>
```

**Blockers**: Need `pad()` or `zeropad()` function for "02" formatting.

### Medium-Value Candidates (Temp with Target)

| Subject Name | Current Format | Formula Requirement | Feasibility |
|--------------|----------------|---------------------|-------------|
| `nozzle_temp_text` | `"%d / %d°C"` or `"%d / --"` | Conditional + decidegree division | ⚠️ MEDIUM |
| `bed_temp_text` | `"%d / %d°C"` or `"%d / --"` | Conditional + decidegree division | ⚠️ MEDIUM |

**C++ Code** (`ui_panel_print_status.cpp:729-742`):
```cpp
// Note: Temps stored as decidegrees, divided by 100 for display
if (target > 0) {
    std::snprintf(nozzle_temp_buf_, sizeof(nozzle_temp_buf_), "%d / %d°C",
                  current / 100, target / 100);
} else {
    std::snprintf(nozzle_temp_buf_, sizeof(nozzle_temp_buf_), "%d / --",
                  current / 100);
}
```

**Formula Equivalent** (requires conditional):
```xml
<!-- Would need if() or ternary operator support -->
<text_body bind_text="=(extruder_temp / 100) + ' / ' + if(extruder_target > 0, (extruder_target / 100) + '°C', '--')"/>
```

**Blockers**: Need conditional/ternary operator in formulas.

### Low-Value Candidates (String Literals / Icons)

| Subject Name | Current Use | Why Not Formula |
|--------------|-------------|-----------------|
| `print_layer_text` | `"Layer %d / %d"` | Localization concerns |
| `pause_button_icon` | UTF-8 icon bytes | Icon switching is state-based, not calculation |
| `pause_button_label` | "Pause"/"Resume" | State-based, not calculated |
| `timelapse_button_icon` | UTF-8 icon bytes | State-based |
| `timelapse_label` | "On"/"Off" | State-based |
| `light_button_icon` | UTF-8 icon bytes | State-based |
| `preparing_operation` | Static text | Progress message, not calculated |

### Formula Capability Matrix

| Capability | Required For | LVGL 9.5 Status |
|------------|--------------|-----------------|
| Integer arithmetic (+, -, *, /) | All % formats | ✅ Confirmed |
| Modulo (%) | Time formatting | ❓ Unknown |
| String concatenation (+) | Suffix attachment | ❓ Unknown |
| Zero-padding / pad() | Time minutes | ❓ Unknown |
| Conditional / ternary | Temp "--" fallback | ❓ Unknown |
| Subject binding | Reactive values | ❓ Unknown |

### Migration Strategy

**Phase 1: Low-Hanging Fruit** (After formula capability confirmed)
- Migrate 6 percentage subjects (`*_text`, `*_display`) - all use same pattern
- Raw integer subjects already exist, just need `+ '%'` suffix

**Phase 2: Time Formatting** (If modulo + padding supported)
- Create raw `print_duration_seconds` and `print_remaining_seconds` subjects
- Use formula for hour/minute calculation

**Phase 3: Temperature** (If conditionals supported)
- Expose `extruder_temp` and `extruder_target` as raw decidegree subjects
- Use formula with conditional for target display

### Observer Callbacks to Remove (If Formulas Work)

| Observer | Lines | Can Remove If |
|----------|-------|---------------|
| `speed_factor_observer_cb` | ~15 | % formatting in XML |
| `flow_factor_observer_cb` | ~15 | % formatting in XML |
| `print_progress_observer_cb` (partial) | ~5 | % formatting in XML |
| `print_duration_observer_cb` | ~20 | Time formatting in XML |
| `print_time_left_observer_cb` | ~20 | Time formatting in XML |
| `extruder_temp_observer_cb` | ~15 | Temp formatting in XML |
| `bed_temp_observer_cb` | ~15 | Temp formatting in XML |

**Estimated C++ Reduction**: ~100 lines of observer callback code

---

## How to Investigate When v9.5 Releases

### Step 1: Check Release Status

```bash
# Check latest LVGL releases
gh release list --repo lvgl/lvgl --limit 5

# View specific release notes
gh release view v9.5.0 --repo lvgl/lvgl
```

Or visit: https://github.com/lvgl/lvgl/releases

### Step 2: Review the Changelog

```bash
# Fetch and read changelog from LVGL repo
curl -s https://raw.githubusercontent.com/lvgl/lvgl/master/docs/CHANGELOG.md | head -200
```

Key sections to look for:
- **Breaking changes** - API changes that require code updates
- **New features** - Slots, style binds, formula evaluator
- **XML changes** - New syntax or attributes

### Step 3: Test Slots Feature

**Read the merged PR for syntax details:**
- https://github.com/lvgl/lvgl/pull/9193

**Create a test branch:**
```bash
cd lib/lvgl
git fetch origin
git checkout v9.5.0  # or v9.5.1 when available

cd ../..
make clean && make -j
```

**Test with a simple slot component:**
```xml
<!-- ui_xml/test_slot.xml -->
<?xml version="1.0"?>
<component>
  <view extends="lv_obj" name="slot_test">
    <!-- Define a slot anchor point -->
    <lv_obj name="content_slot" width="100%" height="content"/>
  </view>
</component>
```

**Use the slot:**
```xml
<slot_test>
  <slot_test-content_slot>
    <text_body text="This goes in the slot!"/>
  </slot_test-content_slot>
</slot_test>
```

### Step 4: Test Formula Evaluator (If Merged)

**Check if feature was merged:**
```bash
# Search for formula-related commits
gh search commits "formula" --repo lvgl/lvgl --limit 10

# Or check the v9.5 planning issue for status
gh issue view 9254 --repo lvgl/lvgl
```

**Test basic formula syntax:**
```xml
<component>
  <consts>
    <const name="margin" value="16"/>
    <const name="columns" value="3"/>
  </consts>
  <view>
    <!-- Test arithmetic -->
    <lv_obj width="=(320 - #margin * 2)"/>

    <!-- Test with division -->
    <lv_obj width="=(320 / #columns)"/>
  </view>
</component>
```

**Test string concatenation (if supported):**
```xml
<!-- This is the key capability we need -->
<text_body bind_text="=(speed_factor) + '%'"/>
```

**Document what works/doesn't work** - update this file with findings.

### Step 5: Test Local Style Binds

**Read the merged PR:**
- https://github.com/lvgl/lvgl/pull/9184

**Test state-based styling:**
```xml
<lv_button>
  <!-- New syntax: style_property-part-state -->
  <lv_button style_bg_color-main-pressed="#FF0000"/>

  <!-- Or with bind_style_prop element -->
  <bind_style_prop prop="bg_color" selector="main|pressed" subject="theme_accent"/>
</lv_button>
```

### Step 6: Upgrade Process

```bash
# 1. Create feature branch
git checkout -b feature/lvgl-9.5-upgrade

# 2. Update submodule
cd lib/lvgl
git fetch origin
git checkout v9.5.1  # Wait for .1 patch!
cd ../..

# 3. Build and test
make clean && make -j
./build/bin/helix-screen --test -vv

# 4. Run test suite
make test-run

# 5. Test on hardware
make pi-test
```

### Step 7: Implement Slot Components

Once slots are confirmed working:

1. Create `ui_xml/kinematics_icon.xml` (already designed above)
2. Create `ui_xml/tune_slider_card.xml` (already designed above)
3. Migrate `print_tune_panel.xml` to use new components
4. Run visual regression tests

### Investigation Checklist

When evaluating v9.5, answer these questions:

**Slots:**
- [ ] Does `<component-slot_name>` syntax work?
- [ ] Can slots contain multiple children?
- [ ] Do slots work with conditional bindings inside?
- [ ] Performance impact of slot resolution?

**Formula Evaluator:**
- [ ] Is the feature included in v9.5?
- [ ] Does `=(expression)` syntax work for numeric properties?
- [ ] Does string concatenation work (`+ '%'`)?
- [ ] Does modulo operator work (`%`)?
- [ ] Can formulas reference subjects (reactive)?
- [ ] What happens on division by zero?

**Style Binds:**
- [ ] Does `style_prop-part-state` attribute syntax work?
- [ ] Does `<bind_style_prop>` element syntax work?
- [ ] Which parts are supported? (main, knob, indicator, etc.)
- [ ] Which states are supported? (pressed, focused, disabled, etc.)

---

## Tracking

- **LVGL 9.5 Planning Issue**: [#9254](https://github.com/lvgl/lvgl/issues/9254)
- **Slots PR**: [#9193](https://github.com/lvgl/lvgl/pull/9193) (Merged)
- **Local Style Binds PR**: [#9184](https://github.com/lvgl/lvgl/pull/9184) (Merged)
- **Formula Evaluator**: Not yet merged - watch for PR in planning issue
- **Feature Freeze**: February 2, 2026
- **Release Target**: February 18, 2026
- **Upgrade Plan**: Wait for v9.5.1 patch (let early adopters find bugs)

---

## Automated Tracking

A GitHub Action (`.github/workflows/lvgl-tracker.yml`) runs weekly to:
1. Check LVGL releases for v9.5+
2. Auto-create an issue with upgrade checklist when detected
3. Tag with `lvgl` and `upgrade` labels

**Manual trigger:**
```bash
gh workflow run lvgl-tracker.yml
```
