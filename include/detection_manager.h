// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "detection_source.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace helix {
class MoonrakerClient;
}
class PrinterState;

namespace helix::detection {

enum class DetectionPolicy { Off, NotifyOnly, DeferToSource };

/// Registry of detection sources plus per-source policy and the UI presenter hook.
///
/// Sources route their (already main-thread-marshaled) events into the manager,
/// which consults the source's policy and invokes the presenter when warranted.
class DetectionManager {
  public:
    static DetectionManager& instance();

    /// Wire up Moonraker/PrinterState deps and probe for detector capabilities.
    void init(helix::MoonrakerClient* client, ::PrinterState* state);

    /// Take ownership of a source, route its events here, default its policy to
    /// DeferToSource if one was not already set.
    void register_source(std::unique_ptr<DetectionSource> src);

    void set_policy(const std::string& source_id, DetectionPolicy p);
    DetectionPolicy policy(const std::string& source_id) const;

    bool any_available() const;

    using Presenter = std::function<void(const DetectionEvent&, DetectionPolicy)>;
    void set_presenter(Presenter p) { presenter_ = std::move(p); }

    void reset_for_test();

  private:
    DetectionManager() = default;

    void on_event(const DetectionEvent& e);
    void probe_capabilities();

    helix::MoonrakerClient* client_ = nullptr;
    ::PrinterState*         state_  = nullptr;

    std::vector<std::unique_ptr<DetectionSource>> sources_;
    std::map<std::string, DetectionPolicy>        policies_;
    Presenter                                     presenter_;
};

}  // namespace helix::detection
