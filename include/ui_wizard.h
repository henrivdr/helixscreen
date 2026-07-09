// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "wizard_step.h" // helix::wizard::StepId

#include <functional>
#include <vector>

// Forward declaration: SubjectManager for wizard subjects (defined in ui_wizard.cpp)
// Wizard is function-based rather than class-based, so we use a static manager

/**
 * Wizard Container - Responsive Multi-Step UI Component
 *
 * Clean separation: This component handles ONLY navigation and layout.
 * Screen content and business logic belong in the wizard screen components.
 *
 * Initialization Order (CRITICAL):
 *   1. Register XML components (globals.xml, wizard_container.xml, all wizard_*.xml)
 *   2. ui_wizard_init_subjects()
 *   3. ui_wizard_register_event_callbacks()
 *   4. ui_wizard_container_register_responsive_constants()  <- BEFORE creating XML
 *   5. ui_wizard_create(parent)
 *   6. ui_wizard_navigate_to_step(1)
 */

/**
 * Initialize wizard subjects
 *
 * Creates and registers reactive subjects for wizard state:
 * - current_step (int)
 * - total_steps (int)
 * - wizard_title (string)
 * - wizard_progress (string, e.g. "Step 2 of 7")
 * - wizard_next_button_text (string, "Next" or "Finish")
 *
 * MUST be called BEFORE creating XML components.
 */
void ui_wizard_init_subjects();

/**
 * Deinitialize wizard subjects
 *
 * Disconnects observers from all wizard subjects before shutdown.
 * Called by StaticPanelRegistry during Application::shutdown().
 */
void ui_wizard_deinit_subjects();

/**
 * Register responsive constants to wizard_container scope and propagate to children
 *
 * Detects screen size and registers wizard-specific constants to wizard_container scope,
 * then propagates to all child wizard screens. Uses parent-defined constants pattern
 * to avoid polluting globals scope.
 *
 * Responsive values by screen size:
 * - SMALL (≤480):    list_padding=4,  header=32,  footer=72,  button=110
 * - MEDIUM (481-800): list_padding=6,  header=42,  footer=82,  button=140
 * - LARGE (>800):     list_padding=8,  header=48,  footer=88,  button=160
 *
 * Also sets responsive fonts and WiFi screen dimensions.
 *
 * MUST be called AFTER all wizard_*.xml components are registered and BEFORE ui_wizard_create().
 */
void ui_wizard_container_register_responsive_constants();

/**
 * Register event callbacks
 *
 * Registers internal navigation callbacks:
 * - on_back_clicked
 * - on_next_clicked
 *
 * MUST be called BEFORE creating XML components.
 */
void ui_wizard_register_event_callbacks();

/**
 * Create wizard container
 *
 * Creates the wizard UI from wizard_container.xml.
 * Returns the root wizard object.
 *
 * Prerequisites:
 * - ui_wizard_init_subjects() called
 * - ui_wizard_register_event_callbacks() called
 * - ui_wizard_container_register_responsive_constants() called
 *
 * @param parent Parent object (typically screen root)
 * @return The wizard root object, or NULL on failure
 */
lv_obj_t* ui_wizard_create(lv_obj_t* parent);

/**
 * Navigate to specific step
 *
 * Updates all wizard subjects (title, progress, button text).
 * Handles back button visibility (hidden on step 1).
 *
 * @param step Wizard step id from the step registry.
 */
void ui_wizard_navigate_to_step(helix::wizard::StepId step);

/**
 * Set wizard title
 *
 * Updates the wizard_title subject.
 *
 * @param title New title string
 */
void ui_wizard_set_title(const char* title);

/**
 * Refresh wizard header translations
 *
 * Re-translates the title and subtitle for the current step using lv_tr().
 * Should be called after language changes to update bound subjects with
 * newly translated text.
 */
void ui_wizard_refresh_header_translations();

/**
 * Complete wizard and transition to main UI
 *
 * Called when user clicks Finish on summary screen. Performs:
 * - Cleans up all wizard screens
 * - Deletes wizard container
 * - Connects to Moonraker using saved config
 * - Transitions to main UI (already created underneath)
 *
 * NOTE: Config should already be saved by wizard screens before calling this.
 */
void ui_wizard_complete();

/**
 * Create a targeted (subset) wizard session
 *
 * Launches the wizard running ONLY the requested step(s), in the given order,
 * for hardware reconfiguration WITHOUT re-running the full first-run wizard.
 * Reuses the standard container/subject setup (ui_wizard_create) and then
 * navigates to the first step in @p steps. While a targeted session is active,
 * the Next/Back navigation advances/retreats only within @p steps; finishing
 * the last step calls ui_wizard_complete_targeted() (which does NOT set the
 * wizard_completed flag) and fires @p on_complete.
 *
 * @param parent      Parent object (typically screen root)
 * @param steps       Ordered subset of wizard steps to run (non-empty)
 * @param on_complete Callback invoked after the subset finishes and the
 *                    container is torn down (typically returns to prior panel)
 * @return The wizard root object, or NULL on failure
 */
lv_obj_t* ui_wizard_create_targeted(lv_obj_t* parent, std::vector<helix::wizard::StepId> steps,
                                    std::function<void()> on_complete);

/**
 * Query the active targeted step subset
 *
 * Returns the ordered subset passed to ui_wizard_create_targeted(). Empty when
 * not running a targeted session (i.e. the normal first-run wizard, or no
 * wizard running).
 */
const std::vector<helix::wizard::StepId>& ui_wizard_active_step_subset();

/**
 * Complete a targeted (subset) wizard session
 *
 * Finishes targeted mode WITHOUT writing the wizard_completed flag and without
 * the expected-hardware population done by ui_wizard_complete(). Tears down the
 * wizard container (shared with ui_wizard_complete()), clears the active subset,
 * and fires the on_complete callback registered by ui_wizard_create_targeted().
 */
void ui_wizard_complete_targeted();
