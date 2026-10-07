#pragma once

#include <atomic>
#include <thread>
#include <chrono>

class FakeLag {
public:
    FakeLag();
    ~FakeLag();

    // процент intensity, 1–100 (default 45)
    // задать до или во время работы — читается атомарно в loop
    void setPercent(int percent);

    // enable=true  → запускает поток с toggle-циклом
    // enable=false → останавливает поток, убирает iptables-правило
    void apply(bool enable);

    // то же что apply(false), вызывается из деструктора
    void destroy();

private:
    void loop();
    void sleepMs(int ms);

    std::atomic<bool> m_enabled;
    std::atomic<bool> m_running;
    std::atomic<int>  m_percent;
    bool              m_blocked;   // только внутри потока loop, без гонок
    std::thread       m_thread;
};
