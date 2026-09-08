#include <stdio.h>
#include <stdlib.h>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <cmath>
#include <linux/input.h>
#include <linux/uinput.h>

#include "imgui.h"

#define maxE 5
#define maxF 10
#define UNGRAB 0
#define GRAB 1

bool other_touch;

static uint32_t orientation = 0;
static float screenHeight = 0, screenWidth = 0;

struct touchObj {
    bool isDown = false;
    int x = 0;
    int y = 0;
    int id = 0;
};

struct targ {
    int fdNum;
    float S2TX;
    float S2TY;
};

static struct {
    input_event downEvent[2]{{{}, EV_KEY, BTN_TOUCH,       1},
                             {{}, EV_KEY, BTN_TOOL_FINGER, 1}};
    input_event event[512]{0};
} input;

static targ targF[maxE];

static touchObj Finger[maxE][maxF];

static int fdNum = 0, origfd[maxE], nowfd;

static int devMaxX = 0, devMaxY = 0;

static float scale_x, scale_y;

static void Touch_UpdateScale() {
    if (devMaxX <= 0 || devMaxY <= 0) return;
    if (orientation == 1 || orientation == 3) {
        ::scale_x = (float) devMaxX / (::screenHeight > 0 ? ::screenHeight : 1);
        ::scale_y = (float) devMaxY / (::screenWidth  > 0 ? ::screenWidth  : 1);
    } else {
        ::scale_x = (float) devMaxX / (::screenWidth  > 0 ? ::screenWidth  : 1);
        ::scale_y = (float) devMaxY / (::screenHeight > 0 ? ::screenHeight : 1);
    }
}

static bool Touch_initialized = false;

static bool Touch_readOnly = false;

static bool checkDeviceIsTouch(int fd);
static void genRandomString(char *string, int length) {
    int flag, i;
    srand((unsigned) time(NULL) + length);
    for (i = 0; i < length - 1; i++) {
        flag = rand() % 3;
        switch (flag) {
            case 0:
                string[i] = 'A' + rand() % 26;
                break;
            case 1:
                string[i] = 'a' + rand() % 26;
                break;
            case 2:
                string[i] = '0' + rand() % 10;
                break;
            default:
                string[i] = 'x';
                break;
        }
    }
    string[length - 1] = '\0';
}

static void Upload() {
    static bool bTouch = false;
    static bool isFirstDown = true;
    while (bTouch);
    bTouch = true;
    int tmpCnt = 0, tmpCnt2 = 0, i, j;
    for (i = 0; i < fdNum; i++) {
        for (j = 0; j < maxF; j++) {
            if (Finger[i][j].isDown) {
                if (tmpCnt2++ > 10) {
                    goto finish;
                }
                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_X;
                input.event[tmpCnt].value = Finger[i][j].x;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_Y;
                input.event[tmpCnt].value = Finger[i][j].y;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_POSITION_X;
                input.event[tmpCnt].value = Finger[i][j].x;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_POSITION_Y;
                input.event[tmpCnt].value = Finger[i][j].y;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_TRACKING_ID;
                input.event[tmpCnt].value = Finger[i][j].id;
                tmpCnt++;

                input.event[tmpCnt].type = EV_SYN;
                input.event[tmpCnt].code = SYN_MT_REPORT;
                input.event[tmpCnt].value = 0;
                tmpCnt++;
            }
        }
    }
    finish:
    bool is = false;
    if (tmpCnt == 0) {
        input.event[tmpCnt].type = EV_SYN;
        input.event[tmpCnt].code = SYN_MT_REPORT;
        input.event[tmpCnt].value = 0;
        tmpCnt++;
        if (!isFirstDown) {
            isFirstDown = true;
            input.event[tmpCnt].type = EV_KEY;
            input.event[tmpCnt].code = BTN_TOUCH;
            input.event[tmpCnt].value = 0;
            tmpCnt++;
            input.event[tmpCnt].type = EV_KEY;
            input.event[tmpCnt].code = BTN_TOOL_FINGER;
            input.event[tmpCnt].value = 0;
            tmpCnt++;
        }
    } else {
        is = true;
    }
    input.event[tmpCnt].type = EV_SYN;
    input.event[tmpCnt].code = SYN_REPORT;
    input.event[tmpCnt].value = 0;
    tmpCnt++;

    if (is && isFirstDown) {
        isFirstDown = false;
        write(nowfd, &input, sizeof(struct input_event) * (tmpCnt + 2));
    } else {
        write(nowfd, input.event, sizeof(struct input_event) * tmpCnt);
    }

    bTouch = false;
}

static void *TypeA(void *arg) {
    targ tmp = *(targ *) arg;
    int i = tmp.fdNum;
    float S2TX = tmp.S2TX;
    float S2TY = tmp.S2TY;
    int latest = 0;
    input_event inputEvent[64]{0};

    while (Touch_initialized) {
        auto readSize = (int32_t) read(origfd[i], inputEvent, sizeof(inputEvent));
        if (readSize <= 0 || (readSize % sizeof(input_event)) != 0) {
            continue;
        }
        size_t count = size_t(readSize) / sizeof(input_event);
        for (size_t j = 0; j < count; j++) {
            input_event &ie = inputEvent[j];
            if (ie.type == EV_ABS) {
                if (ie.code == ABS_MT_SLOT) {
                    latest = ie.value;
                    continue;
                }
                if (ie.code == ABS_MT_TRACKING_ID) {
                    if (ie.value == -1) {
                        Finger[i][latest].isDown = false;
                    } else {
                        Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                        Finger[i][latest].isDown = true;
                    }
                    continue;
                }
                if (ie.code == ABS_MT_POSITION_X) {
                    Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                    Finger[i][latest].x = (int) (ie.value * S2TX);
                    continue;
                }
                if (ie.code == ABS_MT_POSITION_Y) {
                    Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                    Finger[i][latest].y = (int) (ie.value * S2TY);
                    continue;
                }
            }
            if (ie.code == SYN_REPORT) {
                ImGuiIO &io = ImGui::GetIO();
                if (Finger[i][latest].isDown) {
                    float x = Finger[i][latest].x, y = Finger[i][latest].y;
                    float xt = x / scale_x;
                    float yt = y / scale_y;

                    if (other_touch) {
                        switch (orientation) {
                            case 1:
                                x = xt;
                                y = yt;
                                break;
                            case 2:
                                y = yt;
                                x = screenHeight - xt;
                                break;
                            case 3:
                                x = screenHeight - xt;
                                y = screenWidth - yt;
                                break;
                            default:
                                y = xt;
                                x = screenHeight - yt;
                                break;
                        }
                    } else {
                        switch (orientation) {
                            case 1:
                                x = yt;
                                y = screenHeight - xt;
                                break;
                            case 2:
                                x = screenHeight - xt;
                                y = screenWidth - yt;
                                break;
                            case 3:
                                y = xt;
                                x = screenWidth - yt;
                                break;
                            default:
                                x = xt;
                                y = yt;
                                break;
                        }
                    }
                    io.MousePos = {x, y};
                    io.MouseDown[0] = true;
                } else {
                    io.MouseDown[0] = false;
                }
                if (!Touch_readOnly) {
                    Upload();
                }
                continue;
            }
        }
    }
    return nullptr;
}

bool Touch_Init(int w, int h, uint32_t orientation_, bool readOnly) {
    char temp[128];
    DIR *dir = opendir("/dev/input/");
    dirent *ptr = NULL;
    int eventCount = 0;
    while ((ptr = readdir(dir)) != NULL) {
        if (strstr(ptr->d_name, "event"))
            eventCount++;
    }
    struct input_absinfo abs, absX[maxE], absY[maxE];
    int fd, i, tmp1, tmp2;
    int screenX, screenY, minCnt = eventCount + 1;
    fdNum = 0;
    for (i = 0; i <= eventCount; i++) {
        sprintf(temp, "/dev/input/event%d", i);
        fd = open(temp, O_RDWR);
        if (fd < 0) {
            continue;
        }
        if (checkDeviceIsTouch(fd)) {
            tmp1 = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absX[fdNum]);
            tmp2 = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absY[fdNum]);
            if (tmp1 == 0 && tmp2 == 0) {
                origfd[fdNum] = fd;
                if (!readOnly) {
                    ioctl(fd, EVIOCGRAB, GRAB);
                }
                if (i < minCnt) {
                    screenX = absX[fdNum].maximum;
                    screenY = absY[fdNum].maximum;
                    minCnt = i;
                }
                fdNum++;
                if (fdNum >= maxE)
                    break;
            }
        } else {
            close(fd);
        }
    }

    if (minCnt > eventCount) {
        puts("Failed init touch!");
        return false;
    }

    if (!readOnly) {
        struct uinput_user_dev ui_dev;
        nowfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (nowfd <= 0) {
            for (int k = 0; k < fdNum; ++k) { ioctl(origfd[k], EVIOCGRAB, UNGRAB); close(origfd[k]); origfd[k] = 0; }
            fdNum = 0;
            return false;
        }

        int string_len = rand() % 10 + 5;
        char string[string_len];
        memset(&ui_dev, 0, sizeof(ui_dev));

        genRandomString(string, string_len);
        strncpy(ui_dev.name, string, UINPUT_MAX_NAME_SIZE);

        ui_dev.id.bustype = 0;
        ui_dev.id.vendor = rand() % 10 + 5;
        ui_dev.id.product = rand() % 10 + 5;
        ui_dev.id.version = rand() % 10 + 5;

        ioctl(nowfd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

        ioctl(nowfd, UI_SET_EVBIT, EV_ABS);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_X);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_Y);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
        ioctl(nowfd, UI_SET_EVBIT, EV_SYN);
        ioctl(nowfd, UI_SET_EVBIT, EV_KEY);
        ioctl(nowfd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
        ioctl(nowfd, UI_SET_KEYBIT, BTN_TOUCH);

        genRandomString(string, string_len);
        ioctl(nowfd, UI_SET_PHYS, string);

        sprintf(temp, "/dev/input/event%d", minCnt);
        fd = open(temp, O_RDWR);
        if (fd) {
            struct input_id id;
            if (!ioctl(fd, EVIOCGID, &id)) {
                ui_dev.id.bustype = id.bustype;
                ui_dev.id.vendor = id.vendor;
                ui_dev.id.product = id.product;
                ui_dev.id.version = id.version;
            }
            uint8_t *bits = NULL;
            ssize_t bits_size = 0;
            int res, j, k;
            while (1) {
                res = ioctl(fd, EVIOCGBIT(EV_KEY, bits_size), bits);
                if (res < bits_size)
                    break;
                bits_size = res + 16;
                bits = (uint8_t *) realloc(bits, bits_size * 2);
            }
            for (j = 0; j < res; j++) {
                for (k = 0; k < 8; k++)
                    if (bits[j] & 1 << k) {
                        if (j * 8 + k == BTN_TOUCH || j * 8 + k == BTN_TOOL_FINGER)
                            continue;
                        ioctl(nowfd, UI_SET_KEYBIT, j * 8 + k);
                    }
            }
            free(bits);
        }
        ui_dev.absmin[ABS_MT_POSITION_X] = 0;
        ui_dev.absmax[ABS_MT_POSITION_X] = screenX;
        ui_dev.absmin[ABS_MT_POSITION_Y] = 0;
        ui_dev.absmax[ABS_MT_POSITION_Y] = screenY;
        ui_dev.absmin[ABS_X] = 0;
        ui_dev.absmax[ABS_X] = screenX;
        ui_dev.absmin[ABS_Y] = 0;
        ui_dev.absmax[ABS_Y] = screenY;
        ui_dev.absmin[ABS_MT_TRACKING_ID] = 0;
        ui_dev.absmax[ABS_MT_TRACKING_ID] = 65535;
        write(nowfd, &ui_dev, sizeof(ui_dev));

        if (ioctl(nowfd, UI_DEV_CREATE)) {
            close(nowfd); nowfd = 0;
            for (int k = 0; k < fdNum; ++k) { ioctl(origfd[k], EVIOCGRAB, UNGRAB); close(origfd[k]); origfd[k] = 0; }
            fdNum = 0;
            return false;
        }
    }
    Touch_initialized = true;
    Touch_readOnly = readOnly;

    pthread_t t;
    for (i = 0; i < fdNum; i++) {
        targF[i].fdNum = i;
        targF[i].S2TX = (float) screenX / (float) absX[i].maximum;
        targF[i].S2TY = (float) screenY / (float) absY[i].maximum;
        pthread_create(&t, NULL, TypeA, &targF[i]);
    }

    ::screenWidth = w;
    ::screenHeight = h,
    ::orientation = orientation_;
    devMaxX = screenX;
    devMaxY = screenY;
    Touch_UpdateScale();

    system("chmod 000 -R /proc/bus/input/*");
    return true;
}
void UpdateScreenData(int w, int h, uint32_t orientation_) {
    ::screenWidth = w;
    ::screenHeight = h,
    ::orientation = orientation_;
    Touch_UpdateScale();
}

static bool checkDeviceIsTouch(int fd) {
    uint8_t *bits = NULL;
    ssize_t bits_size = 0;
    int res, j, k;
    bool itmp = false, itmp2 = false, itmp3 = false;
    struct input_absinfo abs{};
    while (true) {
        res = ioctl(fd, EVIOCGBIT(EV_ABS, bits_size), bits);
        if (res < bits_size)
            break;
        bits_size = res + 16;
        bits = (uint8_t *) realloc(bits, bits_size * 2);
    }
    for (j = 0; j < res; j++) {
        for (k = 0; k < 8; k++)
            if (bits[j] & 1 << k && ioctl(fd, EVIOCGABS(j * 8 + k), &abs) == 0) {
                if (j * 8 + k == ABS_MT_SLOT) {
                    itmp = true;
                    continue;
                }
                if (j * 8 + k == ABS_MT_POSITION_X) {
                    itmp2 = true;
                    continue;
                }
                if (j * 8 + k == ABS_MT_POSITION_Y) {
                    itmp3 = true;
                    continue;
                }
            }
    }
    free(bits);
    return itmp && itmp2 && itmp3;
}

void Touch_Close() {
    if (Touch_initialized) {
        for (int i = 0; i < maxE; ++i) {
            if (origfd[i] > 0) {
                if (!Touch_readOnly)
                    ioctl(origfd[i], EVIOCGRAB, UNGRAB);
                close(origfd[i]);
                origfd[i] = 0;
            }
        }
        if (nowfd > 0) {
            ioctl(nowfd, UI_DEV_DESTROY);
            close(nowfd);
            nowfd = 0;
        }
        fdNum = 0;
        memset(Finger, 0, sizeof(Finger));
        memset(input.event, 0, sizeof(input.event));
        Touch_initialized = false;
    }
}

float Touch_DeviceUnitsPerPixel() {
    if (!Touch_initialized || devMaxX <= 0 || devMaxY <= 0) return 1.0f;
    float s = ::scale_x < ::scale_y ? ::scale_x : ::scale_y;
    return (s > 0.01f && s < 100.0f) ? s : 1.0f;
}

void Touch_Down(float xt, float yt) {
    if (!Touch_initialized || Touch_readOnly) return;
    // Keep sub-pixel precision all the way to the device grid: the touch
    // controller usually has finer resolution than the display, and the game
    // receives float coordinates. Truncating to whole screen pixels here would
    // make the smallest possible camera step larger than a head at range.
    float x = 0.0f, y = 0.0f;
    switch (orientation) {
        case 1: {
            x = ::screenHeight - yt;
            y = xt;
            break;
        }
        case 2: {
            x = ::screenHeight - xt;
            y = ::screenWidth - yt;
            break;
        }
        case 3: {
            x = yt;
            y = ::screenWidth - xt;
            break;
        }
        default: {
            x = xt;
            y = yt;
            break;
        }
    }
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    touchObj &touch = Finger[0][9];
    touch.id = 60000;
    touch.x = (int) lroundf(x * ::scale_x);
    touch.y = (int) lroundf(y * ::scale_y);
    if (devMaxX > 0 && touch.x > devMaxX) touch.x = devMaxX;
    if (devMaxY > 0 && touch.y > devMaxY) touch.y = devMaxY;
    touch.isDown = true;
    Upload();
}
void Touch_Move(float x, float y) {
    Touch_Down(x, y);
}

void Touch_Up() {
    if (!Touch_initialized || Touch_readOnly) return;
    touchObj &touch = Finger[0][9];
    touch.isDown = false;
    Upload();
}

// ---- Extra synthetic fingers (auto-farm) ------------------------------------
// Same coordinate pipeline as Touch_Down(), but in their own slots (6..8) so
// they coexist with the aimbot finger in slot 9 and with real fingers.
void Touch_Down_N(int finger, float xt, float yt) {
    if (!Touch_initialized || Touch_readOnly) return;
    if (finger < 0 || finger > 2) return;
    float x = 0.0f, y = 0.0f;
    switch (orientation) {
        case 1: { x = ::screenHeight - yt; y = xt; break; }
        case 2: { x = ::screenHeight - xt; y = ::screenWidth - yt; break; }
        case 3: { x = yt; y = ::screenWidth - xt; break; }
        default: { x = xt; y = yt; break; }
    }
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    touchObj &touch = Finger[0][6 + finger];
    touch.id = 61000 + finger;
    touch.x = (int) lroundf(x * ::scale_x);
    touch.y = (int) lroundf(y * ::scale_y);
    if (devMaxX > 0 && touch.x > devMaxX) touch.x = devMaxX;
    if (devMaxY > 0 && touch.y > devMaxY) touch.y = devMaxY;
    touch.isDown = true;
    Upload();
}

void Touch_Up_N(int finger) {
    if (!Touch_initialized || Touch_readOnly) return;
    if (finger < 0 || finger > 2) return;
    touchObj &touch = Finger[0][6 + finger];
    touch.isDown = false;
    Upload();
}

bool Touch_CanInject() {
    return Touch_initialized && !Touch_readOnly;
}
