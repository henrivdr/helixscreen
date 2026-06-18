// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "observer_factory.h"
#include "recovery_modal_presenter.h"

namespace helix {

/// Observes AmsState's action subject and routes AmsAction::ERROR edges into
/// RecoveryModalPresenter. Purely a bridge — contains no error semantics.
class AmsErrorBridge {
  public:
    explicit AmsErrorBridge(helix::ui::RecoveryModalPresenter& presenter);
    void start();  ///< installs the observer on AmsState's action subject (one-shot)
  private:
    void on_action_changed(int action);

    helix::ui::RecoveryModalPresenter& presenter_;
    ObserverGuard action_observer_;
    int prev_action_ = -1;  ///< sentinel; no AmsAction maps to -1, so the first tick never edges
    bool presented_ = false;  ///< true if we showed the modal for the current ERROR episode
};

}  // namespace helix
