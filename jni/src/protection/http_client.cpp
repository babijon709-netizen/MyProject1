#include "http_client.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <ctime>

namespace prot {
namespace http {

std::string UrlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size()*2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c=='-'||c=='_'||c=='.'||c=='~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[c>>4]);
            out.push_back(hex[c&15]);
        }
    }
    return out;
}

std::string GetHWID() {
    // Try to get android id from properties, fallback to random persistent file
    char buf[128] = {0};
    // Try /data/local/tmp/hwid cache
    FILE* f = fopen("/data/local/tmp/.cheat_hwid", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            std::string s(buf);
            // trim
            s.erase(s.find_last_not_of("\r\n") + 1);
            if (s.size() >= 8) return s;
        }
        fclose(f);
    }
    // Try getprop ro.serialno or android_id via system property (need __system_property_get)
    // For simplicity generate random and save
    // Use /proc/sys/kernel/random/uuid
    FILE* fu = fopen("/proc/sys/kernel/random/uuid", "r");
    if (fu) {
        if (fgets(buf, sizeof(buf), fu)) {
            fclose(fu);
            std::string s(buf);
            s.erase(s.find_last_not_of("\r\n") + 1);
            // save
            FILE* fw = fopen("/data/local/tmp/.cheat_hwid", "w");
            if (fw) { fputs(s.c_str(), fw); fclose(fw); }
            return s;
        }
        fclose(fu);
    }
    // fallback: time + pid hash
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "hwid-%d-%ld", getpid(), time(nullptr));
    return std::string(tmp);
}

static std::string exec_command(const std::string& cmd, int timeout_sec) {
    // Use popen with timeout via shell
    std::string result;
    // Add timeout to command if possible
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    
    // Set non-blocking with timeout
    int fd = fileno(pipe);
    // Simple read with deadline
    char buffer[1024];
    time_t start = time(nullptr);
    while (time(nullptr) - start < timeout_sec) {
        if (fgets(buffer, sizeof(buffer), pipe)) {
            result += buffer;
            if (result.size() > 8192) break; // limit
        } else {
            // Check if EOF
            if (feof(pipe)) break;
            usleep(100*1000);
        }
    }
    pclose(pipe);
    return result;
}

// Try curl binary
static std::string try_curl(const std::string& url, int timeout_sec) {
    // Check multiple curl locations
    const char* curl_paths[] = {"/system/bin/curl", "/system/xbin/curl", "/data/local/tmp/curl", "curl", nullptr};
    for (int i=0; curl_paths[i]; ++i) {
        // test if exists (for absolute paths)
        if (curl_paths[i][0]=='/' && access(curl_paths[i], X_OK)!=0) continue;
        char cmd[1024];
        // -s silent, -m timeout, -k insecure (for self-signed), -L follow, --max-filesize 8k
        snprintf(cmd, sizeof(cmd), "%s -s -k -L -m %d --max-filesize 8192 \"%s\" 2>/dev/null", curl_paths[i], timeout_sec, url.c_str());
        std::string out = exec_command(cmd, timeout_sec+1);
        if (!out.empty() && out.find("enabled") != std::string::npos) {
            return out;
        }
        // Even if no enabled, if got JSON with {, return
        if (!out.empty() && out.find("{") != std::string::npos) return out;
    }
    return "";
}

static std::string try_wget(const std::string& url, int timeout_sec) {
    const char* wget_paths[] = {"/system/bin/wget", "/system/xbin/wget", "wget", nullptr};
    for (int i=0; wget_paths[i]; ++i) {
        if (wget_paths[i][0]=='/' && access(wget_paths[i], X_OK)!=0) continue;
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "%s -qO- --timeout=%d --no-check-certificate \"%s\" 2>/dev/null", wget_paths[i], timeout_sec, url.c_str());
        std::string out = exec_command(cmd, timeout_sec+1);
        if (!out.empty() && out.find("enabled") != std::string::npos) return out;
        if (!out.empty() && out.find("{") != std::string::npos) return out;
    }
    return "";
}

// Raw socket HTTP (only http://, not https)
static std::string try_raw_http(const std::string& url, int timeout_sec) {
    // Only support http://
    if (url.rfind("http://", 0) != 0) return "";
    
    // Parse http://host:port/path
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    
    std::string host = hostport;
    std::string port = "80";
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = hostport.substr(colon+1);
    }

    struct addrinfo hints{}, *res=nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (err != 0 || !res) return "";

    int sock = -1;
    for (struct addrinfo* p=res; p; p=p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        // Set timeout
        struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        // Non-blocking connect with timeout
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        int conn = connect(sock, p->ai_addr, p->ai_addrlen);
        if (conn < 0 && errno != EINPROGRESS) { close(sock); sock=-1; continue; }
        if (conn != 0) {
            fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
            struct timeval ctv; ctv.tv_sec = timeout_sec; ctv.tv_usec = 0;
            int sel = select(sock+1, nullptr, &wfds, nullptr, &ctv);
            if (sel <= 0) { close(sock); sock=-1; continue; }
            int so_err=0; socklen_t len=sizeof(so_err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len);
            if (so_err!=0) { close(sock); sock=-1; continue; }
        }
        fcntl(sock, F_SETFL, flags); // blocking again
        break;
    }
    freeaddrinfo(res);
    if (sock < 0) return "";

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: cheat-client/1.0\r\nConnection: close\r\n\r\n";
    if (send(sock, req.c_str(), req.size(), 0) <= 0) { close(sock); return ""; }

    std::string resp;
    char buf[1024];
    time_t start = time(nullptr);
    while (time(nullptr) - start < timeout_sec) {
        ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n]='\0';
        resp += buf;
        if (resp.size() > 16384) break;
    }
    close(sock);

    // Find body after \r\n\r\n
    size_t hdr_end = resp.find("\r\n\r\n");
    if (hdr_end != std::string::npos) {
        return resp.substr(hdr_end+4);
    }
    return resp;
}

std::string Get(const std::string& url, int timeout_sec) {
    if (url.empty()) return "";

    // 1. Try curl (supports https)
    std::string out = try_curl(url, timeout_sec);
    if (!out.empty()) return out;

    // 2. Try wget
    out = try_wget(url, timeout_sec);
    if (!out.empty()) return out;

    // 3. Try raw http (only if http://)
    if (url.rfind("http://", 0) == 0) {
        out = try_raw_http(url, timeout_sec);
        if (!out.empty()) return out;
    }

    // 4. For https, try raw http via http fallback? Try converting https to http if server supports both
    // Some Render/Cloudflare support http->https redirect, but raw http to https port fails.
    // Last attempt: if url is https, try http version (some hosts allow)
    if (url.rfind("https://", 0) == 0) {
        std::string http_url = "http://" + url.substr(8);
        out = try_raw_http(http_url, timeout_sec);
        if (!out.empty()) return out;
    }

    return "";
}

int ParseEnabled(const std::string& json) {
    if (json.empty()) return -1;
    // Very simple JSON parse: look for "enabled": true/false
    // Case-insensitive search
    std::string lower;
    lower.reserve(json.size());
    for (char c : json) lower.push_back(tolower(c));
    
    size_t pos = lower.find("\"enabled\"");
    if (pos == std::string::npos) pos = lower.find("enabled");
    if (pos == std::string::npos) return -1;
    
    size_t colon = lower.find(':', pos);
    if (colon == std::string::npos) return -1;
    
    // Skip spaces
    size_t i = colon+1;
    while (i < lower.size() && (lower[i]==' '||lower[i]=='\t'||lower[i]=='\r'||lower[i]=='\n')) i++;
    if (i+4 <= lower.size()) {
        if (lower.compare(i, 4, "true")==0) return 1;
        if (lower.compare(i, 5, "false")==0) return 0;
    }
    // Also check 1/0
    if (i < lower.size()) {
        if (lower[i]=='1') return 1;
        if (lower[i]=='0') return 0;
    }
    return -1;
}

} // namespace http
} // namespace prot
