#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <sys/types.h>

/// Drives the TV over HDMI-CEC through a long-lived `cec-client` child process.
/// All writes happen on a worker thread so the main loop never blocks.
class CecController {
public:
    explicit CecController(bool volume_enabled);
    ~CecController();

    CecController(const CecController&) = delete;
    CecController& operator=(const CecController&) = delete;

    /// Spawn cec-client and the worker thread. Returns false on failure.
    bool start();

    /// Stop the worker and shut down cec-client. Safe to call more than once.
    void stop();

    bool volume_enabled() const { return volume_enabled_; }

    /// Power on the TV and take over the input, once per wake cycle.
    void wake();

    /// Put the TV in standby (no-op if we didn't wake it).
    void standby();

    /// Sendspin volume is absolute (0-100) but CEC only has up/down key
    /// presses, so the difference is sent as presses. The first call only
    /// records the baseline.
    void set_volume(uint8_t vol);

    /// Mute is a toggle on the CEC side; only sends when the state changes.
    void set_muted(bool muted);

private:
    struct Item {
        std::string cmd;
        int delay_ms;
    };

    void push(std::string cmd, int delay_ms);
    void run();

    bool volume_enabled_;
    int fd_{-1};
    pid_t pid_{-1};
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> q_;
    bool stop_{false};
    std::atomic<bool> awake_{false};
    std::atomic<bool> muted_{false};
    std::atomic<int> last_volume_{-1};
};