#include "gui_only_input.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <mutex>
#include <unistd.h>

namespace gui_input {
namespace {

enum class EventType { Move, Down, Up };
struct Event {
    EventType type;
    float x = 0.f;
    float y = 0.f;
};

struct Device {
    int fd = -1;
    bool multitouch = false;
    int minX = 0, maxX = 1, minY = 0, maxY = 1;
    int rawX = 0, rawY = 0;
    int slot = 0;
    int primarySlot = -1;
    std::array<bool, 16> active{};
    std::array<int, 16> xs{};
    std::array<int, 16> ys{};
    bool singleDown = false;
    bool publishedDown = false;
    int publishedSlot = -1;
};

std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_mutex;
std::deque<Event> g_events;
int g_width = 1, g_height = 1;
uint32_t g_orientation = 0;
Device g_device;

bool HasAbsAxis(int fd, int axis, input_absinfo& info) {
    return ioctl(fd, EVIOCGABS(axis), &info) == 0 && info.maximum > info.minimum;
}

bool OpenTouchDevice(Device& out) {
    for (int index = 0; index < 64; ++index) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", index);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        input_absinfo mtX{}, mtY{}, x{}, y{};
        const bool hasMt = HasAbsAxis(fd, ABS_MT_POSITION_X, mtX) &&
                           HasAbsAxis(fd, ABS_MT_POSITION_Y, mtY);
        const bool hasSingle = HasAbsAxis(fd, ABS_X, x) && HasAbsAxis(fd, ABS_Y, y);
        if (!hasMt && !hasSingle) {
            close(fd);
            continue;
        }

        out = {};
        out.fd = fd;
        out.multitouch = hasMt;
        const input_absinfo& ax = hasMt ? mtX : x;
        const input_absinfo& ay = hasMt ? mtY : y;
        out.minX = ax.minimum;
        out.maxX = ax.maximum;
        out.minY = ay.minimum;
        out.maxY = ay.maximum;
        return true;
    }
    return false;
}

void Push(EventType type, float x, float y) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Keep the queue bounded if a device floods SYN_REPORT events. Never drop
    // a button transition; drop only old movement samples.
    if (g_events.size() >= 256) {
        auto it = std::find_if(g_events.begin(), g_events.end(), [](const Event& e) {
            return e.type == EventType::Move;
        });
        if (it != g_events.end()) g_events.erase(it);
        else g_events.pop_front();
    }
    g_events.push_back({type, x, y});
}

void Map(Device& d, int rawX, int rawY, float& x, float& y) {
    const float nx = std::clamp(
        (rawX - d.minX) / static_cast<float>(std::max(1, d.maxX - d.minX)), 0.f, 1.f);
    const float ny = std::clamp(
        (rawY - d.minY) / static_cast<float>(std::max(1, d.maxY - d.minY)), 0.f, 1.f);

    const float w = static_cast<float>(std::max(1, g_width));
    const float h = static_cast<float>(std::max(1, g_height));
    switch (g_orientation & 3u) {
        case 1: x = (1.f - ny) * w; y = nx * h; break;
        case 2: x = (1.f - nx) * w; y = (1.f - ny) * h; break;
        case 3: x = ny * w; y = (1.f - nx) * h; break;
        default: x = nx * w; y = ny * h; break;
    }
}

void Publish(Device& d) {
    int selected = -1;
    if (d.multitouch) {
        if (d.primarySlot >= 0 && d.primarySlot < static_cast<int>(d.active.size()) &&
            d.active[d.primarySlot]) {
            selected = d.primarySlot;
        } else {
            for (int i = 0; i < static_cast<int>(d.active.size()); ++i) {
                if (d.active[i]) { selected = i; break; }
            }
            d.primarySlot = selected;
        }
    } else if (d.singleDown) {
        selected = 0;
    }

    if (selected < 0) {
        if (d.publishedDown) {
            d.publishedDown = false;
            d.publishedSlot = -1;
            Push(EventType::Up, 0.f, 0.f);
        }
        return;
    }

    const int rawX = d.multitouch ? d.xs[selected] : d.rawX;
    const int rawY = d.multitouch ? d.ys[selected] : d.rawY;
    float x = 0.f, y = 0.f;
    Map(d, rawX, rawY, x, y);
    if (!d.publishedDown) {
        d.publishedDown = true;
        d.publishedSlot = selected;
        Push(EventType::Down, x, y);
    } else {
        Push(EventType::Move, x, y);
    }
}

void ReadLoop() {
    Device d;
    if (!OpenTouchDevice(d)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_device = d;
    }

    input_event events[32]{};
    while (g_running.load()) {
        pollfd pfd{d.fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, 250);
        if (ready <= 0) continue;
        const ssize_t bytes = read(d.fd, events, sizeof(events));
        if (bytes <= 0) continue;
        const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
        for (size_t i = 0; i < count; ++i) {
            const input_event& ev = events[i];
            if (d.multitouch) {
                if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
                    d.slot = std::clamp(ev.value, 0, static_cast<int>(d.active.size()) - 1);
                } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
                    d.active[d.slot] = ev.value >= 0;
                    if (!d.active[d.slot] && d.slot == d.primarySlot) d.primarySlot = -1;
                } else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) {
                    d.xs[d.slot] = ev.value;
                } else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
                    d.ys[d.slot] = ev.value;
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    Publish(d);
                }
            } else {
                if (ev.type == EV_ABS && ev.code == ABS_X) d.rawX = ev.value;
                else if (ev.type == EV_ABS && ev.code == ABS_Y) d.rawY = ev.value;
                else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) d.singleDown = ev.value != 0;
                else if (ev.type == EV_SYN && ev.code == SYN_REPORT) Publish(d);
            }
        }
    }

    close(d.fd);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (d.publishedDown) g_events.push_back({EventType::Up, 0.f, 0.f});
    g_device.fd = -1;
}

}  // namespace

bool Start(int width, int height, uint32_t orientation) {
    Stop();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_width = std::max(1, width);
        g_height = std::max(1, height);
        g_orientation = orientation;
        g_events.clear();
    }
    g_running.store(true);
    g_thread = std::thread(ReadLoop);
    return true;
}

void Stop() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_events.clear();
}

void UpdateScreen(int width, int height, uint32_t orientation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_width = std::max(1, width);
    g_height = std::max(1, height);
    g_orientation = orientation;
}

void ApplyToImGui() {
    std::deque<Event> events;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        events.swap(g_events);
    }
    if (events.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    for (const Event& event : events) {
        if (event.type != EventType::Up)
            io.AddMousePosEvent(event.x, event.y);
        if (event.type == EventType::Down)
            io.AddMouseButtonEvent(0, true);
        else if (event.type == EventType::Up)
            io.AddMouseButtonEvent(0, false);
    }
}

}  // namespace gui_input
