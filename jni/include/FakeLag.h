#pragma once
// FakeLag: дропает UDP-трафик игры через iptables с чередованием блок/пропуск.
// DROP отбрасывает пакет сразу — он не копится в очереди, поэтому после
// выключения функции пинг не улетает в космос от пачки накопившихся пакетов.
// При выключении правило гарантированно снимается (несколько -D подряд).

#include <atomic>
#include <thread>

class FakeLag {
public:
    FakeLag();
    ~FakeLag();

    // intensity 1..100 (дефолт 45). Читается атомарно в loop — можно менять на лету.
    void setPercent(int percent);

    // enable=true  -> запускает поток с toggle-циклом
    // enable=false -> стоп потока + гарантированная очистка iptables
    void apply(bool enable);

    // алиас apply(false), зовётся из деструктора и при выходе приложения
    void destroy();

    bool isEnabled() const { return m_running.load(); }

private:
    void loop();
    void sleepMs(int ms);
    void ensureUnblocked(); // несколько -D подряд — чистит дубликаты и залипшие правила

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_running{false};
    std::atomic<int>  m_percent{45};
    bool              m_blocked = false; // только внутри loop-потока
    std::thread       m_thread;
};
