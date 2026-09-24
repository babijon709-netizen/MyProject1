#include "prot.h"
#include "http_client.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <signal.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "PROT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) printf("[PROT] " __VA_ARGS__); printf("\n")
#define LOGE(...) printf("[PROT-ERR] " __VA_ARGS__); printf("\n")
#endif

namespace prot {

std::atomic<bool> g_prot_enabled{true};
std::atomic<bool> g_prot_checked{false};
std::string g_prot_last_message = "Not checked yet";
static Status g_last_status;
static std::string g_api_url = PROT_API_URL;
static std::string g_api_url_backup = PROT_API_URL_BACKUP;

// Parse full JSON response into Status
static Status ParseResponse(const std::string& body) {
    Status s;
    s.valid = false;
    
    int en = http::ParseEnabled(body);
    if (en == -1) {
        // Try to see if body contains "disabled" message
        if (body.find("disabled") != std::string::npos || body.find("Disabled") != std::string::npos) {
            s.enabled = false;
            s.valid = true;
            s.message = "Disabled keyword found";
            return s;
        }
        return s;
    }
    s.enabled = (en == 1);
    s.valid = true;

    // Try to extract message
    size_t pos = body.find("\"message\"");
    if (pos != std::string::npos) {
        size_t colon = body.find(':', pos);
        if (colon != std::string::npos) {
            size_t q1 = body.find('"', colon+1);
            if (q1 != std::string::npos) {
                size_t q2 = body.find('"', q1+1);
                if (q2 != std::string::npos && q2 > q1) {
                    s.message = body.substr(q1+1, q2-q1-1);
                }
            }
        }
    }

    // version
    pos = body.find("\"version\"");
    if (pos != std::string::npos) {
        size_t colon = body.find(':', pos);
        if (colon != std::string::npos) {
            size_t q1 = body.find('"', colon+1);
            size_t q2 = (q1==std::string::npos)? std::string::npos : body.find('"', q1+1);
            if (q1!=std::string::npos && q2!=std::string::npos) s.version = body.substr(q1+1, q2-q1-1);
        }
    }

    // min_version
    pos = body.find("\"min_version\"");
    if (pos != std::string::npos) {
        size_t colon = body.find(':', pos);
        if (colon != std::string::npos) {
            size_t q1 = body.find('"', colon+1);
            size_t q2 = (q1==std::string::npos)? std::string::npos : body.find('"', q1+1);
            if (q1!=std::string::npos && q2!=std::string::npos) s.min_version = body.substr(q1+1, q2-q1-1);
        }
    }

    // maintenance
    pos = body.find("\"maintenance\"");
    if (pos != std::string::npos) {
        size_t colon = body.find(':', pos);
        if (colon != std::string::npos) {
            std::string sub = body.substr(colon, 20);
            if (sub.find("true") != std::string::npos) s.maintenance = true;
        }
    }

    // timestamp
    pos = body.find("\"timestamp\"");
    if (pos != std::string::npos) {
        size_t colon = body.find(':', pos);
        if (colon != std::string::npos) {
            s.timestamp = atoll(body.c_str() + colon + 1);
        }
    }

    return s;
}

Status CheckServer() {
    Status result;
    result.enabled = true;
    result.valid = false;

    // Build URL with HWID and version
    std::string hwid = http::GetHWID();
    std::string url = g_api_url;
    
    // If URL is placeholder, allow cheat (for dev)
    if (url.find("YOURNAME") != std::string::npos || url.find("CHANGE_ME") != std::string::npos || url.empty()) {
        LOGI("PROT: API URL not configured (%s), allowing", url.c_str());
        result.enabled = true;
        result.valid = true;
        result.message = "URL not configured - dev mode";
        g_last_status = result;
        g_prot_enabled = true;
        g_prot_checked = true;
        return result;
    }

    // Append query params if using /api/check or /api/status (both support)
    // Convert /api/status to /api/check?hwid=...
    std::string checkUrl = url;
    if (checkUrl.find("/api/status") != std::string::npos) {
        checkUrl = checkUrl.replace(checkUrl.find("/api/status"), 11, "/api/check");
    }
    // Add params
    if (checkUrl.find('?') == std::string::npos) checkUrl += "?";
    else checkUrl += "&";
    checkUrl += "hwid=" + http::UrlEncode(hwid) + "&version=" + http::UrlEncode(PROT_CHEAT_VERSION);

    LOGI("PROT: Checking %s", checkUrl.c_str());
    std::string body = http::Get(checkUrl, 8); // 8 sec timeout (Render cold start)

    if (body.empty() && !g_api_url_backup.empty()) {
        LOGI("PROT: Primary failed, trying backup %s", g_api_url_backup.c_str());
        std::string backup = g_api_url_backup;
        if (backup.find("/api/status") != std::string::npos) {
            backup = backup.replace(backup.find("/api/status"), 11, "/api/check");
        }
        if (backup.find('?') == std::string::npos) backup += "?";
        else backup += "&";
        backup += "hwid=" + http::UrlEncode(hwid) + "&version=" + http::UrlEncode(PROT_CHEAT_VERSION);
        body = http::Get(backup, 8);
    }

    // If still empty and original was /api/check, try /api/status (some servers only have status)
    if (body.empty()) {
        LOGI("PROT: Check endpoint failed, trying status endpoint");
        body = http::Get(g_api_url, 8);
    }

    if (body.empty()) {
        LOGE("PROT: Server unreachable, body empty");
        result.valid = false;
        result.message = "Server unreachable";
        // Fail open or closed based on config
#if PROT_FAIL_CLOSED
        result.enabled = false;
        result.message += " - FAIL CLOSED, disabling";
#else
        result.enabled = true;
        result.message += " - FAIL OPEN, allowing (change PROT_FAIL_CLOSED to 1 for secure)";
#endif
        g_last_status = result;
        g_prot_enabled = result.enabled;
        g_prot_checked = true;
        g_prot_last_message = result.message;
        return result;
    }

    LOGI("PROT: Response: %s", body.substr(0, 500).c_str());
    Status parsed = ParseResponse(body);
    if (!parsed.valid) {
        LOGE("PROT: Failed to parse response");
        parsed.valid = false;
        parsed.message = "Parse failed, raw: " + body.substr(0, 200);
#if PROT_FAIL_CLOSED
        parsed.enabled = false;
#else
        parsed.enabled = true;
#endif
    }

    g_last_status = parsed;
    g_prot_enabled = parsed.enabled;
    g_prot_checked = true;
    g_prot_last_message = parsed.message;

    // Version check
    if (parsed.valid && !parsed.min_version.empty() && parsed.min_version != PROT_CHEAT_VERSION) {
        // Simple string compare, you can implement semver
        LOGI("PROT: Version check - server min=%s local=%s", parsed.min_version.c_str(), PROT_CHEAT_VERSION);
        // If you want to force update when min_version > current, do it here
        // For now just log, but you can set enabled=false if you want
    }

    return parsed;
}

void KillCheat(const std::string& reason) {
    LOGE("PROT: KILLING CHEAT: %s", reason.c_str());
    g_prot_enabled = false;
    g_prot_last_message = reason;

    // Try to show message via android log and also kill main thread
    // The main loop checks g_prot_enabled? We'll need to integrate in main.cpp
    // For now, just exit with message

    // Sleep a bit to allow log to flush
    // We will try to create a visible file with reason for debugging
    FILE* f = fopen("/data/local/tmp/cheat_disabled.txt", "w");
    if (f) {
        fprintf(f, "Cheat disabled: %s\nTime: %ld\n", reason.c_str(), time(nullptr));
        fclose(f);
    }

    // If called from Init, we can exit process
    // If from background thread, set flag and let main thread exit
    // Use _exit to avoid cleanup that could hang
    // But first try graceful
    LOGE("PROT: Cheat disabled by admin - %s", reason.c_str());
    
    // Give 2 seconds for user to see log, then kill
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Kill - will be caught by signal handler in main.cpp that sets main_thread_flag=false
    // So we just exit
    // For background checker, we don't exit immediately, we set flag
    // The main.cpp loop should check g_prot_enabled

    // If this is main thread (Init), exit
    // Check if we're in main thread by checking if main_thread_flag exists? Simpler: always exit if called from Init
    // We'll do _exit(1) to ensure
    _exit(1);
}

void StartBackgroundChecker() {
    static bool started = false;
    if (started) return;
    started = true;

    std::thread([]() {
        LOGI("PROT: Background checker started, interval %d sec", PROT_CHECK_INTERVAL_SEC);
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(PROT_CHECK_INTERVAL_SEC));
            
            Status s = CheckServer();
            LOGI("PROT: Background check - enabled=%d valid=%d msg=%s", s.enabled, s.valid, s.message.c_str());

            if (s.valid && !s.enabled) {
                // Server says disabled
                std::string reason = "Disabled by admin: " + s.message;
                g_prot_last_message = reason;
                g_prot_enabled = false;
                LOGE("PROT: Background kill triggered: %s", reason.c_str());
                // Write file
                FILE* f = fopen("/data/local/tmp/cheat_disabled.txt", "w");
                if (f) {
                    fprintf(f, "Cheat disabled in background: %s\nTime: %ld\n", reason.c_str(), time(nullptr));
                    fclose(f);
                }
                // Exit - main loop should check g_prot_enabled
                // Force kill after 3 sec if main loop doesn't exit
                std::this_thread::sleep_for(std::chrono::seconds(3));
                _exit(1);
            }
        }
    }).detach();
}

static std::string LoadUrlFromFile() {
    // Для тестов можно положить URL в файл без перекомпиляции
    // echo "https://your-worker.workers.dev/api/status" > /data/local/tmp/cheat_api_url.txt
    const char* paths[] = {
        "/data/local/tmp/cheat_api_url.txt",
        "/sdcard/cheat_api_url.txt",
        "/data/local/tmp/cheat_url.txt",
        nullptr
    };
    for (int i=0; paths[i]; ++i) {
        FILE* f = fopen(paths[i], "r");
        if (!f) continue;
        char buf[512]={0};
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            std::string s(buf);
            // trim
            s.erase(s.find_last_not_of(" \r\n\t")+1);
            s.erase(0, s.find_first_not_of(" \r\n\t"));
            if (s.rfind("http://",0)==0 || s.rfind("https://",0)==0) {
                LOGI("PROT: Loaded URL from file %s: %s", paths[i], s.c_str());
                return s;
            }
        } else fclose(f);
    }
    return "";
}

void Init() {
    // Попробовать загрузить URL из файла (для тестов без перекомпиляции)
    std::string fileUrl = LoadUrlFromFile();
    if (!fileUrl.empty()) {
        g_api_url = fileUrl;
    }
    LOGI("PROT: Init starting, URL=%s", g_api_url.c_str());
    
    Status s = CheckServer();
    
    if (!s.valid) {
        LOGI("PROT: Initial check invalid, message: %s, enabled=%d", s.message.c_str(), s.enabled);
        // Depending on PROT_FAIL_CLOSED
        if (!s.enabled) {
            KillCheat("Server unreachable and FAIL_CLOSED enabled: " + s.message);
        }
        // Else allow and start background checker which will retry
    } else {
        if (!s.enabled) {
            KillCheat("Cheat disabled by admin: " + s.message);
        } else {
            LOGI("PROT: Cheat enabled, message: %s", s.message.c_str());
        }
    }

    // Start background checker regardless
    StartBackgroundChecker();
}

Status GetLastStatus() {
    return g_last_status;
}

} // namespace prot
