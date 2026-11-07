#pragma once

#include <cstdint>
#include <functional>

namespace gw::app {

// The program runs on one main thread that owns every window and document. Work from other
// threads (language servers, the terminal, git) comes back to it through runOnMain.

/// Seconds on a steady clock.
double now();

/// Runs `work` on the main thread soon. Safe from any thread.
void runOnMain(std::function<void()> work);
/// Runs `work` on a background thread.
void runInBackground(std::function<void()> work);

/// Runs `work` on the main thread after `seconds`. Returns an id for cancelTimer.
uint64_t afterDelay(double seconds, std::function<void()> work);
void cancelTimer(uint64_t timer);

// ---- For the main loop
/// Wakes the main loop from any thread (it sleeps until something happens).
void wakeMainLoop();
/// Runs what other threads queued and the timers that are due. Main thread only.
void runPendingWork();
/// Seconds until the next timer, or a negative number for none.
double secondsUntilNextTimer();
/// The event type SDL delivers for wake-ups.
uint32_t wakeEventType();
void initializeLoop();

}  // namespace gw::app
