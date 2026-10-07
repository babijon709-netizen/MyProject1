#include "FakeLag.h"

#include <cstdio>
#include <thread>
#include <chrono>

// Фильтр длины 80:65535 целится в игровые UDP-пакеты, мелкий служебный трафик не трогаем.
// Блочим обе стороны — INPUT и OUTPUT, иначе исходящие позиции продолжают лететь
// и на сервере ты не лагаешь, а входящие дропаются отдельно. DROP именно
// отбрасывает пакет, а не ставит в очередь: после выключения ничего не
// всплывает пачкой и пинг сразу возвращается в норму.
static const char* BLOCK_IN    = "iptables -I INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP";
static const char* UNBLOCK_IN  = "iptables -D INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP 2>/dev/null";
static const char* BLOCK_OUT   = "iptables -I OUTPUT ! -o lo -p udp -m length --length 80:65535 -j DROP";
static const char* UNBLOCK_OUT = "iptables -D OUTPUT ! -o lo -p udp -m length --length 80:65535 -j DROP 2>/dev/null";

static void executeRoot(const char* cmd) {
    char buf[640];
    // su -c '...' — одинарные кавычки внутри, поэтому cmd не должен их содержать
    snprintf(buf, sizeof(buf), "su -c '%s'", cmd);
    FILE* f = popen(buf, "r");
    if (f) pclose(f);
}

FakeLag::FakeLag() = default;

FakeLag::~FakeLag() {
    destroy();
}

void FakeLag::setPercent(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    m_percent.store(percent, std::memory_order_relaxed);
}

void FakeLag::ensureUnblocked() {
    // Несколько -D подряд гарантированно снимают правило даже если из-за
    // предыдущего креша/утечки в цепочке оказалось два одинаковых правила.
    // Лишние -D просто падают в 2>/dev/null.
    for (int i = 0; i < 3; ++i) {
        executeRoot(UNBLOCK_IN);
        executeRoot(UNBLOCK_OUT);
    }
    m_blocked = false;
}

void FakeLag::apply(bool enable) {
    if (enable) {
        if (m_running.load(std::memory_order_acquire)) return; // уже крутится
        m_enabled.store(true, std::memory_order_release);
        m_running.store(true, std::memory_order_release);
        m_blocked = false;
        m_thread = std::thread(&FakeLag::loop, this);
    } else {
        m_enabled.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable()) m_thread.join();
        if (m_blocked) {
            ensureUnblocked();
        } else {
            // даже если m_blocked==false, на всякий случай чистим — правило
            // могло залипнуть от прошлой сборки/креша до перезапуска потока
            ensureUnblocked();
        }
    }
}

void FakeLag::destroy() {
    apply(false);
}

void FakeLag::loop() {
    while (m_running.load(std::memory_order_acquire)) {
        if (!m_enabled.load(std::memory_order_acquire)) break;

        int percent = m_percent.load(std::memory_order_relaxed);
        if (percent < 1) percent = 1;
        if (percent > 100) percent = 100;

        if (percent >= 90) {
            // почти постоянный лаг: держим дроп и чекаем раз в секунду
            if (!m_blocked) {
                executeRoot(BLOCK_IN);
                executeRoot(BLOCK_OUT);
                m_blocked = true;
            }
            sleepMs(1000);
            continue;
        }

        int blockMs   = (1000 * percent) / 100;
        int unblockMs = 1000 - blockMs;
        if (blockMs < 20) blockMs = 20;
        if (unblockMs < 20) unblockMs = 20;

        if (m_blocked) {
            // сейчас залочено -> открываем окно
            executeRoot(UNBLOCK_IN);
            executeRoot(UNBLOCK_OUT);
            m_blocked = false;
            sleepMs(unblockMs);
        } else {
            executeRoot(BLOCK_IN);
            executeRoot(BLOCK_OUT);
            m_blocked = true;
            sleepMs(blockMs);
        }
    }
    // выход из loop — поток уже не вернётся, но на всякий случай снимем блок
    // если нас прервали посреди block-фазы. Основной apply(false) тоже снимет,
    // двойной -D безвреден.
    if (m_blocked) {
        executeRoot(UNBLOCK_IN);
        executeRoot(UNBLOCK_OUT);
        m_blocked = false;
    }
}

void FakeLag::sleepMs(int ms) {
    // кусками по 50 мс — быстро реагируем на apply(false), пинг не зависает
    // на лишнюю секунду после выключения тумблера
    while (ms > 0 && m_running.load(std::memory_order_acquire)) {
        int step = ms > 50 ? 50 : ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        ms -= step;
    }
}
