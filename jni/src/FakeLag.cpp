#include "FakeLag.h"

#include <cstdio>
#include <cstring>

static const char* BLOCK   = "iptables -I INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP";
static const char* UNBLOCK = "iptables -D INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP 2>/dev/null";

// Выполняет команду через su -c '...'
static void executeRoot(const char* cmd) {
    char buf[512];
    snprintf(buf, sizeof(buf), "su -c '%s'", cmd);
    FILE* f = popen(buf, "r");
    if (f) pclose(f);
}

FakeLag::FakeLag()
    : m_enabled(false)
    , m_percent(45)
    , m_blocked(false)
    , m_running(false)
{}

FakeLag::~FakeLag() {
    destroy();
}

void FakeLag::setPercent(int percent) {
    if (percent < 1)   percent = 1;
    if (percent > 100) percent = 100;
    m_percent.store(percent);
}

void FakeLag::apply(bool enable) {
    if (enable) {
        if (m_running.load()) return;   // уже запущен
        m_enabled.store(true);
        m_running.store(true);
        m_blocked = false;
        m_thread = std::thread(&FakeLag::loop, this);
    } else {
        m_enabled.store(false);
        m_running.store(false);
        if (m_thread.joinable()) m_thread.join();
        // DROP отбрасывает пакет сразу — он не копится, поэтому после
        // выключения пинг не улетает пачкой. Делаем два -D подряд чтобы
        // гарантированно снять правило даже если в цепочке оказалось два
        // одинаковых (креш/дубль), лишние падают в 2>/dev/null.
        executeRoot(UNBLOCK);
        executeRoot(UNBLOCK);
        m_blocked = false;
    }
}

void FakeLag::destroy() {
    apply(false);
}

void FakeLag::loop() {
    while (m_running.load()) {
        if (!m_enabled.load()) break;

        int percent = m_percent.load();

        if (percent >= 90) {
            // постоянный блок, проверяем раз в секунду
            if (!m_blocked) {
                executeRoot(BLOCK);
                m_blocked = true;
            }
            sleepMs(1000);
            continue;
        }

        int blockMs   = (1000 * percent) / 100;
        int unblockMs = 1000 - blockMs;

        if (m_blocked) {
            executeRoot(UNBLOCK);
            m_blocked = false;
            sleepMs(unblockMs);
        } else {
            executeRoot(BLOCK);
            m_blocked = true;
            sleepMs(blockMs);
        }
    }
}

void FakeLag::sleepMs(int ms) {
    // спим по 50мс кускам чтобы быстро реагировать на apply(false)
    while (ms > 0 && m_running.load()) {
        int step = ms > 50 ? 50 : ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        ms -= step;
    }
}
