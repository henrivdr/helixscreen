// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include "label_printer.h"
#include "spoolman_types.h"

#include <functional>
#include <string>

namespace helix {

/**
 * @brief Pre-print modal for IPP sheet label printing
 *
 * Shows before printing to let the user configure:
 * - Number of labels to print (default: from settings)
 * - Start position on the sheet (default: Position 1)
 *
 * Displayed when the user triggers "Print Label" with an IPP printer configured.
 */
class IppPrintModal : public Modal {
  public:
    IppPrintModal();
    ~IppPrintModal() override = default;

    const char* get_name() const override {
        return "IPP Print";
    }
    const char* component_name() const override {
        return "ipp_print_modal";
    }

    /// Configure and show the modal for a spool print job.
    /// When user confirms, executes the print with chosen settings.
    void show_for_spool(const SpoolInfo& spool, PrintCallback callback);

  protected:
    void on_show() override;
    void on_ok() override;
    void on_cancel() override;

  private:
    void init_dropdowns();

    SpoolInfo spool_;
    PrintCallback callback_;
    int max_labels_ = 30;
};

/// Check if IPP printer is configured and show the print modal if so.
/// Returns true if the modal was shown (caller should NOT call print_spool_label).
/// Returns false if not IPP (caller should proceed with print_spool_label).
bool maybe_show_ipp_print_modal(const SpoolInfo& spool, PrintCallback callback);

} // namespace helix
