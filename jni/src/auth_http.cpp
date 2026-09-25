#include "auth_http.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <sys/socket.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace {
#ifndef BENZWARE_CA_DIR1
#define BENZWARE_CA_DIR1 "/apex/com.android.conscrypt/cacerts"
#endif
#ifndef BENZWARE_CA_DIR2
#define BENZWARE_CA_DIR2 "/system/etc/security/cacerts"
#endif

struct Tls {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_context entropy;
    Tls() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&ca);
        mbedtls_ctr_drbg_init(&rng);
        mbedtls_entropy_init(&entropy);
    }
    ~Tls() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&ca);
        mbedtls_ctr_drbg_free(&rng);
        mbedtls_entropy_free(&entropy);
        mbedtls_net_free(&net);
    }
};

bool endpoint(const std::string& url, std::string& host, std::string& port,
              std::string& authority) {
    if (url.compare(0, 8, "https://") != 0) return false;
    const auto slash = url.find('/', 8);
    if (slash == std::string::npos || url.substr(slash) != "/auth") return false;
    authority = url.substr(8, slash - 8);
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
    port = colon == std::string::npos ? "443" : authority.substr(colon + 1);
    if (host.empty() || host.size() > 253 || port.empty() || port.size() > 5 ||
        !std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-';
        }) || !std::all_of(port.begin(), port.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        })) return false;
    const int port_number = std::stoi(port);
    return port_number > 0 && port_number <= 65535;
}

std::string lowercase(std::string input) {
    for (char& c : input) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return input;
}

// HTTP/1.1 can use Content-Length, chunked transfer encoding, or connection
// close. Never accept a partial response as a successful authorization.
bool responseBody(const std::string& response, int& status, std::string& body) {
    const auto end = response.find("\r\n\r\n");
    if (end == std::string::npos || end > 12000 || response.compare(0, 9, "HTTP/1.1 ") != 0 ||
        end < 12 || !std::isdigit(static_cast<unsigned char>(response[9])) ||
        !std::isdigit(static_cast<unsigned char>(response[10])) ||
        !std::isdigit(static_cast<unsigned char>(response[11]))) return false;
    status = (response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0';
    std::string length;
    bool chunked = false;
    for (size_t i = response.find("\r\n") + 2; i < end;) {
        const auto next = response.find("\r\n", i);
        if (next == std::string::npos || next > end) return false;
        const auto colon = response.find(':', i);
        if (colon != std::string::npos && colon < next) {
            const auto key = lowercase(response.substr(i, colon - i));
            const auto value = lowercase(response.substr(colon + 1, next - colon - 1));
            if (key == "content-length") length = value;
            if (key == "transfer-encoding" && value.find("chunked") != std::string::npos)
                chunked = true;
        }
        i = next + 2;
    }
    size_t pos = end + 4;
    if (chunked) {
        while (pos < response.size()) {
            const auto line = response.find("\r\n", pos);
            if (line == std::string::npos || line - pos > 20) return false;
            size_t size = 0;
            for (size_t i = pos; i < line && response[i] != ';'; ++i) {
                const unsigned char c = response[i];
                const int n = c >= '0' && c <= '9' ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (n < 0 || size > 256) return false;
                size = (size << 4) + static_cast<size_t>(n);
            }
            pos = line + 2;
            if (size == 0) return true;
            if (size > 256 || pos + size + 2 > response.size() ||
                response.compare(pos + size, 2, "\r\n") != 0) return false;
            body.append(response, pos, size);
            if (body.size() > 256) return false;
            pos += size + 2;
        }
        return false;
    }
    if (!length.empty()) {
        // Spaces around a numeric Content-Length are permitted.
        length.erase(0, length.find_first_not_of(' '));
        const auto last = length.find_last_not_of(' ');
        if (last == std::string::npos) return false;
        length.erase(last + 1);
        if (length.empty() || length.size() > 3 ||
            !std::all_of(length.begin(), length.end(), [](unsigned char c) {
                return c >= '0' && c <= '9';
            })) return false;
        const auto n = static_cast<size_t>(std::stoul(length));
        if (n > 256 || response.size() - pos < n) return false;
        body = response.substr(pos, n);
    } else {
        body = response.substr(pos);
    }
    return body.size() <= 256;
}
} // namespace

AuthHttpResult PostDeviceAuth(const std::string& url, const std::string& body,
                              const std::string& nonce) {
    std::string host, port, authority;
    if (!endpoint(url, host, port, authority) || body.size() > 512) return AuthHttpResult::denied;
    Tls tls;
    static const unsigned char personal[] = "benzware-auth-v1";
    if (mbedtls_ctr_drbg_seed(&tls.rng, mbedtls_entropy_func, &tls.entropy,
                              personal, sizeof(personal) - 1) != 0) return AuthHttpResult::unavailable;
    // Android 14+ uses Conscrypt APEX, older releases use /system. Never
    // disable verification just because a store is unavailable or empty.
    mbedtls_x509_crt_parse_path(&tls.ca, BENZWARE_CA_DIR1);
    mbedtls_x509_crt_parse_path(&tls.ca, BENZWARE_CA_DIR2);
    if (!tls.ca.raw.p ||
        mbedtls_ssl_config_defaults(&tls.config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return AuthHttpResult::unavailable;
    mbedtls_ssl_conf_authmode(&tls.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls.config, &tls.ca, nullptr);
    mbedtls_ssl_conf_rng(&tls.config, mbedtls_ctr_drbg_random, &tls.rng);
    if (mbedtls_net_connect(&tls.net, host.c_str(), port.c_str(), MBEDTLS_NET_PROTO_TCP) != 0)
        return AuthHttpResult::unavailable;
    timeval timeout{8, 0};
    setsockopt(tls.net.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(tls.net.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (mbedtls_ssl_setup(&tls.ssl, &tls.config) != 0 ||
        mbedtls_ssl_set_hostname(&tls.ssl, host.c_str()) != 0)
        return AuthHttpResult::unavailable;
    mbedtls_ssl_set_bio(&tls.ssl, &tls.net, mbedtls_net_send, mbedtls_net_recv, nullptr);
    if (mbedtls_ssl_handshake(&tls.ssl) != 0 || mbedtls_ssl_get_verify_result(&tls.ssl) != 0)
        return AuthHttpResult::unavailable;
    const std::string request = "POST /auth HTTP/1.1\r\nHost: " + authority
        + "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
          "Accept-Encoding: identity\r\nConnection: close\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < request.size()) {
        const int n = mbedtls_ssl_write(&tls.ssl,
            reinterpret_cast<const unsigned char*>(request.data() + sent), request.size() - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return AuthHttpResult::unavailable;
        sent += static_cast<size_t>(n);
    }
    std::string response;
    while (response.size() < 16384) {
        unsigned char buffer[1024];
        const int n = mbedtls_ssl_read(&tls.ssl, buffer, sizeof(buffer));
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (n < 0) return AuthHttpResult::unavailable;
        response.append(reinterpret_cast<const char*>(buffer), static_cast<size_t>(n));
    }
    if (response.size() >= 16384) return AuthHttpResult::unavailable;
    int status = 0;
    std::string content;
    if (!responseBody(response, status, content)) return AuthHttpResult::unavailable;
    if (status == 200) {
        if (content == "enabled:" + nonce + "\n") return AuthHttpResult::allowed;
        return AuthHttpResult::denied; // Bad or replayed nonce, or explicit disabled.
    }
    if (status == 400 || status == 401 || status == 403 || status == 404)
        return AuthHttpResult::denied;
    return AuthHttpResult::unavailable;
}
