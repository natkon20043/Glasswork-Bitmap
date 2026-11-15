#include "Loop.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace gw::app {

namespace {

std::mutex queueMutex;
std::deque<std::function<void()>> mainQueue;

struct Timer {
    uint64_t id;
    std::function<void()> work;
};
std::multimap<double, Timer> timers;  // main thread only
uint64_t nextTimerId = 1;

uint32_t wakeType = 0;
std::atomic<bool> wakePending{false};
SDL_TimerID wakeTimer = 0;
double wakeTimerDeadline = -1;
/// When the main loop's next timer is due (-1: none), for the watchdog.
std::atomic<double> nextDeadline{-1};

/// A few worker threads for background jobs, so a burst of them doesn't start a thread each.
class WorkerPool {
public:
    WorkerPool() {
        unsigned count = std::max(2u, std::min(6u, std::thread::hardware_concurrency()));
        for (unsigned index = 0; index < count; ++index) {
            std::thread([this] { run(); }).detach();
        }
    }

    void add(std::function<void()> work) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(work));
        }
        ready_.notify_one();
    }

private:
    void run() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
};

WorkerPool& pool() {
    static WorkerPool* workers = new WorkerPool();  // never destroyed: its threads outlive main
    return *workers;
}

Uint32 wakeAtDeadline(void*, SDL_TimerID, Uint32) {
    wakeMainLoop();
    return 0;
}

double steadySeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/// SDL (3.4) can miss a wake-up: an event pushed from another thread in the moment between the main
/// thread finding the queue empty and starting to wait doesn't wake it (SDL_WaitEventTimeout_Device
/// sets the window to wake only after looking at the queue). Work for the main loop (a debugger
/// stopping at a breakpoint, a language server's answer, the terminal's output) then waited for
/// the next key or mouse move. Ten times a second this looks for work or a timer that has waited
/// too long and pushes another event, which does wake it; when nothing is waiting it does nothing.
Uint32 watchdog(void*, SDL_TimerID, Uint32 interval) {
    bool overdue = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        overdue = !mainQueue.empty();
    }
    double deadline = nextDeadline.load();
    if (deadline >= 0 && steadySeconds() > deadline + 0.05) {
        overdue = true;
    }
    if (overdue) {
        SDL_Event event = {};
        event.type = wakeType;
        SDL_PushEvent(&event);
    }
    return interval;
}

}  // namespace

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void initializeLoop() {
    wakeType = SDL_RegisterEvents(1);
    SDL_AddTimer(100, watchdog, nullptr);
}

uint32_t wakeEventType() {
    return wakeType;
}

void wakeMainLoop() {
    if (wakePending.exchange(true)) {
        return;  // one wake-up is already on its way
    }
    SDL_Event event = {};
    event.type = wakeType;
    SDL_PushEvent(&event);
}

void runOnMain(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        mainQueue.push_back(std::move(work));
    }
    wakeMainLoop();
}

void runInBackground(std::function<void()> work) {
    pool().add(std::move(work));
}

uint64_t afterDelay(double seconds, std::function<void()> work) {
    uint64_t id = nextTimerId++;
    timers.emplace(now() + std::max(0.0, seconds), Timer{id, std::move(work)});
    return id;
}

void cancelTimer(uint64_t timer) {
    for (auto it = timers.begin(); it != timers.end(); ++it) {
        if (it->second.id == timer) {
            timers.erase(it);
            return;
        }
    }
}

void runPendingWork() {
    wakePending.store(false);
    // Queued work first; anything it queues waits for the next round, so nothing can starve the loop.
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        batch.swap(mainQueue);
    }
    for (auto& work : batch) {
        work();
    }
    double current = now();
    std::vector<std::function<void()>> due;
    while (!timers.empty() && timers.begin()->first <= current) {
        due.push_back(std::move(timers.begin()->second.work));
        timers.erase(timers.begin());
    }
    for (auto& work : due) {
        work();
    }
    // Sleep no longer than the next timer.
    double wait = secondsUntilNextTimer();
    nextDeadline.store(wait >= 0 ? now() + wait : -1);
    if (wait >= 0) {
        double deadline = now() + wait;
        if (wakeTimer == 0 || wakeTimerDeadline < 0 || deadline < wakeTimerDeadline - 0.0005 || now() > wakeTimerDeadline) {
            if (wakeTimer != 0) {
                SDL_RemoveTimer(wakeTimer);
            }
            Uint32 milliseconds = static_cast<Uint32>(std::max(1.0, wait * 1000.0));
            wakeTimer = SDL_AddTimer(milliseconds, wakeAtDeadline, nullptr);
            wakeTimerDeadline = deadline;
        }
    }
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!mainQueue.empty()) {
        wakeMainLoop();
    }
}

double secondsUntilNextTimer() {
    if (timers.empty()) {
        return -1;
    }
    return std::max(0.0, timers.begin()->first - now());
}

}  // namespace gw::app
