#pragma once

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>

// Per-tick aim trace written to CSV. One file per active aim session, kept in
// the device Downloads folder so it survives the app and can be pulled over
// USB or opened from the file manager. Rows are written only while aim is
// active; a session closes after kIdleClose seconds without an active tick.
namespace aim_trace {

struct Record {
    bool  active = false;        // false = aim idle this tick, nothing is written
    const char* mode = "";       // "touch" | "memory"
    double wall = 0.0;           // monotonic seconds
    float dt = 0.f;
    int   haveCam = 0;
    float camYaw = 0.f, camPitch = 0.f;     // camera angles read this tick
    float accYaw = 0.f, accPitch = 0.f;     // memory: raw accumulator values
    float rawYaw = 0.f, rawPitch = 0.f;     // offset to the bone, before the filter
    float filtYaw = 0.f, filtPitch = 0.f;   // touch: offset after the target filter;
                                            // memory: filtered target minus camera
    float errYaw = 0.f, errPitch = 0.f;     // error the controller acts on
    float stepYaw = 0.f, stepPitch = 0.f;   // memory: deg written; touch: finger px
    float fingerX = 0.f, fingerY = 0.f;
    int   held = 0;              // 1 = controller held this tick
    int   reason = 0;            // hold reason: 0 zero step, 1 ack wait, 2 dead zone,
                                 // 3 jitter gate, 4 placement/hold frames, 5 on target,
                                 // 6 no target
    int   wrote = 0;             // 1 = a write or finger move happened
    int   lifted = 0;            // 1 = finger lifted and re-placed
    float flipDamp = 1.f;
    float acquire = 0.f;
    unsigned long long targetId = 0;
    float worldDist = -1.f;
};

inline const char* header() {
    return "wall,mode,dt,have_cam,cam_yaw,cam_pitch,acc_yaw,acc_pitch,"
           "raw_yaw,raw_pitch,filt_yaw,filt_pitch,err_yaw,err_pitch,"
           "step_yaw,step_pitch,finger_x,finger_y,held,reason,wrote,lifted,"
           "flip_damp,acquire,target_id,world_dist\n";
}

// Returns the number of characters written (excluding the terminator), or a
// negative value on error.
inline int format_row(char* buf, size_t n, const Record& r) {
    return std::snprintf(buf, n,
        "%.4f,%s,%.5f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
        "%.5f,%.5f,%.1f,%.1f,%d,%d,%d,%d,%.3f,%.3f,%llu,%.2f\n",
        r.wall, r.mode, r.dt, r.haveCam, r.camYaw, r.camPitch, r.accYaw, r.accPitch,
        r.rawYaw, r.rawPitch, r.filtYaw, r.filtPitch, r.errYaw, r.errPitch,
        r.stepYaw, r.stepPitch, r.fingerX, r.fingerY, r.held, r.reason, r.wrote, r.lifted,
        r.flipDamp, r.acquire, r.targetId, r.worldDist);
}

enum class Result { Idle, Written, OpenFailed };

class Writer {
public:
    static constexpr double kIdleClose = 1.0;          // s without an active tick
    static constexpr size_t kMaxBytes = 40u << 20;     // stop a file at 40 MB
    static constexpr int    kFlushEvery = 10;          // rows between fflush calls

    Writer() : dirs_{"/storage/emulated/0/Download/benzware", "/storage/emulated/0/benzware"} {}

    // Test hook: replace the candidate directories.
    void setDirs(const std::vector<std::string>& dirs) { dirs_ = dirs; }

    const std::string& path() const { return path_; }

    Result record(const Record& r) {
        if (!r.active) {
            if (f_ && r.wall - lastActive_ > kIdleClose) close();
            failedThisSession_ = false;
            return Result::Idle;
        }
        if (f_ && mode_ != r.mode) close();   // mode switch starts a new session
        if (!f_) {
            if (failedThisSession_) return Result::Idle;
            if (!open(r)) { failedThisSession_ = true; return Result::OpenFailed; }
        }
        lastActive_ = r.wall;
        char line[512];
        const int n = format_row(line, sizeof line, r);
        if (n <= 0) return Result::Written;
        std::fwrite(line, 1, size_t(n), f_);
        bytes_ += size_t(n);
        if (++sinceFlush_ >= kFlushEvery) { std::fflush(f_); sinceFlush_ = 0; }
        if (bytes_ >= kMaxBytes) close();
        return Result::Written;
    }

    void close() {
        if (!f_) return;
        std::fflush(f_);
        std::fclose(f_);
        f_ = nullptr;
        bytes_ = 0; sinceFlush_ = 0;
    }

    ~Writer() { close(); }

private:
    bool open(const Record& r) {
        char stamp[32] = {};
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
        localtime_r(&now, &tmv);
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
        for (const std::string& dir : dirs_) {
            mkdir(dir.c_str(), 0775);
            const std::string p = dir + "/aim_" + r.mode + "_" + stamp + ".csv";
            FILE* f = std::fopen(p.c_str(), "w");
            if (!f) continue;
            std::fputs(header(), f);
            std::fflush(f);
            f_ = f;
            path_ = p;
            mode_ = r.mode;
            bytes_ = std::strlen(header());
            sinceFlush_ = 0;
            lastActive_ = r.wall;
            return true;
        }
        return false;
    }

    std::vector<std::string> dirs_;
    std::string path_;
    std::string mode_;
    FILE* f_ = nullptr;
    size_t bytes_ = 0;
    int sinceFlush_ = 0;
    double lastActive_ = 0.0;
    bool failedThisSession_ = false;
};

inline Writer& writer() {
    static Writer w;
    return w;
}

}  // namespace aim_trace
