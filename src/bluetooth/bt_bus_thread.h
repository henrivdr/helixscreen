// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <systemd/sd-bus.h>
#include <thread>

namespace helix::bluetooth {

/// Work item executed on the bus thread. Receives the bus pointer, returns nothing
/// (result is captured via promise/future owned by the submitter).
using BusWork = std::function<void(sd_bus*)>;

/// Single-threaded owner of an sd_bus* connection. All sd_bus_* calls (process,
/// wait, call_method, match_signal, slot_unref) must happen on this thread.
///
/// Usage:
///   BusThread bt(bus);
///   bt.start();
///   int result;
///   bt.submit([&](sd_bus* b){ result = sd_bus_call_method(b, ...); }).wait();
///   bt.stop();
class BusThread {
  public:
    /// Construct with a bus pointer. The BusThread does NOT take ownership —
    /// the caller must keep the bus alive until after stop() returns.
    ///
    /// `bus` may be `nullptr`, meaning "no bus, idle worker": the loop still
    /// runs and processes queued work items, but skips all sd_bus_* calls and
    /// only polls the wakeup pipe. This is useful for tests and defends
    /// against production null-bus misconfigurations.
    explicit BusThread(sd_bus* bus);
    ~BusThread();

    BusThread(const BusThread&) = delete;
    BusThread& operator=(const BusThread&) = delete;

    /// Launch the worker thread. Idempotent.
    void start();

    /// Signal the worker to stop, drain any in-flight work item, then join.
    /// Safe to call multiple times. Must be called before destroying the bus.
    void stop();

    /// Submit a work item. Returns a future that resolves once the work runs
    /// (or is dropped during stop()). The promise is broken if stop() runs
    /// before the work is executed — callers should tolerate std::future_error.
    ///
    /// Safe to call from any thread, including from *inside* another work item
    /// (but watch for self-wait deadlocks — never wait on the returned future
    /// from inside a bus-thread work item).
    std::future<void> submit(BusWork work);

    /// Run `fn(bus)` synchronously on the bus thread and wait for completion.
    /// Throws if the thread is stopping. Prefer this over submit() in most
    /// public API wrappers.
    void run_sync(BusWork work);

    /// Wake the thread from sd_bus_wait(). Used by submit() to ensure queued
    /// work runs promptly even if no D-Bus traffic arrives.
    void notify();

    /// True if called from the bus thread itself.
    bool on_thread() const noexcept;

  private:
    void loop();

    /// Pop every queued work item and break its promise with `reason`. Caller
    /// must hold mu_. Used by both stop() (after join) and loop() (on the
    /// worker thread when sd_bus_process fails) to ensure no submit()ted future
    /// is ever orphaned waiting on ~BusThread.
    void drain_and_break_promises_locked(const char* reason);

    sd_bus* bus_;
    std::thread thread_;
    std::mutex mu_;
    std::deque<std::pair<BusWork, std::promise<void>>> queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    // Assigned by the worker itself at the top of loop() (before any work runs),
    // so on_thread() always sees a valid id without racing against the parent's
    // post-spawn write. Atomic because any thread may read via on_thread().
    std::atomic<std::thread::id> thread_id_{std::thread::id{}};

    /// File descriptor pair used to wake sd_bus_wait() when work is queued.
    /// sd_bus_wait() only returns on bus activity or timeout; to get work
    /// processed promptly we need an external wakeup.
    int wakeup_fds_[2] = {-1, -1};
};

} // namespace helix::bluetooth
