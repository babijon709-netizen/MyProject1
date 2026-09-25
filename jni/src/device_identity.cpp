#include "device_identity.h"
#include <sys/system_properties.h>
#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92 // Android system property value buffer size
#endif
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
uint32_t ror(uint32_t value, int n) { return (value >> n) | (value << (32 - n)); }

std::string property(const char* key) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(key, value) <= 0) return {};
    return value;
}

// Use a fixed absolute system executable (no shell/PATH). A timeout prevents
// Android's settings service from hanging application startup indefinitely.
std::string settings(const char* scope, const char* key) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        char* const args[] = {const_cast<char*>("settings"), const_cast<char*>("get"),
                             const_cast<char*>(scope), const_cast<char*>(key), nullptr};
        execv("/system/bin/settings", args);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); return {}; }
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
    std::string text;
    int status = 0;
    bool exited = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        char buf[128];
        ssize_t count;
        while ((count = read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (text.size() + static_cast<size_t>(count) > 160) { text.clear(); break; }
            text.append(buf, static_cast<size_t>(count));
        }
        if (waitpid(pid, &status, WNOHANG) == pid) { exited = true; break; }
        pollfd pfd{pipefd[0], POLLIN, 0};
        poll(&pfd, 1, 100);
    }
    if (!exited) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    } else {
        char buf[128];
        ssize_t count;
        while ((count = read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (text.size() + static_cast<size_t>(count) > 160) { text.clear(); break; }
            text.append(buf, static_cast<size_t>(count));
        }
    }
    close(pipefd[0]);
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text == "null" ? "" : text;
}

std::string json_string(const std::string& source) {
    std::string encoded = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : source) {
        if (c == '"' || c == '\\') { encoded += '\\'; encoded += char(c); }
        else if (c < 32) { encoded += "\\u00"; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
        else encoded += char(c);
    }
    return encoded + '"';
}
} // namespace

std::string DeviceSha256Hex(const std::string& input) {
    std::array<uint32_t, 8> h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::vector<uint8_t> bytes(input.begin(), input.end());
    uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (int i = 7; i >= 0; --i) bytes.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    for (size_t offset = 0; offset < bytes.size(); offset += 64) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            size_t p = offset + static_cast<size_t>(i) * 4;
            w[i] = (uint32_t(bytes[p]) << 24) | (uint32_t(bytes[p+1]) << 16)
                 | (uint32_t(bytes[p+2]) << 8) | uint32_t(bytes[p+3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], z=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = z + (ror(e,6) ^ ror(e,11) ^ ror(e,25))
                          + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (ror(a,2) ^ ror(a,13) ^ ror(a,22))
                          + ((a & b) ^ (a & c) ^ (b & c));
            z=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=z;
    }
    std::string output;
    const char* hex = "0123456789abcdef";
    for (uint32_t word : h) for (int i = 7; i >= 0; --i) output += hex[(word >> (i*4)) & 15];
    return output;
}

bool GetDeviceIdentity(DeviceIdentity& out) {
    std::string id = settings("secure", "android_id");
    bool valid = id.size() >= 8 && id.size() <= 32;
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) valid = false;
    if (!valid) {
        id = property("ro.boot.serialno");
        if (id.empty() || id.size() > 64 || id == "unknown") return false;
    }
    out.hwid = DeviceSha256Hex("benzware:device:v1:" + id);
    out.name = settings("global", "device_name");
    if (out.name.empty() || out.name.size() > 80) {
        out.name = property("ro.product.manufacturer") + " " + property("ro.product.model");
    }
    if (out.name.find_first_not_of(" \t\r\n") == std::string::npos) out.name = "Android device";
    if (out.name.size() > 80) {
        size_t end = 80;
        while (end > 0 && (static_cast<unsigned char>(out.name[end]) & 0xc0) == 0x80) --end;
        out.name.resize(end);
    }
    return true;
}

std::string DeviceIdentityJson(const DeviceIdentity& identity, bool register_device,
                               const std::string& nonce) {
    return "{\"hwid\":" + json_string(identity.hwid) + ",\"name\":"
         + json_string(identity.name) + ",\"register\":" + (register_device ? "true" : "false")
         + ",\"nonce\":" + json_string(nonce) + "}";
}
