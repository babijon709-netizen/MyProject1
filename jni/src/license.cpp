// license.cpp — защита бинаря benzware: лицензионные ключи, привязка к устройству,
// удалённое отключение (kill-switch), подписанные ответы сервера (HMAC-SHA256),
// анти-отладка, самопроверка целостности ELF.
//
// ══════════════════ НАСТРОЙКА ПЕРЕД СБОРКОЙ (2 места ниже) ══════════════════
//   1. S_HOST()   — адрес сервера лицензий: IP или домен, БЕЗ "http://".
//   2. S_SECRET() — секрет подписи. ДОЛЖЕН совпадать с переменной окружения
//                   SECRET на сервере (server/server.js). Поменяй на свой!
// Оба значения зашиты XOR-обфускацией (xp::_mk), в бинаре открытым текстом
// не лежат.
// ═══════════════════════════════════════════════════════════════════════════

#include "license.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "str.h" // xp::_mk — compile-time XOR строк

#ifdef __ANDROID__
#  include <android/log.h>
#  include <sys/system_properties.h>
#else
// Заглушки для десктопной проверки сборки/тестов.
static int __system_property_get(const char*, char*) { return 0; }
#endif

// main.cpp: флаг главного цикла — выставляем в false при kill.
extern std::atomic<bool> main_thread_flag;

namespace license {
namespace {

// ────────────────────────── настройки (МЕНЯТЬ ЗДЕСЬ) ─────────────────────────

std::string S_HOST()   { static constexpr auto s = xp::_mk("benzware-license-server.fly.dev");             return s.d(); }
std::string S_SECRET() { static constexpr auto s = xp::_mk("CHANGE_ME_same_secret_as_on_server_7f3a9c21"); return s.d(); }

// ВНИМАНИЕ: значения выше — плейсхолдеры. В CI перед сборкой их заменяет
// tools/protect/inject_settings.py из GitHub Secrets (BZW_HOST, BZW_SECRET),
// так что в репозитории реальный секрет никогда не лежит.
constexpr uint16_t kPort         = 80;               // fly.dev снаружи слушает 80 (self-host — поменяй под свой порт)
constexpr uint32_t kBuildId      = 1;                // номер сборки (сервер может требовать min_build)
constexpr int      kHeartbeatSec = 180;              // как часто спрашиваем сервер
constexpr int64_t  kGraceSec     = 24 * 3600;        // сколько живём без связи с сервером
constexpr int      kMaxGraceBoots = 3;               // сколько раз подряд разрешён офлайн-старт

// ─────────────────────────── пути и обфусцированные строки ──────────────────

const char* kLicDir()  { static constexpr auto s = xp::_mk("/storage/emulated/0/benzware/"); return s.d(); }
std::string KeyFile()   { return std::string(kLicDir()) + ".lic"; }
std::string StateFile() { return std::string(kLicDir()) + ".lst"; }

// ────────────────────────────────── лог ──────────────────────────────────────

void logi(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("[lic] %s\n", buf);
    fflush(stdout);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "benzware", "[lic] %s", buf);
#endif
}

// ──────────────────────────────── SHA-256 ────────────────────────────────────

struct Sha256 {
    uint32_t h[8]; uint64_t total; uint8_t blk[64]; size_t n;
    Sha256() { reset(); }
    void reset() {
        static const uint32_t iv[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                                       0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
        memcpy(h, iv, sizeof iv); total = 0; n = 0;
    }
    static uint32_t rotr(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
    void chunk(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4*i])<<24)|(uint32_t(p[4*i+1])<<16)|(uint32_t(p[4*i+2])<<8)|uint32_t(p[4*i+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch = (e&f)^(~e&g);
            uint32_t t1 = hh+S1+ch+K[i]+w[i];
            uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t mj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0+mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += len;
        while (len) {
            size_t take = 64 - n; if (take > len) take = len;
            memcpy(blk + n, p, take); n += take; p += take; len -= take;
            if (n == 64) { chunk(blk); n = 0; }
        }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = total * 8;
        uint8_t one = 0x80; update(&one, 1);
        uint8_t z = 0; while (n != 56) update(&z, 1);
        uint8_t lb[8]; for (int i = 0; i < 8; ++i) lb[i] = uint8_t(bits >> (56 - 8*i));
        update(lb, 8);
        for (int i = 0; i < 8; ++i) {
            out[4*i]   = uint8_t(h[i] >> 24); out[4*i+1] = uint8_t(h[i] >> 16);
            out[4*i+2] = uint8_t(h[i] >> 8);  out[4*i+3] = uint8_t(h[i]);
        }
    }
};

std::string hex_of(const uint8_t* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s; s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) { s[2*i] = x[d[i] >> 4]; s[2*i+1] = x[d[i] & 15]; }
    return s;
}

std::string sha256_hex(const void* d, size_t n) {
    Sha256 s; s.update(d, n); uint8_t o[32]; s.final(o);
    return hex_of(o, sizeof o);
}

std::string hmac_hex(const std::string& key, const void* data, size_t n) {
    uint8_t k[64] = {};
    if (key.size() > 64) { Sha256 s; s.update(key.data(), key.size()); s.final(k); }
    else memcpy(k, key.data(), key.size());
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = uint8_t(k[i] ^ 0x36); opad[i] = uint8_t(k[i] ^ 0x5c); }
    Sha256 s1; s1.update(ipad, 64); s1.update(data, n); uint8_t inner[32]; s1.final(inner);
    Sha256 s2; s2.update(opad, 64); s2.update(inner, 32); uint8_t o[32]; s2.final(o);
    return hex_of(o, sizeof o);
}

// ──────────────────────────────── CRC-32 ─────────────────────────────────────

uint32_t crc32_feed(uint32_t crc, const uint8_t* d, size_t n) {
    static uint32_t tbl[256]; static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = tbl[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ─────────────────────── самопроверка целостности ELF ────────────────────────
//
// В .data зашит 16-байтовый слот: 4 байта магии + 12-байтовый тег.
// tools/protect/patch_crc.py при сборке заменяет магию на CRC32 всего файла
// (со слотом, занулённым при подсчёте). Пропатчил байт — CRC не сошёлся.
// Маркер не должен оптимизироваться/выкидываться: volatile + ссылка из кода.

volatile const uint8_t kCrcSlot[16] = {
    0xDE, 0xAD, 0xBE, 0xEF, 'X','V','C','R','C','P','A','T','C','H','4','2'
};

enum SelfRes { SELF_OK, SELF_UNPATCHED, SELF_TAMPERED };

__attribute__((noinline)) SelfRes SelfCheck() {
    // Касание слота — чтобы линкер не выкинул его как мёртвые данные.
    volatile uint8_t sink = kCrcSlot[0]; (void)sink;

    // Тег собираем XOR-ом, а не литералом: открытая ASCII-последовательность
    // попала бы в .rodata самой константой, и скан находил бы два вхождения —
    // слот в .data и её саму (поймано отладкой: tag лежал и по 0x88a0 в
    // .rodata). XOR-таблица оставляет в файле только слот.
    uint8_t TAG[12];
    {
        static const uint8_t enc[12] = {
            0xFD, 0xF3, 0xE6, 0xF7, 0xE6, 0xF5, 0xE4, 0xF1, 0xE6, 0xED, 0x91, 0x97
        };
        for (int i = 0; i < 12; ++i) TAG[i] = uint8_t(enc[i] ^ 0xA5); // "XVCRCPATCH42"
    }
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0) return SELF_UNPATCHED;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || lseek(fd, 0, SEEK_SET) != 0) { close(fd); return SELF_UNPATCHED; }

    // Читаем бинарь целиком: он весит мегабайты, сканить стримингом — только
    // шанс накосячить с перекрытием буферов (проверено тестом, ловил границу).
    std::vector<uint8_t> f;
    f.resize(static_cast<size_t>(sz));
    size_t got = 0;
    while (got < f.size()) {
        ssize_t r = read(fd, f.data() + got, f.size() - got);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return SELF_UNPATCHED; }
        if (r == 0) break;
        got += size_t(r);
    }
    close(fd);
    if (got != f.size()) return SELF_UNPATCHED;

    int64_t hit = -1; int hits = 0;
    for (size_t i = 0; i + 12 <= f.size(); ++i) {
        if (memcmp(f.data() + i, TAG, 12) == 0) {
            hit = int64_t(i) - 4; // 4 байта CRC/магии прямо перед тегом
            if (++hits > 1) return SELF_UNPATCHED; // неоднозначно — пропускаем
        }
    }
    if (hits == 0 || hit < 0) return SELF_UNPATCHED;          // слота нет — dev-сборка
    if (f[hit] == 0xDE && f[hit+1] == 0xAD && f[hit+2] == 0xBE && f[hit+3] == 0xEF)
        return SELF_UNPATCHED; // слот нетронут: сборка без CRC-патча (NOCRC=1)

    uint32_t expect = uint32_t(f[hit]) | (uint32_t(f[hit+1]) << 8) |
                      (uint32_t(f[hit+2]) << 16) | (uint32_t(f[hit+3]) << 24);
    memset(f.data() + hit, 0, 16); // слот считаем нулями — как patch_crc.py
    return crc32_feed(0, f.data(), f.size()) == expect ? SELF_OK : SELF_TAMPERED;
}

// ─────────────────────────────── анти-отладка ────────────────────────────────
// От ленивых: TracerPid, frida (дефолтный порт/маппинги). Переименованная
// frida это не поймает — честно сказано в PROTECTION.md.

bool file_contains(const char* path, const char* needle) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char buf[4096]; std::string acc; acc.reserve(8192);
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        acc.append(buf, size_t(r));
        if (acc.size() > 256 * 1024) break;
    }
    close(fd);
    return acc.find(needle) != std::string::npos;
}

bool tracer_attached() {
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return false;
    char buf[2048]; ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return false;
    buf[r] = 0;
    const char* p = strstr(buf, "TracerPid:");
    if (!p) return false;
    return atoi(p + 10) != 0;
}

bool frida_seen() {
    // 27042 = 0x69A2 — дефолтный порт frida-server; в /proc/net/tcp* hex upper-case.
    if (file_contains("/proc/net/tcp",  ":69A2")) return true;
    if (file_contains("/proc/net/tcp6", ":69A2")) return true;
    if (file_contains("/proc/self/maps", "frida"))  return true;
    if (file_contains("/proc/self/maps", "gadget")) return true;
    return false;
}

// ──────────────────────────────── HTTP-клиент ────────────────────────────────
// Минимум над сокетами: root-бинарю INTERNET-пермишен не нужен.
// Подделку ответов закрывает HMAC-подпись тела (проверяется ниже), MITM может
// лишь «заглушить» сервер — тогда сработает grace и останов.

bool http_post(const std::string& host, uint16_t port, const char* path,
               const std::string& req_body, std::string& out_body, std::string& out_sig) {
    char port_str[8]; snprintf(port_str, sizeof port_str, "%u", port);
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) return false;

    int fd = -1;
    for (addrinfo* ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        // connect с таймаутом 8с через O_NONBLOCK+poll
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) { close(fd); fd = -1; continue; }
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, 8000) != 1) { close(fd); fd = -1; continue; }
            int err = 0; socklen_t el = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err != 0) { close(fd); fd = -1; continue; }
        }
        fcntl(fd, F_SETFL, fl);
    }
    freeaddrinfo(res);
    if (fd < 0) return false;

    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    std::string req;
    req.reserve(256 + req_body.size());
    req += "POST "; req += path; req += " HTTP/1.1\r\nHost: ";
    req += host; req += "\r\nUser-Agent: bzw/1\r\nContent-Type: application/json\r\nContent-Length: ";
    req += std::to_string(req_body.size());
    req += "\r\nConnection: close\r\n\r\n";
    req += req_body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t w = send(fd, req.data() + sent, req.size() - sent, 0);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return false; }
        sent += size_t(w);
    }

    std::string raw; raw.reserve(4096);
    for (;;) {
        char buf[4096];
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return false; }
        if (r == 0) break;
        raw.append(buf, size_t(r));
        if (raw.size() > 65536) { close(fd); return false; }
    }
    close(fd);

    size_t he = raw.find("\r\n\r\n");
    if (he == std::string::npos) return false;
    int code = 0;
    sscanf(raw.c_str(), "HTTP/%*d.%*d %d", &code);
    if (code != 200) return false;

    std::string hdrs = raw.substr(0, he);
    for (auto& c : hdrs) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    size_t sp = hdrs.find("\r\nx-sig:");
    if (sp != std::string::npos) {
        size_t vs = sp + 8;
        while (vs < hdrs.size() && (hdrs[vs] == ' ' || hdrs[vs] == '\t')) ++vs;
        size_t ve = hdrs.find("\r\n", vs);
        out_sig = hdrs.substr(vs, ve == std::string::npos ? ve : ve - vs);
    }
    out_body = raw.substr(he + 4);
    return !out_body.empty();
}

// ─────────────────────────────── мини-JSON ───────────────────────────────────
// Парсим только плоские ответы своего сервера: "ok":false, "ts":123,
// "expires":0, "session":123, "reason":"killed". Сигнатура проверена ДО этого,
// так что кривой JSON может прислать только сам сервер.

bool json_bool(const std::string& j, const char* name, bool& v) {
    std::string needle = std::string("\"") + name + "\"";
    size_t p = j.find(needle); if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size()); if (p == std::string::npos) return false;
    ++p; while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (j.compare(p, 4, "true") == 0)  { v = true;  return true; }
    if (j.compare(p, 5, "false") == 0) { v = false; return true; }
    return false;
}

bool json_u64(const std::string& j, const char* name, uint64_t& v) {
    std::string needle = std::string("\"") + name + "\"";
    size_t p = j.find(needle); if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size()); if (p == std::string::npos) return false;
    ++p;
    if (p >= j.size() || j[p] < '0' || j[p] > '9') return false;
    v = strtoull(j.c_str() + p, nullptr, 10);
    return true;
}

bool json_str(const std::string& j, const char* name, std::string& v) {
    std::string needle = std::string("\"") + name + "\"";
    size_t p = j.find(needle); if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size()); if (p == std::string::npos) return false;
    p = j.find('"', p + 1); if (p == std::string::npos) return false;
    size_t e = j.find('"', p + 1); if (e == std::string::npos) return false;
    v = j.substr(p + 1, e - p - 1);
    return true;
}

// ─────────────────────────────── состояние ───────────────────────────────────

std::atomic<bool>     g_dead{false};
std::atomic<uint32_t> g_session{0};
std::string           g_key, g_hwid, g_secret;
uint64_t              g_expires  = 0;       // unix-сек, 0 = бессрочно
int64_t               g_last_srv = 0;       // последний серверный ts
std::chrono::steady_clock::time_point g_last_srv_mono;
std::atomic<bool>     g_have_srv{false};
int                   g_hb_sec   = kHeartbeatSec; // BZHB — отладочный оверрайд интервала

// Оценка «серверного сейчас»: ts сервера + монотонное время с последнего ответа
// (перевод часов устройства нам не страшен).
int64_t srv_now_est() {
    if (!g_have_srv.load()) return 0;
    auto d = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_last_srv_mono).count();
    return g_last_srv + (d > 0 ? d : 0);
}

void note_srv_ok(uint64_t ts, uint64_t expires, uint32_t session) {
    if (int64_t(ts) + 120 >= g_last_srv) { // анти-replay: время назад не принимаем
        g_last_srv = int64_t(ts);
        g_last_srv_mono = std::chrono::steady_clock::now();
        g_have_srv.store(true);
        g_expires = expires;
        g_session.store(session);
    }
}

// ───────────────────────── завершение по kill/ban ────────────────────────────

__attribute__((noinline)) void remote_kill(const char* why) {
    bool expected = false;
    if (!g_dead.compare_exchange_strong(expected, true)) return; // уже умираем
    logi("СТОП: %s — процесс завершается", why);
    main_thread_flag.store(false);
    // Второй рубеж на случай, если главный цикл пропатчили: выход с
    // недетерминированной задержкой из отдельного потока.
    std::thread([]() {
        unsigned ms = 2000 + unsigned(rand()) % 10000;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        _exit(0);
    }).detach();
}

[[noreturn]] __attribute__((noinline)) void die_now(const char* why) {
    printf("\n[lic] ОТКАЗ: %s\n\n", why);
    fflush(stdout);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "benzware", "[lic] ОТКАЗ: %s", why);
#endif
    _exit(1);
}

// ──────────────────────── HWID (привязка к устройству) ───────────────────────

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string shell_out(const char* cmd) {
    FILE* p = popen(cmd, "r");
    if (!p) return "";
    std::string r; char buf[128];
    while (fgets(buf, sizeof buf, p)) r += buf;
    pclose(p);
    return trim(r);
}

std::string make_hwid() {
    std::string aid = shell_out("/system/bin/settings get secure android_id");
    if (aid == "null") aid.clear();
    char serial[128] = {};
    __system_property_get("ro.serialno", serial);
    std::string raw = aid + "|" + serial;
    if (raw == "|") { // совсем фолбэк: машинный id загрузки (меняется при ребуте — только край)
        return sha256_hex("fallback", 8).substr(0, 24);
    }
    return sha256_hex(raw.data(), raw.size()).substr(0, 24);
}

// ─────────────────────────── grace-кэш (офлайн-старт) ─────────────────────────
// Последний успешный ответ серверa складываем в .lst с HMAC: файл не подделать
// без секрета. Даёт старт при лежащем сервере, но не более kMaxGraceBoots раз
// подряд и не дольше kGraceSec. Серверный контакт счётчик сбрасывает.

void cache_write(int grace_boots) {
    mkdir(kLicDir(), 0777);
    int64_t wall = int64_t(time(nullptr));
    std::string keysha = sha256_hex(g_key.data(), g_key.size());
    char meta[256];
    snprintf(meta, sizeof meta, "%s|%s|%lld|%llu|%lld|%d",
             keysha.c_str(), g_hwid.c_str(), (long long)g_last_srv,
             (unsigned long long)g_expires, (long long)wall, grace_boots);
    std::string mac = hmac_hex(g_secret, meta, strlen(meta));
    FILE* f = fopen(StateFile().c_str(), "w");
    if (!f) return;
    fprintf(f, "%s|%s\n", meta, mac.c_str());
    fclose(f);
    chmod(StateFile().c_str(), 0600);
}

bool cache_allows_offline() {
    FILE* f = fopen(StateFile().c_str(), "r");
    if (!f) return false;
    char line[512] = {};
    if (!fgets(line, sizeof line, f)) { fclose(f); return false; }
    fclose(f);
    std::string s = trim(line);

    char keysha[80] = {}, hwid[80] = {};
    long long srv = 0, wall = 0; unsigned long long exp = 0; int boots = 0;
    // формат: keysha|hwid|srv|exp|wall|boots|mac
    std::string body = s.substr(0, s.rfind('|'));
    std::string gotmac = s.substr(s.rfind('|') + 1);
    if (hmac_hex(g_secret, body.data(), body.size()) != gotmac) return false;
    if (sscanf(body.c_str(), "%79[^|]|%79[^|]|%lld|%llu|%lld|%d",
               keysha, hwid, &srv, &exp, &wall, &boots) != 6) return false;
    if (keysha != sha256_hex(g_key.data(), g_key.size())) return false;
    if (hwid != g_hwid) return false;
    if (boots >= kMaxGraceBoots) return false;

    int64_t now_wall = int64_t(time(nullptr));
    int64_t elapsed = now_wall > wall ? now_wall - wall : 0;
    if (elapsed > kGraceSec) return false;
    int64_t srv_est = srv + elapsed;
    if (exp != 0 && srv_est > int64_t(exp)) return false;

    g_last_srv = srv; g_last_srv_mono = std::chrono::steady_clock::now();
    g_have_srv.store(true);
    g_expires = exp;
    cache_write(boots + 1); // расходуем одну офлайн-попытку
    return true;
}

// ─────────────────────────── обращение к серверу ─────────────────────────────

enum class Srv { OK, DENY, NET };

__attribute__((noinline)) Srv call(const char* path, char* reason, size_t reason_cap) {
    char body[640];
    snprintf(body, sizeof body, "{\"key\":\"%s\",\"hwid\":\"%s\",\"build\":%u}",
             g_key.c_str(), g_hwid.c_str(), kBuildId);
    std::string resp, sig;
    if (!http_post(S_HOST(), kPort, path, body, resp, sig)) return Srv::NET;
    if (sig.empty() || sig != hmac_hex(g_secret, resp.data(), resp.size())) {
        logi("ответ без валидной подписи (подмена?) — считаем сеть мёртвой");
        return Srv::NET; // неподписанный deny не честим: это мог быть MITM
    }
    bool ok = false;
    if (json_bool(resp, "ok", ok) && ok) {
        uint64_t ts = 0, exp = 0, sess = 0;
        json_u64(resp, "ts", ts);
        json_u64(resp, "expires", exp);
        json_u64(resp, "session", sess);
        note_srv_ok(ts, exp, uint32_t(sess));
        if (int64_t(ts) + 120 < g_last_srv) return Srv::NET; // replay старого ответа
        return Srv::OK;
    }
    std::string r = "denied";
    json_str(resp, "reason", r);
    snprintf(reason, reason_cap, "%s", r.c_str());
    return Srv::DENY;
}

const char* reason_ru(const char* r) {
    if (!strcmp(r, "killed"))   { static constexpr auto s = xp::_mk("доступ отключён разработчиком"); return s.d(); }
    if (!strcmp(r, "banned"))   { static constexpr auto s = xp::_mk("ключ заблокирован"); return s.d(); }
    if (!strcmp(r, "hwid"))     { static constexpr auto s = xp::_mk("ключ привязан к другому устройству"); return s.d(); }
    if (!strcmp(r, "expired"))  { static constexpr auto s = xp::_mk("срок действия ключа истёк"); return s.d(); }
    if (!strcmp(r, "update"))   { static constexpr auto s = xp::_mk("требуется обновление сборки"); return s.d(); }
    if (!strcmp(r, "bad_key"))  { static constexpr auto s = xp::_mk("неверный ключ лицензии"); return s.d(); }
    static constexpr auto d = xp::_mk("отказ сервера"); return d.d();
}

// ────────────────────────── фоновые потоки ───────────────────────────────────

void heartbeat_loop() {
    char reason[64];
    int beats = 0;
    while (!g_dead.load()) {
        for (int i = 0; i < g_hb_sec && !g_dead.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        if (tracer_attached() || frida_seen()) { remote_kill("обнаружен отладчик/инъекция"); continue; }
        if (++beats % 10 == 0 && SelfCheck() == SELF_TAMPERED) { remote_kill("бинарь изменён"); continue; }

        switch (call("/api/heartbeat", reason, sizeof reason)) {
        case Srv::OK:
            cache_write(0);
            break;
        case Srv::DENY:
            remote_kill(reason_ru(reason));
            break;
        case Srv::NET: {
            int64_t since_ok = g_have_srv.load()
                ? std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - g_last_srv_mono).count()
                : kGraceSec + 1;
            if (since_ok > kGraceSec) remote_kill("сервер недоступен слишком долго");
            break;
        }
        }
    }
}

void expiry_loop() {
    while (!g_dead.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (g_expires != 0 && g_have_srv.load() && srv_now_est() > int64_t(g_expires) + 60)
            remote_kill("срок действия ключа истёк");
    }
}

// ─────────────────────────────── ключ ────────────────────────────────────────

std::string load_key() {
    const char* env = getenv("BZKEY"); // удобно для теста: BZKEY=... ./benzware.bin
    if (env && trim(env).size() >= 8) return trim(env);
    FILE* f = fopen(KeyFile().c_str(), "r");
    if (!f) {
        mkdir(kLicDir(), 0777);
        f = fopen(KeyFile().c_str(), "w");
        if (f) fclose(f);
        return "";
    }
    char buf[128] = {};
    std::string k = fgets(buf, sizeof buf, f) ? trim(buf) : "";
    fclose(f);
    return k;
}

} // namespace

// ─────────────────────────────── public API ──────────────────────────────────

__attribute__((noinline)) void Gate() {
    srand(unsigned(time(nullptr)) ^ unsigned(getpid()));
    g_secret = S_SECRET();

    switch (SelfCheck()) {
    case SELF_TAMPERED:  die_now("бинарь изменён (проверка целостности)");
    case SELF_UNPATCHED: logi("самопроверка пропущена (dev-сборка, нет CRC-патча)"); break;
    case SELF_OK:        logi("целостность: OK");
    }
    if (tracer_attached()) die_now("обнаружен отладчик");

    if (const char* hb = getenv("BZHB")) { // отладка: BZHB=5 — heartbeat раз в 5с
        int v = atoi(hb);
        if (v >= 5 && v <= 3600) g_hb_sec = v;
    }

    g_hwid = make_hwid();
    g_key  = load_key();
    if (g_key.size() < 8) {
        printf("\n[lic] Нужен ключ лицензии.\n[lic] Положи ключ в файл: %s.lic\n"
               "[lic]   echo \"ВАШ_КЛЮЧ\" > %s.lic\n\n", kLicDir(), kLicDir());
        fflush(stdout);
        _exit(1);
    }

    char reason[64] = {};
    Srv r = Srv::NET;
    for (int attempt = 0; attempt < 2 && r == Srv::NET; ++attempt) {
        r = call("/api/verify", reason, sizeof reason);
        if (r == Srv::NET && attempt == 0) std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (r == Srv::DENY) die_now(reason_ru(reason));
    if (r == Srv::NET) {
        if (cache_allows_offline()) logi("сервер недоступен — офлайн-старт по кэшу");
        else                        die_now("сервер лицензий недоступен (и офлайн-лимит исчерпан)");
    } else {
        cache_write(0);
    }

    logi("лицензия принята%s", g_expires ? " (с ограничением срока)" : "");
    std::thread(heartbeat_loop).detach();
    std::thread(expiry_loop).detach();
}

bool Dead() { return g_dead.load(); }
uint32_t SessionMask() { return g_session.load(); }

} // namespace license
